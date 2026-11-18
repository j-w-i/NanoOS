/* Triangle-list -> triangle-strip conversion for static element buffers.
 *
 * Every converted index costs one full vertex-program run plus its FIFO words,
 * and indexed lists are fed as 3 vertices per triangle. A strip shares two
 * vertices with the previous triangle, so a connected mesh costs close to one
 * vertex per triangle. The same triangles are drawn with the same winding;
 * only their order inside the draw changes, so conversion is limited to
 * opaque draws that test and write depth (order can then only matter for
 * exactly equal depths). Strips are joined with repeated indices; the joining
 * triangles have zero area. Results (including "not worth it") are cached
 * per (buffer, generation, offset, count, type); a buffer upload bumps its
 * generation. A full pool clears the cache.
 *
 * GL2_STRIP_POOL_WORDS 0 disables the feature. */
#ifndef GL2_STRIP_POOL_WORDS
#define GL2_STRIP_POOL_WORDS 65536u      /* uint16 indices: 128 KiB */
#endif
#ifndef GL2_STRIP_MAX_TRIS
#define GL2_STRIP_MAX_TRIS 4096u
#endif
#define GL2_STRIP_SLOTS 1024u
#define GL2_STRIP_EDGE_SLOTS 16384u      /* power of two, > 3 * GL2_STRIP_MAX_TRIS */
#if GL2_STRIP_POOL_WORDS
typedef struct { const gl2_ctx *ctx; uint32_t name, gen, off, count, type, start, nprim, nidx; uint32_t vc_off[2], vc_sig[2], vc_epoch[2]; uint8_t vc_next; } gl2_strip_entry;   /* two cached streams: multipass engines draw one mesh with two programs (e.g. shadow casters) */
static gl2_strip_entry strip_cache[GL2_STRIP_SLOTS];
static uint16_t strip_pool[GL2_STRIP_POOL_WORDS];
static uint32_t strip_used, strip_entries;
static uint32_t strip_ekey[GL2_STRIP_EDGE_SLOTS];
static uint16_t strip_etri[GL2_STRIP_EDGE_SLOTS];
static uint16_t strip_tri[GL2_STRIP_MAX_TRIS][3];
static uint8_t strip_mark[GL2_STRIP_MAX_TRIS];          /* 0 free, 1 committed, 2 tentative */
static uint16_t strip_touch[GL2_STRIP_MAX_TRIS];
static uint16_t strip_tmp[GL2_STRIP_MAX_TRIS + 2];
static uint16_t strip_tmp2[3u * GL2_STRIP_MAX_TRIS];
static uint32_t strip_hash(uint32_t k) { k *= 2654435761u; return (k ^ (k >> 15)) & (GL2_STRIP_EDGE_SLOTS - 1u); }
static void strip_edge_add(uint32_t a, uint32_t b, uint32_t t) {
    uint32_t key = (a << 16 | b) + 1u, h = strip_hash(key);
    while (strip_ekey[h]) h = (h + 1u) & (GL2_STRIP_EDGE_SLOTS - 1u);
    strip_ekey[h] = key; strip_etri[h] = (uint16_t)t;
}
/* A free triangle with directed edge a->b; its third vertex in *x, or -1. */
static int strip_find(uint32_t a, uint32_t b, uint32_t *x) {
    uint32_t key = (a << 16 | b) + 1u, h = strip_hash(key);
    for (; strip_ekey[h]; h = (h + 1u) & (GL2_STRIP_EDGE_SLOTS - 1u)) {
        if (strip_ekey[h] != key) continue;
        uint32_t t = strip_etri[h];
        if (strip_mark[t]) continue;
        const uint16_t *v = strip_tri[t];
        for (unsigned r = 0; r < 3; ++r)
            if (v[r] == a && v[(r + 1) % 3] == b) { *x = v[(r + 2) % 3]; return (int)t; }
    }
    return -1;
}
/* Grow a strip from triangle t rotated by r, marking its triangles with mark.
 * Triangle k of a strip is (s[k],s[k+1],s[k+2]) for even k and
 * (s[k+1],s[k],s[k+2]) for odd k, so each keeps its source winding.
 * Returns the index count; touched triangles are listed in strip_touch. */
static uint32_t strip_grow(uint32_t t, unsigned r, uint8_t mark, uint16_t *s, uint32_t *ntouch) {
    const uint16_t *v = strip_tri[t];
    uint32_t n = 3; s[0] = v[r]; s[1] = v[(r + 1) % 3]; s[2] = v[(r + 2) % 3];
    strip_mark[t] = mark; strip_touch[0] = (uint16_t)t; *ntouch = 1;
    for (;;) {
        uint32_t p = s[n - 2], q = s[n - 1], x = 0, k = n - 2;
        int u = (k & 1u) ? strip_find(q, p, &x) : strip_find(p, q, &x);
        if (u < 0 || n >= GL2_STRIP_MAX_TRIS + 2u) return n;
        strip_mark[u] = mark; strip_touch[(*ntouch)++] = (uint16_t)u; s[n++] = (uint16_t)x;
    }
}
/* Convert count list indices to strips stored in strip_pool as
 * [nprim lengths][indices]. Each strip is drawn as its own primitive packet
 * (two header words after the cached state), so no joining vertices are
 * needed: a lone quad costs 4 vertex runs instead of 6. Returns the number of
 * strips (0: keep the list: fewer than ~10% vertex-equivalents saved, or no
 * room), *nidx the index count and *start the pool offset. */
#ifndef GL2_STRIP_JOIN
#define GL2_STRIP_JOIN 1   /* all strips of a draw in one primitive, joined by degenerate triangles */
#endif
#ifndef GL2_PRIM_COST_VERTS
#define GL2_PRIM_COST_VERTS 5u   /* per-packet cost in vertex runs (run513: ~4 us packet vs ~0.9 us vertex) */
#endif
static uint32_t stripify(const uint8_t *src, uint32_t type, uint32_t count, uint32_t *start, uint32_t *nidx) {
    uint32_t ntri = count / 3u;
    if (count % 3u || ntri < 2u || ntri > GL2_STRIP_MAX_TRIS) return 0;
    for (uint32_t i = 0; i < GL2_STRIP_EDGE_SLOTS; ++i) strip_ekey[i] = 0;
    uint32_t real = 0;
    for (uint32_t t = 0; t < ntri; ++t) {
        uint32_t v[3];
        for (unsigned k = 0; k < 3; ++k) v[k] = draw_index(src, type, 0, t * 3u + k);
        if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue;   /* zero-area source triangle: draws nothing */
        strip_tri[real][0] = (uint16_t)v[0]; strip_tri[real][1] = (uint16_t)v[1]; strip_tri[real][2] = (uint16_t)v[2];
        strip_edge_add(v[0], v[1], real); strip_edge_add(v[1], v[2], real); strip_edge_add(v[2], v[0], real);
        strip_mark[real++] = 0;
    }
    if (!real) return 0;
    /* Pass 1: count strips and indices (marks committed), pass 2 is avoided by
     * writing indices to the pool tail and lengths to strip_touch-sized scratch. */
    static uint16_t lens[GL2_STRIP_MAX_TRIS];
    uint32_t base = strip_used, nprim = 0, n_idx = 0, budget = count - count / 10u;
    for (uint32_t t = 0; t < real; ++t) {
        if (strip_mark[t]) continue;
        unsigned best = 0; uint32_t bestn = 0, nt = 0;
        for (unsigned r = 0; r < 3; ++r) {                    /* try each starting edge, keep the longest */
            uint32_t n = strip_grow(t, r, 2u, strip_tmp, &nt);
            for (uint32_t i = 0; i < nt; ++i) strip_mark[strip_touch[i]] = 0;
            if (n > bestn) { bestn = n; best = r; }
        }
        uint32_t n = strip_grow(t, best, 1u, strip_tmp, &nt);
#if GL2_STRIP_JOIN
        if (nprim) {
            /* join to the previous strip in the same primitive: repeat its last index and this
             * strip's first (zero-area triangles), plus one more when needed so this strip's
             * first triangle lands on an even position and keeps its winding */
            uint32_t j = (n_idx & 1u) ? 3u : 2u;
            if (n_idx + j + n + GL2_PRIM_COST_VERTS > budget) return 0;
            uint16_t last = strip_tmp2[n_idx - 1];
            strip_tmp2[n_idx++] = last; if (j == 3u) strip_tmp2[n_idx++] = last;
            strip_tmp2[n_idx++] = strip_tmp[0];
            for (uint32_t i = 0; i < n; ++i) strip_tmp2[n_idx + i] = strip_tmp[i];
            n_idx += n; lens[0] = (uint16_t)n_idx; continue;
        }
#endif
        if (n_idx + n + (nprim + 1u) * GL2_PRIM_COST_VERTS > budget) return 0;
        if (base + real + n_idx + n > GL2_STRIP_POOL_WORDS) return 0;   /* lengths need <= real slots */
        for (uint32_t i = 0; i < n; ++i) strip_tmp2[n_idx + i] = strip_tmp[i];
        lens[nprim++] = (uint16_t)n; n_idx += n;
    }
    if (base + nprim + n_idx > GL2_STRIP_POOL_WORDS) return 0;
    for (uint32_t i = 0; i < nprim; ++i) strip_pool[base + i] = lens[i];
    for (uint32_t i = 0; i < n_idx; ++i) strip_pool[base + nprim + i] = strip_tmp2[i];
    *start = base; *nidx = n_idx;
    strip_used = base + nprim + n_idx;
    return nprim;
}
static gl2_strip_entry *strip_slot(const gl2_ctx *c, uint32_t name, uint32_t gen, uint32_t off, uint32_t count, uint32_t type, int *found) {
    uint32_t h = (name * 2654435761u ^ off * 40503u ^ count * 97u ^ gen * 7919u) & (GL2_STRIP_SLOTS - 1u);
    for (unsigned i = 0; i < 16u; ++i, h = (h + 1u) & (GL2_STRIP_SLOTS - 1u)) {
        gl2_strip_entry *e = &strip_cache[h];
        if (!e->ctx) { *found = 0; return e; }
        if (e->ctx == c && e->name == name && e->gen == gen && e->off == off && e->count == count && e->type == type) { *found = 1; return e; }
    }
    *found = 0; return 0;
}
static void strip_reset(void) {
    for (uint32_t i = 0; i < GL2_STRIP_SLOTS; ++i) strip_cache[i].ctx = 0;
    strip_used = strip_entries = 0;
}
/* Strips for a list draw from a static element buffer: returns the length
 * table (nprim entries, indices follow it), or NULL to draw the list. */
static const uint16_t *strip_lookup(gl2_ctx *c, const gl2_buffer *b, uint32_t off, uint32_t count, uint32_t type, const uint8_t *src, uint32_t *nprim, uint32_t *nidx, gl2_strip_entry **entry) {
    if (b->usage != GL_STATIC_DRAW) return 0;
    int found = 0;
    gl2_strip_entry *e = strip_slot(c, b->name, b->gen, off, count, type, &found);
    if (!e || (!found && (strip_entries >= GL2_STRIP_SLOTS / 2u || strip_used + count + count / 3u > GL2_STRIP_POOL_WORDS))) {
        strip_reset();
        e = strip_slot(c, b->name, b->gen, off, count, type, &found);
        if (!e) return 0;
    }
    if (!found) {
        uint32_t start = 0, ni = 0, np = stripify(src, type, count, &start, &ni);
        e->ctx = c; e->name = b->name; e->gen = b->gen; e->off = off; e->count = count; e->type = type;
        e->start = start; e->nprim = np; e->nidx = ni; e->vc_epoch[0] = e->vc_epoch[1] = 0; e->vc_next = 0; ++strip_entries;
    }
    *entry = e;                         /* list draws keep the entry for the vertex cache */
    if (!e->nprim) return 0;
    *nprim = e->nprim; *nidx = e->nidx;
    return strip_pool + e->start;
}
#endif
