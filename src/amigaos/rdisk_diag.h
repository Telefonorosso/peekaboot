#ifndef RDISK_DIAG_H
#define RDISK_DIAG_H
/* Diagnostic ABI shared by AmigaOS device and diagnostic client. ULONG is
 * 32-bit on m68k. Query works before the DWC2 backend has initialized. */
#include <exec/types.h>
#define RDISK_DIAG_COMMAND 0x7f01u
#define RDISK_DIAG_MAGIC 0x52444432UL /* RDD2 */
#define RDISK_DIAG_VERSION 3UL
/* stage is last completed checkpoint; failure is 0 while pending/healthy. */
#define RD_STAGE_NONE       0UL
#define RD_STAGE_WORKER     1UL
#define RD_STAGE_SIGNALS    2UL
#define RD_STAGE_POWER      3UL
#define RD_STAGE_ID         4UL
#define RD_STAGE_DISCONNECT 5UL
#define RD_STAGE_MODE       6UL
#define RD_STAGE_AHBIDLE    7UL
#define RD_STAGE_CORE_RESET 8UL
#define RD_STAGE_DEVICE     9UL
#define RD_STAGE_CORE_READY 10UL
#define RD_STAGE_IRQ_READY  11UL
#define RD_STAGE_USB_RESET  12UL
#define RD_STAGE_ENUM_DONE  13UL
#define RD_STAGE_CONFIGURED 14UL
#define RD_STAGE_IDENT      15UL
/* Failure code identifies operation, even if it has no USB register data. */
#define RD_FAIL_NONE          0UL
#define RD_FAIL_POWER         1UL
#define RD_FAIL_ID            2UL
#define RD_FAIL_AHBIDLE       3UL
#define RD_FAIL_CORE_RESET    4UL
#define RD_FAIL_HOST_MODE     5UL
#define RD_FAIL_SIGNAL       6UL

/* read_fail_precheck bits: set only if a READ was rejected before the
 * transaction could be started. */
#define RD_PRE_NO_HW       (1UL<<0)
#define RD_PRE_NO_CONFIG   (1UL<<1)
#define RD_PRE_NO_GEOMETRY (1UL<<2)
#define RD_PRE_TXN_ACTIVE  (1UL<<3)
#define RD_PRE_TOO_LARGE   (1UL<<4)
#define RD_PRE_NULL_BUFFER (1UL<<5)
struct RDiskDiag {
    ULONG magic,version,stage,failure;
    ULONG worker_ready,hw_ready,configured,blocks;
    ULONG snpsid,gintsts,gintmsk,gahbcfg,gusbcfg,dctl,dsts,grstctl;
    ULONG usb_resets,enum_dones,config_events;
    /* FIX3 appended fields: preserve the FIX2 prefix for ABI compatibility. */
    ULONG irq_calls,irq_claimed,irq_no_dwc_pending;
    ULONG vblanks,worker_wakes,usb_polls;
    ULONG ep0_setups,ep0_in_done,ep0_out_done;
    ULONG last_ep0_in,last_ep0_out,last_gintsts,last_masked;
    ULONG last_setup_a,last_setup_b;
    ULONG disconnects,suspends,wakeups,ep0_state;
    ULONG dcfg,daint,daintmsk,diepmsk,doepmsk;
    ULONG diepctl0,doepctl0,doeptsiz0;

    /* v3: first READ failure latch.  Zero until the first failing READ; once
     * latched it is never overwritten, so a later diagnostic query cannot
     * destroy the evidence.  transport_code is the exact rdisk_direct result
     * when available (-30/-32/-33/-34/-40/-41, etc.). */
    ULONG read_fail_latched;
    LONG  read_fail_transport_code;
    ULONG read_fail_lba,read_fail_blocks,read_fail_bytes,read_fail_seq;
    ULONG read_fail_rx_len,read_fail_rx_expected;
    ULONG read_fail_bulk_out_programmed,read_fail_doeptsiz;
    ULONG read_fail_resp_magic,read_fail_resp_version;
    ULONG read_fail_resp_cmd,read_fail_resp_seq;
    LONG  read_fail_resp_result;
    ULONG read_fail_resp_length;
    ULONG read_fail_precheck;

    /* Last mismatched complete response observed while a READ was active.
     * This is especially useful when the final visible error is only -41
     * timeout after an earlier cmd/sequence mismatch was discarded. */
    ULONG read_stale_seen;
    ULONG read_stale_cmd,read_stale_seq;
    LONG  read_stale_result;
    ULONG read_stale_length;
    ULONG read_stale_rx_len,read_stale_rx_expected;

    /* Device-level latch: tells whether AmigaDOS failed before entering the
     * USB transaction or because rdisk_usb_submit() returned failure. */
    ULONG driver_read_fail_latched;
    ULONG driver_read_fail_stage; /* 1=refresh_geometry, 2=trackdisk submit, 3=SCSI submit */
    LONG  driver_read_fail_io_error;
    ULONG driver_read_fail_offset;
    ULONG driver_read_fail_length;
    ULONG driver_read_fail_actual;
    ULONG driver_read_fail_lba;
    ULONG driver_read_fail_blocks;
};
#endif
