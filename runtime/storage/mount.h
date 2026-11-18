#ifndef NS_MOUNT_H
#define NS_MOUNT_H
#include "disk.h"
enum { NS_IO=-7, NS_INCOMPLETE=-8, NS_TIMEOUT=-9 };
/* inspect: 0 normal bytes, -2 transport-complete array-error bytes, other
 * negative values mean no usable bytes. context follows nand.h count=4. */
struct ns_mount_io {
 void *opaque;
 ns_page_read inspect;
 int (*context)(void *,unsigned block,unsigned page,unsigned view,unsigned count,void *,uint32_t *);
 uint32_t (*usec)(void *);
 /* Optional fast read (nand_read_ppn_direct): DMA straight into data (32-byte aligned; NULL =
  * records only), count = sectors 1..4 from column zero, later records 0xffffffff. Same
  * 0/-2/negative contract as inspect. When set, mount and ns_mount_page_read use it. */
 int (*direct)(void *,uint32_t ce,uint32_t row,unsigned count,void *data,uint32_t *record16);
 /* Sectors survey() transfers per first page (0 = 4). 1 reads only sector 0 and its record:
  * a quarter of the bus time. Needs direct. */
 uint32_t survey_sectors;
};
struct ns_root {uint32_t block,gen,key;};
struct ns_mount {
 struct ns_sftl map;
 struct ns_mount_io io;
 uint32_t *page,*record;
 uint8_t *mask,*states;
 uint32_t *first_gen,*first_key;
 uint32_t blocks,root,open_block,context_block,context_gen;
 uint32_t reads,erased,errors,replay_pages,roots,tree_nodes,stage,detail;
 uint32_t start,budget,ready_block;
 uint32_t base_head[16],root_state_words[2],hint_state_words[2],open_count;
 struct ns_root candidate[8];
 /* Recovery is opt-in and always produces a read-only result if a page is
  * dropped. Only one final, incomplete page in a checkpoint-unreferenced
  * READY block may be skipped; no later programmed page may follow it. */
 uint32_t recovery_enabled,array_error,recovered_pages,quarantine_block,bad_ce,bad_row;
 uint8_t deferred[2088];
 uint32_t stage_us[6]; /* time spent in context, survey, load_root, replay, (unused), total */
};
/* Fixed N31 geometry:2088physical blocks,2084SFTL blocks; the reserved tail
 * is accepted only with0x08240824 in both context-body copies.
 * Workspace sizes: mask/states2088bytes each,
 * first_gen/key2088words each, data4096words, records16words.
 * run/update workspace capacities are explicit and never silently truncated. */
int ns_mount_n31(struct ns_mount *,const struct ns_mount_io *,uint8_t *mask,
 uint8_t *states,uint32_t *first_gen,uint32_t *first_key,
 uint32_t *data,uint32_t *record,struct ns_run *,uint32_t run_capacity,
 struct ns_update *,uint32_t update_capacity,uint32_t budget_us);
/* Conservative read-only recovery attempt. Transport failures and errors in
 * checkpoint-referenced/non-READY blocks remain fatal. A successful recovery
 * sets map.read_only, which blocks driver mutation planning. Callers must
 * surface recovery status; no program/erase is authorized. */
int ns_mount_n31_recover(struct ns_mount *,const struct ns_mount_io *,uint8_t *mask,
 uint8_t *states,uint32_t *first_gen,uint32_t *first_key,uint32_t *data,uint32_t *record,
 struct ns_run *,uint32_t run_capacity,struct ns_update *,uint32_t update_capacity,uint32_t budget_us);
int ns_mount_page_read(void *,uint32_t ce,uint32_t row,void *,uint32_t *);
#endif
