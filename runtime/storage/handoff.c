#include "handoff.h"
#define BLOCKS 2088u
static uint32_t mix(const uint32_t *p,uint32_t words){
 uint32_t s=0x9e3779b9u;
 for(uint32_t i=0;i<words;++i){s=(s<<5|s>>27)^p[i];s*=0x01000193u;}
 return s;
}
static void copy(void *d,const void *s,uint32_t n){
 uint32_t *dw=d;const uint32_t *sw=s;for(uint32_t i=0;i<n/4u;++i)dw[i]=sw[i];
}
static uint32_t total(uint32_t nruns,uint32_t nupdates){
 return (uint32_t)sizeof(struct ns_handoff)+(uint32_t)sizeof(struct ns_mount)+2u*BLOCKS+8u*BLOCKS+
        nruns*(uint32_t)sizeof(struct ns_run)+nupdates*(uint32_t)sizeof(struct ns_update);
}
void ns_handoff_clear(void *dst){if(dst)((volatile uint32_t *)dst)[0]=0;}
int ns_handoff_save(void *dst,uint32_t cap,const struct ns_mount *m,int strict_rc,uint32_t mount_us,uint32_t flags){
 if(!dst||((uintptr_t)dst&3u)||!m||!m->map.ready||!m->mask||!m->states||!m->first_gen||!m->first_key)return NS_ARGUMENT;
 ns_handoff_clear(dst);
 uint32_t nr=m->map.nruns,nu=m->map.nupdates,bytes=total(nr,nu);
 if(bytes>cap)return NS_SPACE;
 struct ns_handoff *h=dst;unsigned char *p=(unsigned char *)(h+1);
 copy(p,m,sizeof *m);p+=sizeof *m;
 copy(p,m->mask,BLOCKS);copy(p+BLOCKS,m->states,BLOCKS);p+=2u*BLOCKS;
 copy(p,m->first_gen,4u*BLOCKS);p+=4u*BLOCKS;copy(p,m->first_key,4u*BLOCKS);p+=4u*BLOCKS;
 copy(p,m->map.runs,nr*(uint32_t)sizeof(struct ns_run));p+=nr*sizeof(struct ns_run);
 copy(p,m->map.updates,nu*(uint32_t)sizeof(struct ns_update));
 *h=(struct ns_handoff){0,NS_HANDOFF_VERSION,bytes,0,(uint32_t)sizeof *m,nr,nu,strict_rc,mount_us,flags,{0}};
 h->sum=mix((const uint32_t *)h+4,(bytes-16u)/4u);
 __asm__ volatile("" ::: "memory");
 h->magic=NS_HANDOFF_MAGIC;
 return NS_OK;
}
int ns_handoff_load(const void *src,uint32_t cap,struct ns_mount *m,const struct ns_mount_io *io,
 uint8_t *mask,uint8_t *states,uint32_t *fg,uint32_t *fk,uint32_t *data,uint32_t *record,
 struct ns_run *runs,uint32_t nr,struct ns_update *updates,uint32_t nu,int *strict_rc,const struct ns_handoff **out){
 if(out)*out=0;
 if(!src||((uintptr_t)src&3u)||!m||!io||!io->inspect||!io->context||!mask||!states||!fg||!fk||!data||!record||!runs||!updates)return NS_ARGUMENT;
 const struct ns_handoff *h=src;
 if(cap<sizeof *h||h->magic!=NS_HANDOFF_MAGIC||h->version!=NS_HANDOFF_VERSION||h->mount_size!=sizeof *m||
    h->bytes>cap||h->nruns>nr||h->nupdates>nu||h->bytes!=total(h->nruns,h->nupdates)||
    h->sum!=mix((const uint32_t *)h+4,(h->bytes-16u)/4u))return NS_NOT_READY;
 const unsigned char *p=(const unsigned char *)(h+1);
 copy(m,p,sizeof *m);p+=sizeof *m;
 if(!m->map.ready||m->map.nruns!=h->nruns||m->map.nupdates!=h->nupdates){*m=(struct ns_mount){0};return NS_NOT_READY;}
 copy(mask,p,BLOCKS);copy(states,p+BLOCKS,BLOCKS);p+=2u*BLOCKS;
 copy(fg,p,4u*BLOCKS);p+=4u*BLOCKS;copy(fk,p,4u*BLOCKS);p+=4u*BLOCKS;
 copy(runs,p,h->nruns*(uint32_t)sizeof(struct ns_run));p+=h->nruns*sizeof(struct ns_run);
 copy(updates,p,h->nupdates*(uint32_t)sizeof(struct ns_update));
 m->io=*io;m->page=data;m->record=record;m->mask=mask;m->states=states;m->first_gen=fg;m->first_key=fk;
 m->map.bank_mask=mask;m->map.runs=runs;m->map.updates=updates;m->map.run_capacity=nr;m->map.update_capacity=nu;
 m->map.order=0;m->map.order_n=0;
 if(strict_rc)*strict_rc=h->strict_rc;
 if(out)*out=h;
 return NS_OK;
}
