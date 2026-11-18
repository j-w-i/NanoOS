#ifndef NS_HANDOFF_H
#define NS_HANDOFF_H
#include "mount.h"
/* Mounted-volume handoff from the kernel to the game it loads.
 *
 * The kernel mounts the NAND volume to find and load game.bin. Instead of the game paying
 * for the same full SFTL mount again, the kernel serialises its finished struct ns_mount
 * (map, bank masks, block states, base runs, replay updates) here and the game adopts it.
 * Nothing writes the flash between the two, so the adopted map is the map a fresh mount
 * would build. The kernel clears the magic at every boot before it mounts, so a handoff is
 * never older than the running kernel; layout changes are caught by version + struct size.
 *
 * Fixed window: the top 256 KiB of the SDK's reserved volume window (0x0a400000..0x0a600000,
 * see sdk/src/nano_fs.c and nano_heap.c), clear of both mount workspaces and of the heap. */
#define NS_HANDOFF_ADDR  0x0a5c0000u
#define NS_HANDOFF_BYTES 0x00040000u
#define NS_HANDOFF_MAGIC 0x4648534eu   /* "NSHF" */
#define NS_HANDOFF_VERSION 1u
struct ns_handoff {
 uint32_t magic,version,bytes,sum;     /* sum covers everything after this word up to bytes */
 uint32_t mount_size,nruns,nupdates;int32_t strict_rc;
 uint32_t mount_us,flags,reserved[6];  /* kernel's mount time; flags: NS_HANDOFF_F_* */
};
enum { NS_HANDOFF_F_DIRECT=1u, NS_HANDOFF_F_RECOVERED=2u };
/* Serialise a successful mount into dst (cap bytes). NS_SPACE if it does not fit. */
int ns_handoff_save(void *dst,uint32_t cap,const struct ns_mount *,int strict_rc,uint32_t mount_us,uint32_t flags);
/* Validate and adopt a handoff into caller workspaces (same shapes as ns_mount_n31). All
 * pointers in *m are rebound to the arguments; io replaces the kernel's callbacks.
 * NS_NOT_READY when src holds no valid handoff (fall back to a normal mount). */
int ns_handoff_load(const void *src,uint32_t cap,struct ns_mount *m,const struct ns_mount_io *io,
 uint8_t *mask,uint8_t *states,uint32_t *first_gen,uint32_t *first_key,uint32_t *data,uint32_t *record,
 struct ns_run *runs,uint32_t run_capacity,struct ns_update *updates,uint32_t update_capacity,
 int *strict_rc,const struct ns_handoff **header);
void ns_handoff_clear(void *dst);
#endif
