#ifndef RDISK_DIRECT_H
#define RDISK_DIRECT_H
#include <stdint.h>
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include "rdisk_diag.h"

struct RDiskState;

/* ROMSAFE backend state: DevInit allocates this many bytes in RAM, then
 * rdisk_usb_state_init() returns a 128-byte-aligned state within the block. */
ULONG rdisk_usb_state_bytes(void);
struct RDiskState *rdisk_usb_state_init(APTR raw, struct ExecBase *sys);

int rdisk_usb_init(struct RDiskState *,struct Task *,ULONG);
void rdisk_usb_poll(struct RDiskState *);
void rdisk_usb_note_worker_wake(struct RDiskState *);
int rdisk_usb_wait_ready(struct RDiskState *);
void rdisk_usb_get_diag(struct RDiskState *,struct RDiskDiag *out);
int rdisk_usb_geometry(struct RDiskState *,uint32_t *,uint32_t *,uint32_t *);
int rdisk_usb_submit(struct RDiskState *,uint32_t,uint32_t,uint32_t,uint32_t,void *,uint32_t);
#endif
