/* POC4 ROMSAFE Direct-DWC2 backend.
 * Based on the hardware-validated v43r111 Extended-ROM backend.
 * READ-MULTIPACKET port: ONLY READ response payloads > 4 KiB use a bounded,
 * 8 KiB maximum multipacket OUT DMA. Header, <=4 KiB READ, WRITE, protocol,
 * boot/reset logic and companion behavior remain unchanged.
 * SPDX-License-Identifier: MPL-2.0
 */
#include <stdint.h>
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/tasks.h>
#include <hardware/intbits.h>
#include "rdisk_direct.h"

/* ROMSAFE freestanding byte primitives: never depend on libnix SysBase. */
static void *l2_memset(void *dst, int value, ULONG bytes)
{
    UBYTE *d=(UBYTE *)dst;
    UBYTE v=(UBYTE)value;
    while(bytes--) *d++=v;
    return dst;
}

static void *l2_memcpy(void *dst, const void *src, ULONG bytes)
{
    UBYTE *d=(UBYTE *)dst;
    const UBYTE *src8=(const UBYTE *)src;
    while(bytes--) *d++=*src8++;
    return dst;
}

/* Exec calls used by the direct DWC2 backend.  All use an explicit ExecBase;
 * no proto/exec.h call may recover a hidden global SysBase from ROM storage. */
static APTR exec_CachePreDMA(struct ExecBase *sys, APTR addr, ULONG *len, ULONG flags)
{
    register struct ExecBase *a6 __asm("a6")=sys;
    register APTR a0 __asm("a0")=addr;
    register ULONG *a1 __asm("a1")=len;
    register ULONG d0 __asm("d0")=flags;
    __asm volatile("jsr -762(a6)" : "+r"(d0)
                   : "r"(a6), "r"(a0), "r"(a1)
                   : "d1", "cc", "memory");
    return (APTR)d0;
}

static void exec_CachePostDMA(struct ExecBase *sys, APTR addr, ULONG *len, ULONG flags)
{
    register struct ExecBase *a6 __asm("a6")=sys;
    register APTR a0 __asm("a0")=addr;
    register ULONG *a1 __asm("a1")=len;
    register ULONG d0 __asm("d0")=flags;
    __asm volatile("jsr -768(a6)"
                   : "+r"(d0)
                   : "r"(a6), "r"(a0), "r"(a1)
                   : "d1", "cc", "memory");
}

static void exec_AddIntServer(struct ExecBase *sys, ULONG intnum, struct Interrupt *intr)
{
    register struct ExecBase *a6 __asm("a6")=sys;
    register ULONG d0 __asm("d0")=intnum;
    register struct Interrupt *a1 __asm("a1")=intr;
    __asm volatile("jsr -168(a6)"
                   :
                   : "r"(a6), "r"(d0), "r"(a1)
                   : "d1", "a0", "cc", "memory");
}

static void exec_Signal(struct ExecBase *sys, struct Task *task, ULONG signals)
{
    register struct ExecBase *a6 __asm("a6")=sys;
    register struct Task *a1 __asm("a1")=task;
    register ULONG d0 __asm("d0")=signals;
    __asm volatile("jsr -324(a6)"
                   :
                   : "r"(a6), "r"(a1), "r"(d0)
                   : "d1", "a0", "cc", "memory");
}

static ULONG exec_Wait(struct ExecBase *sys, ULONG signals)
{
    register struct ExecBase *a6 __asm("a6")=sys;
    register ULONG d0 __asm("d0")=signals;
    __asm volatile("jsr -318(a6)"
                   : "+r"(d0)
                   : "r"(a6)
                   : "d1", "a0", "a1", "cc", "memory");
    return d0;
}

#ifndef DMA_READ_FROM_RAM
#define DMA_READ_FROM_RAM 8UL
#endif
#define RDISK_POC_BLOCK_SIZE 512u
#define RDISK_CMD_IDENT 1u
#define RDISK_CMD_READ 2u
#define RDISK_CMD_WRITE 3u
#define RDISK_CMD_FLUSH 4u
#define RDISK_FEATURE_WRITE 1u
#define RDISK_FEATURE_FLUSH 2u
#define RDISK_FEATURE_USB_BULK 4u
/* Millisecond timers are unavailable in the bare Exec task. VBlank ticks
 * provide a real elapsed-time budget regardless of USB interrupt rate. */
#define RDISK_IDENT_RETRY_VBLANKS 150u /* ~2.5-3 s PAL/NTSC */
#define RDISK_READY_VBLANKS       750u /* ~12.5-15 s PAL/NTSC */
static inline uint32_t bswap32(uint32_t v)
{
    return ((v&0xffu)<<24)|((v&0xff00u)<<8)|((v>>8)&0xff00u)|(v>>24);
}
/* ------------------------------------------------------------------------- */
/* DWC2 low level, ported from the hardware-validated DIRECT-DWC2 USBNET POC */
/* ------------------------------------------------------------------------- */
#define USB2_BASE               0xF2980000u
#define MBOX_READ_ADDR          0xF200B880u
#define MBOX_STATUS_ADDR        0xF200B898u
#define MBOX_WRITE_ADDR         0xF200B8A0u
#define MBOX_TX_FULL            0x80000000u
#define MBOX_RX_EMPTY           0x40000000u
#define MBOX_PROPERTY           8u
#define TAG_SET_POWER           0x00028001u
#define DEV_USB_HCD             3u

#define USB_GAHBCFG             0x008u
#define USB_GUSBCFG             0x00cu
#define USB_GRSTCTL             0x010u
#define USB_GINTSTS             0x014u
#define USB_GINTMSK             0x018u
#define USB_GRXFSIZ             0x024u
#define USB_GNPTXFSIZ           0x028u
#define USB_GSNPSID             0x040u
#define USB_DPTXFSIZ(n)         (0x100u + ((n) * 4u))
#define USB_DCFG                0x800u
#define USB_DCTL                0x804u
#define USB_DSTS                0x808u
#define USB_DIEPMSK             0x810u
#define USB_DOEPMSK             0x814u
#define USB_DAINT               0x818u
#define USB_DAINTMSK            0x81cu
#define USB_DIEPCTL(n)          (0x900u + ((n) * 0x20u))
#define USB_DIEPINT(n)          (0x908u + ((n) * 0x20u))
#define USB_DIEPTSIZ(n)         (0x910u + ((n) * 0x20u))
#define USB_DIEPDMA(n)          (0x914u + ((n) * 0x20u))
#define USB_DOEPCTL(n)          (0xb00u + ((n) * 0x20u))
#define USB_DOEPINT(n)          (0xb08u + ((n) * 0x20u))
#define USB_DOEPTSIZ(n)         (0xb10u + ((n) * 0x20u))
#define USB_DOEPDMA(n)          (0xb14u + ((n) * 0x20u))

#define USB_GAHBCFG_GLBL_INTR_EN        (1u << 0)
#define USB_GAHBCFG_DMA_EN              (1u << 5)
#define USB_GUSBCFG_FORCEDEVMODE        (1u << 30)
#define USB_GUSBCFG_FORCEHOSTMODE       (1u << 29)
#define USB_GUSBCFG_HNPCAP              (1u << 9)
#define USB_GUSBCFG_SRPCAP              (1u << 8)
#define USB_GUSBCFG_TOUTCAL_MASK        0x7u
#define USB_GRSTCTL_AHBIDLE             (1u << 31)
#define USB_GRSTCTL_TXFNUM_ALL          (0x10u << 6)
#define USB_GRSTCTL_TXFFLSH             (1u << 5)
#define USB_GRSTCTL_RXFFLSH             (1u << 4)
#define USB_GRSTCTL_CSFTRST             (1u << 0)
#define USB_GINTSTS_OEPINT              (1u << 19)
#define USB_GINTSTS_USBSUSP             (1u << 11)
#define USB_GINTSTS_DISCONNINT          (1u << 29)
#define USB_GINTSTS_WKUPINT             (1u << 31)
#define USB_GINTSTS_IEPINT              (1u << 18)
#define USB_GINTSTS_ENUMDONE            (1u << 13)
#define USB_GINTSTS_USBRST              (1u << 12)
#define USB_GINTSTS_CURMODE_HOST        (1u << 0)
#define USB_DCFG_DEVADDR_MASK           (0x7fu << 4)
#define USB_DCFG_DEVADDR(a)             (((uint32_t)(a) & 0x7fu) << 4)
#define USB_DCFG_DEVSPD_MASK            3u
#define USB_DCTL_SFTDISCON              (1u << 1)
#define USB_DAINT_INEP(n)               (1u << (n))
#define USB_DAINT_OUTEP(n)              (1u << ((n) + 16))
#define USB_DXEPCTL_EPENA               (1u << 31)
#define USB_DXEPCTL_CNAK                (1u << 26)
#define USB_DXEPCTL_SNAK                (1u << 27)
#define USB_DXEPCTL_TXFNUM(n)           (((uint32_t)(n) & 0xfu) << 22)
#define USB_DXEPCTL_STALL               (1u << 21)
#define USB_DXEPCTL_EPTYPE_BULK         (2u << 18)
#define USB_DXEPCTL_USBACTEP            (1u << 15)
#define USB_DXEPCTL_MPS(n)              ((uint32_t)(n) & 0x7ffu)
#define USB_DXEPINT_SETUP               (1u << 3)
#define USB_DXEPINT_XFERCOMPL           (1u << 0)
#define USB_DXEPTSIZ_PKTCNT(n)          (((uint32_t)(n) & 0x3ffu) << 19)
#define USB_DXEPTSIZ_XFERSIZE(n)        ((uint32_t)(n) & 0x7ffffu)
#define USB_DIEPTSIZ0_PKTCNT(n)         (((uint32_t)(n) & 3u) << 19)
#define USB_DIEPTSIZ0_XFERSIZE(n)       ((uint32_t)(n) & 0x7fu)
#define USB_DOEPTSIZ0_SUPCNT(n)         (((uint32_t)(n) & 3u) << 29)
#define USB_DOEPTSIZ0_PKTCNT            (1u << 19)

#define USB_REQ_GET_STATUS               0x00u
#define USB_REQ_CLEAR_FEATURE            0x01u
#define USB_REQ_SET_ADDRESS              0x05u
#define USB_REQ_GET_DESCRIPTOR           0x06u
#define USB_REQ_GET_CONFIGURATION        0x08u
#define USB_REQ_SET_CONFIGURATION        0x09u
#define USB_REQ_GET_INTERFACE            0x0au
#define USB_REQ_SET_INTERFACE            0x0bu
#define USB_DT_DEVICE                    1u
#define USB_DT_CONFIG                    2u
#define USB_DT_STRING                    3u
#define USB_DT_DEVICE_QUALIFIER          6u
#define MS_OS_STRING_INDEX               0xeeu
#define MS_OS_VENDOR_CODE                0x20u
#define MS_OS_EXT_COMPAT_ID_INDEX        0x0004u

#define USB_EP_OUT                       1u
#define USB_EP_IN                        2u
#define USB_BULK_MPS                     512u
#define RDISK_USB_MAX_BYTES              (256u * 512u)
#define RDISK_USB_HDR_BYTES              32u
#define RDISK_USB_RX_MAX                 (RDISK_USB_HDR_BYTES + RDISK_USB_MAX_BYTES)
/* Build with -DRDISK_READ_MULTIPACKET=0 for the exact legacy OUT behavior.
 * The one-packet header and all <=4KiB READ replies stay on the legacy path.
 * The host's READ payload is always an integer number of 512-byte blocks. */
#ifndef RDISK_READ_MULTIPACKET
#define RDISK_READ_MULTIPACKET           1
#endif
#define RDISK_OUT_DMA_CHUNK              (16u * USB_BULK_MPS)
#define RDISK_OUT_SMALL_READ_MAX         (8u * USB_BULK_MPS)
#define RDISK_WIRE_REQ_MAGIC             0x32534452u /* "RDS2" */
#define RDISK_WIRE_RESP_MAGIC            0x32524452u /* "RDR2" */
#define RDISK_WIRE_VERSION               1u

static inline uint32_t usb_rd(uint32_t off) { return bswap32(*(volatile uint32_t *)(uintptr_t)(USB2_BASE+off)); }

static inline void usb_wr(uint32_t off,uint32_t value) { *(volatile uint32_t *)(uintptr_t)(USB2_BASE+off)=bswap32(value); }

static inline uint32_t raw_rd(uint32_t addr)
{
    return bswap32(*(volatile uint32_t *)(uintptr_t)addr);
}

static inline void raw_wr(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)addr = bswap32(value);
}

static void usb_delay(unsigned long loops)
{
    while (loops--)
        __asm__ volatile("nop");
}

static int usb_wait_mask(uint32_t off, uint32_t mask, uint32_t wanted, uint32_t loops)
{
    while (loops--)
        if ((usb_rd(off) & mask) == wanted)
            return 1;
    return 0;
}

/* CachePreDMA / CachePostDMA are the validated 68k hardware DMA API.
 * Track each outstanding mapping, including EP0 fragments and ZLP. */
struct dma_map { const void *p; ULONG len, phys, flags; };

struct usb_txn {
    uint8_t active;
    uint8_t internal_ident;
    uint32_t cmd;
    uint32_t seq;
    uint32_t lba;
    uint32_t blocks;
    uint32_t length;
    uint32_t buffer68k;
};

/*
 * ROMSAFE: every byte below is mutable runtime state and MUST live in RAM.
 * The v43r108 LoadModule binary placed these objects in its RAM hunk.  A
 * Kickstart Extended ROM does not: code/rodata remain in ROM, therefore a
 * single explicitly allocated state block replaces all writable globals.
 *
 * The block is 128-byte aligned because it owns all DMA buffers.  DevInit()
 * allocates rdisk_usb_state_bytes() bytes from MEMF_PUBLIC|MEMF_CLEAR and
 * rdisk_usb_state_init() returns the aligned state address inside that block.
 */
struct RDiskState {
    struct ExecBase *m_sysbase;

    volatile ULONG m_diag_stage,m_diag_failure;
    volatile ULONG m_diag_snpsid,m_diag_resets,m_diag_enums,m_diag_configs;
    volatile ULONG m_diag_irq_calls,m_diag_irq_claimed,m_diag_irq_no_dwc_pending;
    volatile ULONG m_diag_vblanks,m_diag_worker_wakes,m_diag_polls;
    volatile ULONG m_diag_ep0_setups,m_diag_ep0_in_done,m_diag_ep0_out_done;
    volatile ULONG m_diag_last_ep0_in,m_diag_last_ep0_out;
    volatile ULONG m_diag_last_gintsts,m_diag_last_masked;
    volatile ULONG m_diag_last_setup_a,m_diag_last_setup_b;
    volatile ULONG m_diag_disconnects,m_diag_suspends,m_diag_wakeups;

    volatile int m_rdisk_done;
    volatile int m_rdisk_result;
    uint32_t m_g_total_blocks,m_g_block_size,m_g_features;

    struct dma_map m_dma_maps[8];

    uint8_t m_g_prop[32] __attribute__((aligned(128)));
    uint8_t m_g_ep0_setup[8] __attribute__((aligned(64)));
    uint8_t m_g_ep0_out[64] __attribute__((aligned(64)));
    uint8_t m_g_ep0_tx[256] __attribute__((aligned(64)));
#if RDISK_READ_MULTIPACKET
    uint8_t m_g_bulk_out_pkt[RDISK_OUT_DMA_CHUNK] __attribute__((aligned(64)));
#else
    uint8_t m_g_bulk_out_pkt[USB_BULK_MPS] __attribute__((aligned(64)));
#endif
    uint8_t m_g_req_hdr[RDISK_USB_HDR_BYTES] __attribute__((aligned(64)));
    uint8_t m_g_payload[RDISK_USB_MAX_BYTES] __attribute__((aligned(64)));
    uint8_t m_g_rx[RDISK_USB_RX_MAX] __attribute__((aligned(64)));

    uint8_t m_usb_configured;
    uint8_t m_usb_hw_ready;
    uint8_t m_ep0_state;
    uint16_t m_ep0_tx_len;
    uint16_t m_ep0_tx_pos;
    uint16_t m_ep0_out_expected;
    uint8_t m_bulk_out_armed;
    uint32_t m_bulk_out_programmed;
    uint8_t m_bulk_in_active;
    uint8_t m_bulk_in_stage;

    struct usb_txn m_g_txn;
    uint32_t m_g_internal_seq;
    uint32_t m_g_ident_started_vblank;
    volatile uint32_t m_g_ident_timeouts,m_g_ident_stale_responses;
    uint32_t m_g_rx_len;
    uint32_t m_g_rx_expected;

    /* v43r114 first READ-failure diagnostic latch. */
    uint32_t m_read_fail_latched;
    int32_t  m_read_fail_code;
    uint32_t m_read_fail_lba,m_read_fail_blocks,m_read_fail_bytes,m_read_fail_seq;
    uint32_t m_read_fail_rx_len,m_read_fail_rx_expected;
    uint32_t m_read_fail_bulk_out_programmed,m_read_fail_doeptsiz;
    uint32_t m_read_fail_resp_magic,m_read_fail_resp_version;
    uint32_t m_read_fail_resp_cmd,m_read_fail_resp_seq;
    int32_t  m_read_fail_resp_result;
    uint32_t m_read_fail_resp_length,m_read_fail_precheck;
    uint32_t m_read_stale_seen,m_read_stale_cmd,m_read_stale_seq;
    int32_t  m_read_stale_result;
    uint32_t m_read_stale_length,m_read_stale_rx_len,m_read_stale_rx_expected;

    uint8_t m_usb_irq_added;
    struct Interrupt m_usb_irq,m_usb_vblank;
    struct Task *m_wake_task;
    ULONG m_wake_mask;
    uint8_t m_vblank_added;
} __attribute__((aligned(128)));

/* Keep the old, hardware-validated backend body readable: identifiers still
 * look like the v43r108 globals, but now resolve through the per-device RAM
 * state passed as `s` to every stateful function. */
#define diag_stage              (s->m_diag_stage)
#define diag_failure            (s->m_diag_failure)
#define diag_snpsid             (s->m_diag_snpsid)
#define diag_resets             (s->m_diag_resets)
#define diag_enums              (s->m_diag_enums)
#define diag_configs            (s->m_diag_configs)
#define diag_irq_calls          (s->m_diag_irq_calls)
#define diag_irq_claimed        (s->m_diag_irq_claimed)
#define diag_irq_no_dwc_pending (s->m_diag_irq_no_dwc_pending)
#define diag_vblanks            (s->m_diag_vblanks)
#define diag_worker_wakes       (s->m_diag_worker_wakes)
#define diag_polls              (s->m_diag_polls)
#define diag_ep0_setups         (s->m_diag_ep0_setups)
#define diag_ep0_in_done        (s->m_diag_ep0_in_done)
#define diag_ep0_out_done       (s->m_diag_ep0_out_done)
#define diag_last_ep0_in        (s->m_diag_last_ep0_in)
#define diag_last_ep0_out       (s->m_diag_last_ep0_out)
#define diag_last_gintsts       (s->m_diag_last_gintsts)
#define diag_last_masked        (s->m_diag_last_masked)
#define diag_last_setup_a       (s->m_diag_last_setup_a)
#define diag_last_setup_b       (s->m_diag_last_setup_b)
#define diag_disconnects        (s->m_diag_disconnects)
#define diag_suspends           (s->m_diag_suspends)
#define diag_wakeups            (s->m_diag_wakeups)
#define rdisk_done              (s->m_rdisk_done)
#define rdisk_result            (s->m_rdisk_result)
#define g_total_blocks          (s->m_g_total_blocks)
#define g_block_size            (s->m_g_block_size)
#define g_features              (s->m_g_features)
#define dma_maps               (s->m_dma_maps)
#define g_prop                  (s->m_g_prop)
#define g_ep0_setup             (s->m_g_ep0_setup)
#define g_ep0_out               (s->m_g_ep0_out)
#define g_ep0_tx                (s->m_g_ep0_tx)
#define g_bulk_out_pkt          (s->m_g_bulk_out_pkt)
#define g_req_hdr               (s->m_g_req_hdr)
#define g_payload               (s->m_g_payload)
#define g_rx                    (s->m_g_rx)
#define usb_configured          (s->m_usb_configured)
#define usb_hw_ready            (s->m_usb_hw_ready)
#define ep0_sm_state            (s->m_ep0_state)
#define ep0_tx_len              (s->m_ep0_tx_len)
#define ep0_tx_pos              (s->m_ep0_tx_pos)
#define ep0_out_expected        (s->m_ep0_out_expected)
#define bulk_out_armed          (s->m_bulk_out_armed)
#define bulk_out_programmed     (s->m_bulk_out_programmed)
#define bulk_in_active          (s->m_bulk_in_active)
#define bulk_in_stage           (s->m_bulk_in_stage)
#define g_txn                   (s->m_g_txn)
#define g_internal_seq          (s->m_g_internal_seq)
#define g_ident_started_vblank  (s->m_g_ident_started_vblank)
#define g_ident_timeouts        (s->m_g_ident_timeouts)
#define g_ident_stale_responses (s->m_g_ident_stale_responses)
#define g_rx_len                (s->m_g_rx_len)
#define g_rx_expected           (s->m_g_rx_expected)
#define read_fail_latched       (s->m_read_fail_latched)
#define read_fail_code          (s->m_read_fail_code)
#define read_fail_lba           (s->m_read_fail_lba)
#define read_fail_blocks        (s->m_read_fail_blocks)
#define read_fail_bytes         (s->m_read_fail_bytes)
#define read_fail_seq           (s->m_read_fail_seq)
#define read_fail_rx_len        (s->m_read_fail_rx_len)
#define read_fail_rx_expected   (s->m_read_fail_rx_expected)
#define read_fail_bulk_out_programmed (s->m_read_fail_bulk_out_programmed)
#define read_fail_doeptsiz      (s->m_read_fail_doeptsiz)
#define read_fail_resp_magic    (s->m_read_fail_resp_magic)
#define read_fail_resp_version  (s->m_read_fail_resp_version)
#define read_fail_resp_cmd      (s->m_read_fail_resp_cmd)
#define read_fail_resp_seq      (s->m_read_fail_resp_seq)
#define read_fail_resp_result   (s->m_read_fail_resp_result)
#define read_fail_resp_length   (s->m_read_fail_resp_length)
#define read_fail_precheck      (s->m_read_fail_precheck)
#define read_stale_seen         (s->m_read_stale_seen)
#define read_stale_cmd          (s->m_read_stale_cmd)
#define read_stale_seq          (s->m_read_stale_seq)
#define read_stale_result       (s->m_read_stale_result)
#define read_stale_length       (s->m_read_stale_length)
#define read_stale_rx_len       (s->m_read_stale_rx_len)
#define read_stale_rx_expected  (s->m_read_stale_rx_expected)
#define usb_irq_added           (s->m_usb_irq_added)
#define usb_irq                 (s->m_usb_irq)
#define usb_vblank              (s->m_usb_vblank)
#define wake_task               (s->m_wake_task)
#define wake_mask               (s->m_wake_mask)
#define vblank_added            (s->m_vblank_added)

ULONG rdisk_usb_state_bytes(void)
{
    return (ULONG)sizeof(struct RDiskState) + 127UL;
}

struct RDiskState *rdisk_usb_state_init(APTR raw, struct ExecBase *sys)
{
    uintptr_t p;
    struct RDiskState *s;
    if (!raw || !sys) return NULL;
    p = ((uintptr_t)raw + 127u) & ~(uintptr_t)127u;
    s = (struct RDiskState *)p;
    l2_memset(s, 0, sizeof(*s));
    s->m_sysbase = sys;
    s->m_g_internal_seq = 0x80000000u;
    return s;
}

static void diag_mark(struct RDiskState *s, ULONG stage) { diag_stage=stage; }
static void diag_fail(struct RDiskState *s, ULONG failure) { diag_failure=failure; }
static void rdisk_complete(struct RDiskState *s, int32_t r) {rdisk_result=r;rdisk_done=1;}
static uint32_t dma_bus_addr(struct RDiskState *s, const void *p)
{
    unsigned i;
    for(i=0;i<8;i++) if(dma_maps[i].p==p) return dma_maps[i].phys;
    return 0;
}
static void dma_prepare(struct RDiskState *s, void *p, uint32_t bytes, ULONG flags)
{
    ULONG len=bytes?bytes:1;
    ULONG phys;
    unsigned i;
    for(i=0;i<8;i++) if(dma_maps[i].p==p) return;
    phys=(ULONG)exec_CachePreDMA(s->m_sysbase,p,&len,flags);
    if(!phys || len<(bytes?bytes:1)) {
        if(phys) exec_CachePostDMA(s->m_sysbase,p,&len,flags);
        return;
    }
    for(i=0;i<8;i++) if(!dma_maps[i].p) {
        dma_maps[i].p=p;
        dma_maps[i].len=len;
        dma_maps[i].phys=(phys&0x3fffffffUL)|0xc0000000UL;
        dma_maps[i].flags=flags;
        return;
    }
    exec_CachePostDMA(s->m_sysbase,p,&len,flags);
}
static void dma_finish(struct RDiskState *s, const void *p)
{
    unsigned i;
    for(i=0;i<8;i++) if(dma_maps[i].p==p) {
        ULONG len=dma_maps[i].len;
        exec_CachePostDMA(s->m_sysbase,(APTR)p,&len,dma_maps[i].flags);
        dma_maps[i].p=0;
        return;
    }
}
static void dma_to_device(struct RDiskState *s,void *p,uint32_t bytes) { dma_prepare(s,p,bytes,DMA_READ_FROM_RAM); }
static void dma_from_device_prepare(struct RDiskState *s,void *p,uint32_t bytes) { dma_prepare(s,p,bytes,0); }
static void dma_from_device_done(struct RDiskState *s,void *p,uint32_t bytes) { (void)bytes;dma_finish(s,p); }
static void put_le32(uint8_t *, uint32_t);
static int usb_power_on(struct RDiskState *s)
{
    uint32_t phys,msg,guard,st;
    l2_memset(g_prop,0,sizeof(g_prop));
    put_le32(g_prop,32);put_le32(g_prop+8,TAG_SET_POWER);
    put_le32(g_prop+12,8);put_le32(g_prop+16,8);
    put_le32(g_prop+20,DEV_USB_HCD);put_le32(g_prop+24,3);
    dma_from_device_prepare(s,g_prop,sizeof(g_prop));
    phys=dma_bus_addr(s,g_prop);
    if(!phys) return 0;
    msg=(phys&0xfffffff0u)|MBOX_PROPERTY;
    guard=0x00800000u;
    do {st=raw_rd(MBOX_STATUS_ADDR);if(!(st&MBOX_TX_FULL))break;}while(--guard);
    if(!guard){dma_finish(s,g_prop);return 0;}
    raw_wr(MBOX_WRITE_ADDR,msg);
    guard=0x00800000u;
    while(guard--){
        uint32_t r;
        st=raw_rd(MBOX_STATUS_ADDR);
        if(st&MBOX_RX_EMPTY)continue;
        r=raw_rd(MBOX_READ_ADDR);
        if((r&15u)==MBOX_PROPERTY && (r&~15u)==(msg&~15u)){
            dma_finish(s,g_prop);return 1;
        }
    }
    dma_finish(s,g_prop);return 0;
}
static void usb_flush_fifos(void)
{
    usb_wr(USB_GRSTCTL, USB_GRSTCTL_TXFNUM_ALL | USB_GRSTCTL_TXFFLSH | USB_GRSTCTL_RXFFLSH);
    (void)usb_wait_mask(USB_GRSTCTL, USB_GRSTCTL_TXFFLSH | USB_GRSTCTL_RXFFLSH, 0, 1000000u);
}

struct usb_setup_packet {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static const uint8_t device_desc[] = {
    18, USB_DT_DEVICE, 0x00,0x02, 0,0,0,64,
    0x25,0x05, 0xad,0xa4, 0x00,0x01, 1,2,3,1
};
static const uint8_t qualifier_desc[] = {10,USB_DT_DEVICE_QUALIFIER,0x00,0x02,0,0,0,64,1,0};
static const uint8_t config_desc[] = {
    9,USB_DT_CONFIG, 32,0, 1,1,0,0x80,50,
    9,4,0,0,2,0xff,0x52,0x44,0,
    7,5,0x01,0x02,0x00,0x02,0,
    7,5,0x82,0x02,0x00,0x02,0
};
static const uint8_t str0[] = {4,USB_DT_STRING,0x09,0x04};
static const uint8_t str1[] = {16,USB_DT_STRING,'P',0,'i',0,'S',0,'t',0,'o',0,'r',0,'m',0};
static const uint8_t str2[] = {40,USB_DT_STRING,'P',0,'i',0,'S',0,'t',0,'o',0,'r',0,'m',0,' ',0,'R',0,'e',0,'m',0,'o',0,'t',0,'e',0,' ',0,'D',0,'i',0,'s',0,'k',0};
static const uint8_t str3[] = {20,USB_DT_STRING,'R',0,'D',0,'I',0,'S',0,'K',0,'P',0,'O',0,'C',0,'2',0};
static const uint8_t ms_os_string_desc[] = {0x12,USB_DT_STRING,'M',0,'S',0,'F',0,'T',0,'1',0,'0',0,'0',0,MS_OS_VENDOR_CODE,0};
static const uint8_t ms_os_ext_compat_id[] = {
    0x28,0,0,0, 0,1, 4,0, 1,0,0,0,0,0,0,0,
    0,1, 'W','I','N','U','S','B',0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0
};


enum { EP0_IDLE=0, EP0_IN_DATA, EP0_OUT_STATUS, EP0_OUT_DATA, EP0_IN_STATUS };
enum { BIN_IDLE=0, BIN_HEADER, BIN_PAYLOAD, BIN_WAIT_RESPONSE };
static uint32_t bulk_out_next_bytes(struct RDiskState *s); /* defined after response state */

static void ep0_arm_setup(struct RDiskState *s)
{
    dma_from_device_prepare(s,g_ep0_setup, 8);
    usb_wr(USB_DOEPDMA(0), dma_bus_addr(s,g_ep0_setup));
    usb_wr(USB_DOEPTSIZ(0), USB_DOEPTSIZ0_SUPCNT(1) | USB_DOEPTSIZ0_PKTCNT | 8u);
    usb_wr(USB_DOEPCTL(0), USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
}

static void ep0_arm_out(struct RDiskState *s,uint16_t len)
{
    dma_from_device_prepare(s,g_ep0_out, len ? len : 1u);
    usb_wr(USB_DOEPDMA(0), dma_bus_addr(s,g_ep0_out));
    usb_wr(USB_DOEPTSIZ(0), USB_DOEPTSIZ0_PKTCNT | (uint32_t)len);
    usb_wr(USB_DOEPCTL(0), USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
}

static void ep0_send_next(struct RDiskState *s)
{
    uint16_t remain, chunk;
    if (ep0_tx_pos >= ep0_tx_len)
        return;
    remain = (uint16_t)(ep0_tx_len - ep0_tx_pos);
    chunk = remain > 64u ? 64u : remain;
    dma_to_device(s,g_ep0_tx + ep0_tx_pos, chunk);
    usb_wr(USB_DIEPDMA(0), dma_bus_addr(s,g_ep0_tx + ep0_tx_pos));
    usb_wr(USB_DIEPTSIZ(0), USB_DIEPTSIZ0_PKTCNT(1) | USB_DIEPTSIZ0_XFERSIZE(chunk));
    usb_wr(USB_DIEPCTL(0), USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
    ep0_tx_pos = (uint16_t)(ep0_tx_pos + chunk);
}

static void ep0_send(struct RDiskState *s,const uint8_t *data, uint16_t len)
{
    if (len > sizeof(g_ep0_tx))
        len = sizeof(g_ep0_tx);
    l2_memcpy(g_ep0_tx, data, len);
    ep0_tx_len = len;
    ep0_tx_pos = 0;
    if (len)
        ep0_send_next(s);
}

static void ep0_zlp(struct RDiskState *s)
{
    dma_to_device(s,g_ep0_tx, 1);
    usb_wr(USB_DIEPDMA(0), dma_bus_addr(s,g_ep0_tx));
    usb_wr(USB_DIEPTSIZ(0), USB_DIEPTSIZ0_PKTCNT(1));
    usb_wr(USB_DIEPCTL(0), USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
}

static void ep0_stall(void)
{
    usb_wr(USB_DIEPCTL(0), usb_rd(USB_DIEPCTL(0)) | USB_DXEPCTL_STALL);
    usb_wr(USB_DOEPCTL(0), usb_rd(USB_DOEPCTL(0)) | USB_DXEPCTL_STALL);
}

static void set_address_now(uint8_t addr)
{
    uint32_t v = usb_rd(USB_DCFG);
    v &= ~USB_DCFG_DEVADDR_MASK;
    v |= USB_DCFG_DEVADDR(addr);
    usb_wr(USB_DCFG, v);
}

static void bulk_out_arm(struct RDiskState *s)
{
    uint32_t bytes, phys;
    if (!usb_configured)
        return;
    bytes = bulk_out_next_bytes(s);
    dma_from_device_prepare(s,g_bulk_out_pkt, bytes);
    phys = dma_bus_addr(s,g_bulk_out_pkt);
    /* CachePreDMA may refuse a non-contiguous large mapping. Fall back to
     * the proven single-packet path rather than ever programming DMA addr 0. */
    if (!phys && bytes > USB_BULK_MPS) {
        bytes = USB_BULK_MPS;
        dma_from_device_prepare(s,g_bulk_out_pkt, bytes);
        phys = dma_bus_addr(s,g_bulk_out_pkt);
    }
    if (!phys)
        return;
    usb_wr(USB_DOEPDMA(USB_EP_OUT), phys);
    usb_wr(USB_DOEPTSIZ(USB_EP_OUT),
           USB_DXEPTSIZ_PKTCNT(bytes / USB_BULK_MPS) |
           USB_DXEPTSIZ_XFERSIZE(bytes));
    usb_wr(USB_DOEPCTL(USB_EP_OUT), usb_rd(USB_DOEPCTL(USB_EP_OUT)) | USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
    bulk_out_programmed = bytes;
    bulk_out_armed = 1;
}

static void bulk_endpoints_open(struct RDiskState *s)
{
    usb_wr(USB_DIEPINT(USB_EP_IN), 0xffffffffu);
    usb_wr(USB_DOEPINT(USB_EP_OUT), 0xffffffffu);
    usb_wr(USB_DIEPCTL(USB_EP_IN), USB_DXEPCTL_MPS(USB_BULK_MPS) | USB_DXEPCTL_USBACTEP |
           USB_DXEPCTL_EPTYPE_BULK | USB_DXEPCTL_TXFNUM(USB_EP_IN));
    usb_wr(USB_DOEPCTL(USB_EP_OUT), USB_DXEPCTL_MPS(USB_BULK_MPS) | USB_DXEPCTL_USBACTEP |
           USB_DXEPCTL_EPTYPE_BULK);
    bulk_out_arm(s);
}

static void bulk_send(struct RDiskState *s,const uint8_t *data, uint32_t len, uint8_t stage)
{
    uint32_t packets;
    if (!len || bulk_in_active)
        return;
    packets = (len + USB_BULK_MPS - 1u) / USB_BULK_MPS;
    dma_to_device(s,(void *)(uintptr_t)data, len);
    bulk_in_active = 1;
    bulk_in_stage = stage;
    usb_wr(USB_DIEPDMA(USB_EP_IN), dma_bus_addr(s,data));
    usb_wr(USB_DIEPTSIZ(USB_EP_IN), USB_DXEPTSIZ_PKTCNT(packets) | USB_DXEPTSIZ_XFERSIZE(len));
    usb_wr(USB_DIEPCTL(USB_EP_IN), USB_DXEPCTL_MPS(USB_BULK_MPS) | USB_DXEPCTL_USBACTEP |
           USB_DXEPCTL_EPTYPE_BULK | USB_DXEPCTL_TXFNUM(USB_EP_IN) |
           USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
}

static void usb_reset_state(struct RDiskState *s); /* used by SET_CONFIGURATION before definition */
static void handle_setup(struct RDiskState *s,const uint8_t *raw)
{
    uint8_t bm = raw[0], req = raw[1];
    uint16_t wValue = get_le16(raw + 2), wIndex = get_le16(raw + 4), wLength = get_le16(raw + 6);
    const uint8_t *data = NULL;
    uint16_t len = 0;
    static const uint8_t zero2[2] = {0,0};

    if ((bm & 0x80u) && req == USB_REQ_GET_DESCRIPTOR) {
        uint8_t type = (uint8_t)(wValue >> 8), idx = (uint8_t)wValue;
        if (type == USB_DT_DEVICE) { data = device_desc; len = sizeof(device_desc); }
        else if (type == USB_DT_DEVICE_QUALIFIER) { data = qualifier_desc; len = sizeof(qualifier_desc); }
        else if (type == USB_DT_CONFIG) { data = config_desc; len = sizeof(config_desc); }
        else if (type == USB_DT_STRING) {
            if (idx == 0) { data = str0; len = sizeof(str0); }
            else if (idx == 1) { data = str1; len = sizeof(str1); }
            else if (idx == 2) { data = str2; len = sizeof(str2); }
            else if (idx == 3) { data = str3; len = sizeof(str3); }
            else if (idx == MS_OS_STRING_INDEX) { data = ms_os_string_desc; len = sizeof(ms_os_string_desc); }
        }
        if (!data) { ep0_stall(); return; }
        if (len > wLength) len = wLength;
        ep0_sm_state = EP0_IN_DATA;
        ep0_send(s,data, len);
        return;
    }
    if (bm == 0x00u && req == USB_REQ_SET_ADDRESS) {
        set_address_now((uint8_t)(wValue & 0x7fu));
        ep0_sm_state = EP0_IN_STATUS;
        ep0_zlp(s);
        return;
    }
    if (bm == 0x00u && req == USB_REQ_SET_CONFIGURATION) {
        /* A new configuration is a new media/transaction generation, even
         * if it happens without a visibly separate DWC2 USBRST interrupt. */
        if (usb_configured || !wValue) usb_reset_state(s);
        usb_configured = wValue ? 1u : 0u;
        if (usb_configured) {++diag_configs;diag_mark(s,RD_STAGE_CONFIGURED);bulk_endpoints_open(s);}
        ep0_sm_state = EP0_IN_STATUS;
        ep0_zlp(s);
        return;
    }
    if (bm == 0x80u && req == USB_REQ_GET_CONFIGURATION) {
        g_ep0_tx[0] = usb_configured;
        ep0_sm_state = EP0_IN_DATA;
        ep0_send(s,g_ep0_tx, wLength < 1u ? wLength : 1u);
        return;
    }
    if ((bm & 0x7fu) == 0x00u && req == USB_REQ_GET_STATUS) {
        ep0_sm_state = EP0_IN_DATA;
        ep0_send(s,zero2, wLength < 2u ? wLength : 2u);
        return;
    }
    if ((bm & 0x7fu) == 0x02u && req == USB_REQ_CLEAR_FEATURE && wValue == 0) {
        ep0_sm_state = EP0_IN_STATUS;
        ep0_zlp(s);
        return;
    }
    if ((bm & 0x7fu) == 0x01u && req == USB_REQ_GET_INTERFACE) {
        g_ep0_tx[0] = 0;
        ep0_sm_state = EP0_IN_DATA;
        ep0_send(s,g_ep0_tx, wLength < 1u ? wLength : 1u);
        return;
    }
    if ((bm & 0x7fu) == 0x01u && req == USB_REQ_SET_INTERFACE && wValue == 0 && wIndex == 0) {
        ep0_sm_state = EP0_IN_STATUS;
        ep0_zlp(s);
        return;
    }
    if (bm == 0xc0u && req == MS_OS_VENDOR_CODE && wIndex == MS_OS_EXT_COMPAT_ID_INDEX) {
        len = sizeof(ms_os_ext_compat_id);
        if (len > wLength) len = wLength;
        ep0_sm_state = EP0_IN_DATA;
        ep0_send(s,ms_os_ext_compat_id, len);
        return;
    }
    ep0_stall();
}

/* ------------------------------------------------------------------------- */
/* USB remote-disk transaction state                                         */
/* ------------------------------------------------------------------------- */

/* Receive the 32-byte response header with the original short arm. Once its
 * length is known, only a large READ payload may use 8 KiB OUT DMA chunks.
 * Invalid/mismatched headers and small READs stay on the legacy 512-byte path. */
static uint32_t bulk_out_next_bytes(struct RDiskState *s)
{
#if RDISK_READ_MULTIPACKET
    if (g_txn.active && g_txn.cmd == RDISK_CMD_READ &&
        g_txn.length > RDISK_OUT_SMALL_READ_MAX &&
        g_rx_len >= RDISK_USB_HDR_BYTES &&
        g_rx_expected == RDISK_USB_HDR_BYTES + g_txn.length &&
        g_rx_expected > g_rx_len) {
        uint32_t remaining = g_rx_expected - g_rx_len;
        uint32_t bytes = remaining < RDISK_OUT_DMA_CHUNK ?
                         remaining : RDISK_OUT_DMA_CHUNK;
        if (bytes >= 2u * USB_BULK_MPS &&
            (bytes % USB_BULK_MPS) == 0)
            return bytes;
    }
#endif
    return USB_BULK_MPS;
}

/* v43r114 diagnostics: preserve the first failing READ forever.
 * This is intentionally passive: no retries, no timing changes and no
 * alteration of the transport decision.  Header fields are sampled only
 * when already present in the normal RX accumulator. */
static void read_diag_latch_current(struct RDiskState *s,int32_t error,uint32_t precheck)
{
    if (read_fail_latched || !g_txn.active || g_txn.internal_ident ||
        g_txn.cmd != RDISK_CMD_READ)
        return;
    read_fail_latched=1;
    read_fail_code=error;
    read_fail_lba=g_txn.lba;
    read_fail_blocks=g_txn.blocks;
    read_fail_bytes=g_txn.length;
    read_fail_seq=g_txn.seq;
    read_fail_rx_len=g_rx_len;
    read_fail_rx_expected=g_rx_expected;
    read_fail_bulk_out_programmed=bulk_out_programmed;
    read_fail_doeptsiz=usb_hw_ready ? usb_rd(USB_DOEPTSIZ(USB_EP_OUT)) : 0;
    read_fail_precheck=precheck;
    if (g_rx_len >= RDISK_USB_HDR_BYTES) {
        read_fail_resp_magic=get_le32(g_rx+0);
        read_fail_resp_version=get_le32(g_rx+4);
        read_fail_resp_cmd=get_le32(g_rx+8);
        read_fail_resp_seq=get_le32(g_rx+12);
        read_fail_resp_result=(int32_t)get_le32(g_rx+16);
        read_fail_resp_length=get_le32(g_rx+20);
    }
}

static void read_diag_latch_request(struct RDiskState *s,int32_t error,
                                    uint32_t cmd,uint32_t seq,uint32_t lba,
                                    uint32_t blocks,uint32_t length,
                                    uint32_t precheck)
{
    if (read_fail_latched || cmd != RDISK_CMD_READ) return;
    read_fail_latched=1;
    read_fail_code=error;
    read_fail_lba=lba;
    read_fail_blocks=blocks;
    read_fail_bytes=length;
    read_fail_seq=seq;
    read_fail_rx_len=g_rx_len;
    read_fail_rx_expected=g_rx_expected;
    read_fail_bulk_out_programmed=bulk_out_programmed;
    read_fail_doeptsiz=usb_hw_ready ? usb_rd(USB_DOEPTSIZ(USB_EP_OUT)) : 0;
    read_fail_precheck=precheck;
}

static void usb_txn_fail(struct RDiskState *s,int32_t error)
{
    uint8_t internal = g_txn.internal_ident;
    read_diag_latch_current(s,error,0);
    l2_memset(&g_txn, 0, sizeof(g_txn));
    g_rx_len = g_rx_expected = 0;
    bulk_in_active = 0;
    bulk_in_stage = BIN_IDLE;
    if (!internal)
        rdisk_complete(s,error);
}

static void usb_process_response(struct RDiskState *s)
{
    uint32_t magic, version, cmd, seq, result_u, length, total_blocks, block_size;
    int32_t result;
    if (g_rx_len < RDISK_USB_HDR_BYTES)
        return;
    magic = get_le32(g_rx + 0);
    version = get_le32(g_rx + 4);
    cmd = get_le32(g_rx + 8);
    seq = get_le32(g_rx + 12);
    result_u = get_le32(g_rx + 16);
    length = get_le32(g_rx + 20);
    total_blocks = get_le32(g_rx + 24);
    block_size = get_le32(g_rx + 28);
    result = (int32_t)result_u;

    if (magic != RDISK_WIRE_RESP_MAGIC || version != RDISK_WIRE_VERSION ||
        !g_txn.active || length > RDISK_USB_MAX_BYTES) {
        usb_txn_fail(s,-30);
        return;
    }
    if (g_rx_len < RDISK_USB_HDR_BYTES + length)
        return;
    /* A response to an expired internal IDENT may arrive after the next
     * IDENT started. Ignore the complete old frame, NOT the new transaction.
     * A malformed frame is still a transport error, not a resync marker. */
    if (cmd != g_txn.cmd || seq != g_txn.seq) {
        ++g_ident_stale_responses;
        if (g_txn.cmd == RDISK_CMD_READ && !g_txn.internal_ident) {
            read_stale_seen++;
            read_stale_cmd=cmd;
            read_stale_seq=seq;
            read_stale_result=result;
            read_stale_length=length;
            read_stale_rx_len=g_rx_len;
            read_stale_rx_expected=g_rx_expected;
        }
        g_rx_len = g_rx_expected = 0;
        return;
    }

    if (g_txn.cmd == RDISK_CMD_IDENT && result == 0) {
        if (block_size == RDISK_POC_BLOCK_SIZE && total_blocks != 0 &&
            (length == 0 || length == 4)) {
            uint32_t offered = length == 4 ?
                get_le32(g_rx + RDISK_USB_HDR_BYTES) : 0u;
            g_total_blocks = total_blocks;
            g_block_size = block_size;
            diag_mark(s,RD_STAGE_IDENT);
            /* A legacy IDENT without capability data is conservatively RO. */
            g_features = (offered & RDISK_FEATURE_WRITE) |
                         RDISK_FEATURE_FLUSH | RDISK_FEATURE_USB_BULK;
        } else {
            result = -31;
        }
    } else if (g_txn.cmd == RDISK_CMD_READ && result == 0) {
        if (length != g_txn.length) {
            result = -32;
        } else {
            l2_memcpy((void *)(uintptr_t)g_txn.buffer68k, g_rx + RDISK_USB_HDR_BYTES, length);
        }
    }

    {
        uint8_t internal = g_txn.internal_ident;
        if (!internal && g_txn.cmd == RDISK_CMD_READ && result != 0)
            read_diag_latch_current(s,result,0);
        l2_memset(&g_txn, 0, sizeof(g_txn));
        g_rx_len = g_rx_expected = 0;
        bulk_in_stage = BIN_IDLE;
        if (!internal)
            rdisk_complete(s,result);
    }
}

static void usb_rx_packet(struct RDiskState *s,uint32_t got)
{
    uint32_t room;
    if (!got || !g_txn.active)
        return;
    room = RDISK_USB_RX_MAX - g_rx_len;
    if (got > room) {
        usb_txn_fail(s,-33);
        return;
    }
    l2_memcpy(g_rx + g_rx_len, g_bulk_out_pkt, got);
    g_rx_len += got;
    if (g_rx_len >= RDISK_USB_HDR_BYTES && g_rx_expected == 0) {
        uint32_t len = get_le32(g_rx + 20);
        if (len > RDISK_USB_MAX_BYTES) {
            usb_txn_fail(s,-34);
            return;
        }
        g_rx_expected = RDISK_USB_HDR_BYTES + len;
    }
    if (g_rx_expected && g_rx_len >= g_rx_expected)
        usb_process_response(s);
}

static void usb_start_txn(struct RDiskState *s,uint32_t cmd, uint32_t seq, uint32_t lba,
                          uint32_t blocks, uint32_t length, uint32_t buffer68k,
                          uint8_t internal_ident)
{
    if (g_txn.active || bulk_in_active || !usb_configured)
        return;
    if (length > RDISK_USB_MAX_BYTES) {
        if (!internal_ident) rdisk_complete(s,-20);
        return;
    }

    l2_memset(&g_txn, 0, sizeof(g_txn));
    g_txn.active = 1;
    g_txn.internal_ident = internal_ident;
    g_txn.cmd = cmd;
    g_txn.seq = seq;
    g_txn.lba = lba;
    g_txn.blocks = blocks;
    g_txn.length = length;
    g_txn.buffer68k = buffer68k;
    if (internal_ident) g_ident_started_vblank = diag_vblanks;
    g_rx_len = g_rx_expected = 0;

    if (cmd == RDISK_CMD_WRITE && length)
        l2_memcpy(g_payload, (const void *)(uintptr_t)buffer68k, length);

    put_le32(g_req_hdr + 0, RDISK_WIRE_REQ_MAGIC);
    put_le32(g_req_hdr + 4, RDISK_WIRE_VERSION);
    put_le32(g_req_hdr + 8, cmd);
    put_le32(g_req_hdr + 12, seq);
    put_le32(g_req_hdr + 16, lba);
    put_le32(g_req_hdr + 20, blocks);
    put_le32(g_req_hdr + 24, length);
    put_le32(g_req_hdr + 28, 0);
    bulk_send(s,g_req_hdr, RDISK_USB_HDR_BYTES, BIN_HEADER);
}

static void usb_bulk_in_complete(struct RDiskState *s)
{
    uint8_t stage = bulk_in_stage;
    bulk_in_active = 0;
    if (!g_txn.active) {
        bulk_in_stage = BIN_IDLE;
        return;
    }
    if (stage == BIN_HEADER && g_txn.cmd == RDISK_CMD_WRITE && g_txn.length) {
        bulk_send(s,g_payload, g_txn.length, BIN_PAYLOAD);
    } else {
        bulk_in_stage = BIN_WAIT_RESPONSE;
    }
}

static void usb_reset_state(struct RDiskState *s)
{
    if(g_txn.active && !g_txn.internal_ident) {
        read_diag_latch_current(s,-40,0);
        rdisk_complete(s,-40);
    }
    /* A reset can cancel a jumbo OUT transfer without XFERCOMPL. Do not
     * retain its CachePreDMA mapping into the next USB configuration. */
    if (bulk_out_armed && bulk_out_programmed > USB_BULK_MPS)
        dma_finish(s,g_bulk_out_pkt);
    usb_configured = 0;
    ep0_sm_state = EP0_IDLE;
    ep0_tx_len = ep0_tx_pos = 0;
    ep0_out_expected = 0;
    bulk_out_armed = 0;
    bulk_out_programmed = 0;
    bulk_in_active = 0;
    bulk_in_stage = BIN_IDLE;
    l2_memset(&g_txn, 0, sizeof(g_txn));
    g_rx_len = g_rx_expected = 0;
    g_total_blocks = 0;
    g_ident_started_vblank = diag_vblanks;
    g_block_size = RDISK_POC_BLOCK_SIZE;
    g_features = 0;
}

static void usb_on_reset(struct RDiskState *s)
{
    usb_reset_state(s);
    set_address_now(0);
    usb_flush_fifos();
    usb_wr(USB_DIEPINT(0), 0xffffffffu);
    usb_wr(USB_DOEPINT(0), 0xffffffffu);
    usb_wr(USB_DAINTMSK, USB_DAINT_INEP(0) | USB_DAINT_OUTEP(0) |
           USB_DAINT_INEP(USB_EP_IN) | USB_DAINT_OUTEP(USB_EP_OUT));
    ep0_arm_setup(s);
}

static void usb_poll_epints(struct RDiskState *s)
{
    uint32_t daint = usb_rd(USB_DAINT);
    if (daint & USB_DAINT_OUTEP(0)) {
        uint32_t i = usb_rd(USB_DOEPINT(0));
        if (i) { diag_last_ep0_out=i; usb_wr(USB_DOEPINT(0), i); }
        if (i & USB_DXEPINT_XFERCOMPL) ++diag_ep0_out_done;
        if (i & USB_DXEPINT_SETUP) {
            dma_from_device_done(s,g_ep0_setup, 8);
            ++diag_ep0_setups;
            diag_last_setup_a=((ULONG)g_ep0_setup[0]<<24)|((ULONG)g_ep0_setup[1]<<16)|
                              ((ULONG)g_ep0_setup[2]<<8)|g_ep0_setup[3];
            diag_last_setup_b=((ULONG)g_ep0_setup[4]<<24)|((ULONG)g_ep0_setup[5]<<16)|
                              ((ULONG)g_ep0_setup[6]<<8)|g_ep0_setup[7];
            handle_setup(s,g_ep0_setup);
        }
        if (i & USB_DXEPINT_XFERCOMPL) {
            if (ep0_sm_state == EP0_OUT_STATUS) {
                ep0_sm_state = EP0_IDLE;
                ep0_arm_setup(s);
            } else if (ep0_sm_state == EP0_OUT_DATA) {
                dma_from_device_done(s,g_ep0_out, ep0_out_expected ? ep0_out_expected : 1u);
                ep0_sm_state = EP0_IN_STATUS;
                ep0_zlp(s);
            }
        }
    }
    if (daint & USB_DAINT_INEP(0)) {
        uint32_t i = usb_rd(USB_DIEPINT(0));
        if (i) { diag_last_ep0_in=i; usb_wr(USB_DIEPINT(0), i); }
        if (i & USB_DXEPINT_XFERCOMPL) ++diag_ep0_in_done;
        if (i & USB_DXEPINT_XFERCOMPL) {
            if (ep0_sm_state == EP0_IN_STATUS) {
                dma_finish(s,g_ep0_tx);
                ep0_sm_state = EP0_IDLE;
                ep0_arm_setup(s);
            } else if (ep0_sm_state == EP0_IN_DATA) {
                dma_finish(s,g_ep0_tx + (ep0_tx_pos ? ((ep0_tx_pos-1u)/64u)*64u : 0));
                if (ep0_tx_pos < ep0_tx_len)
                    ep0_send_next(s);
                else {
                    ep0_sm_state = EP0_OUT_STATUS;
                    ep0_arm_out(s,0);
                }
            }
        }
    }
    if (daint & USB_DAINT_OUTEP(USB_EP_OUT)) {
        uint32_t i = usb_rd(USB_DOEPINT(USB_EP_OUT));
        if (i) usb_wr(USB_DOEPINT(USB_EP_OUT), i);
        if ((i & USB_DXEPINT_XFERCOMPL) && bulk_out_armed) {
            uint32_t programmed = bulk_out_programmed;
            uint32_t residual = usb_rd(USB_DOEPTSIZ(USB_EP_OUT)) & 0x7ffffu;
            uint32_t got = residual <= programmed ? programmed - residual : 0;
            bulk_out_armed = 0;
            bulk_out_programmed = 0;
            dma_from_device_done(s,g_bulk_out_pkt, programmed);
            usb_rx_packet(s,got);
            if (usb_configured && !bulk_out_armed)
                bulk_out_arm(s);
        }
    }
    if (daint & USB_DAINT_INEP(USB_EP_IN)) {
        uint32_t i = usb_rd(USB_DIEPINT(USB_EP_IN));
        if (i) usb_wr(USB_DIEPINT(USB_EP_IN), i);
        if ((i & USB_DXEPINT_XFERCOMPL) && bulk_in_active) {
            dma_finish(s,bulk_in_stage == BIN_HEADER ? g_req_hdr : g_payload);
            usb_bulk_in_complete(s);
        }
    }
}

static int usb_hw_init(struct RDiskState *s)
{
    uint32_t v;
    if (usb_hw_ready)
        return 1;
    diag_mark(s,RD_STAGE_SIGNALS);
    if (!usb_power_on(s)) { diag_fail(s,RD_FAIL_POWER); return 0; }
    diag_mark(s,RD_STAGE_POWER);
    usb_delay(1000000u);
    v = usb_rd(USB_GSNPSID);
    diag_snpsid=v;
    if ((v & 0xffff0000u) != 0x4f540000u) {
        diag_fail(s,RD_FAIL_ID); return 0;
    }
    diag_mark(s,RD_STAGE_ID);
    usb_wr(USB_DCTL, usb_rd(USB_DCTL) | USB_DCTL_SFTDISCON);
    diag_mark(s,RD_STAGE_DISCONNECT);
    v = usb_rd(USB_GUSBCFG);
    v &= ~(USB_GUSBCFG_FORCEHOSTMODE | USB_GUSBCFG_HNPCAP | USB_GUSBCFG_SRPCAP | USB_GUSBCFG_TOUTCAL_MASK);
    v |= USB_GUSBCFG_FORCEDEVMODE | 7u;
    usb_wr(USB_GUSBCFG, v);
    usb_delay(1000000u);
    diag_mark(s,RD_STAGE_MODE);
    if (!usb_wait_mask(USB_GRSTCTL, USB_GRSTCTL_AHBIDLE, USB_GRSTCTL_AHBIDLE, 5000000u)) {
        diag_fail(s,RD_FAIL_AHBIDLE); return 0;
    }
    diag_mark(s,RD_STAGE_AHBIDLE);
    usb_wr(USB_GRSTCTL, USB_GRSTCTL_CSFTRST);
    if (!usb_wait_mask(USB_GRSTCTL, USB_GRSTCTL_CSFTRST, 0, 5000000u)) {
        diag_fail(s,RD_FAIL_CORE_RESET); return 0;
    }
    diag_mark(s,RD_STAGE_CORE_RESET);
    usb_delay(250000u);
    v = usb_rd(USB_GUSBCFG);
    v &= ~USB_GUSBCFG_FORCEHOSTMODE;
    v |= USB_GUSBCFG_FORCEDEVMODE;
    usb_wr(USB_GUSBCFG, v);
    usb_delay(1000000u);
    if (usb_rd(USB_GINTSTS) & USB_GINTSTS_CURMODE_HOST) {
        diag_fail(s,RD_FAIL_HOST_MODE); return 0;
    }
    diag_mark(s,RD_STAGE_DEVICE);

    v = usb_rd(USB_GAHBCFG);
    v &= ~USB_GAHBCFG_GLBL_INTR_EN;
    v |= USB_GAHBCFG_DMA_EN;
    usb_wr(USB_GAHBCFG, v);
    usb_wr(USB_GINTMSK, 0);
    usb_wr(USB_GRXFSIZ, 256u);
    usb_wr(USB_GNPTXFSIZ, (128u << 16) | 256u);
    usb_wr(USB_DPTXFSIZ(USB_EP_IN), (512u << 16) | 384u);
    usb_flush_fifos();
    v = usb_rd(USB_DCFG);
    v &= ~(USB_DCFG_DEVADDR_MASK | USB_DCFG_DEVSPD_MASK);
    usb_wr(USB_DCFG, v);
    usb_wr(USB_DIEPMSK, USB_DXEPINT_XFERCOMPL);
    usb_wr(USB_DOEPMSK, USB_DXEPINT_XFERCOMPL | USB_DXEPINT_SETUP);
    usb_hw_ready = 1;
    diag_mark(s,RD_STAGE_CORE_READY);
    usb_on_reset(s);
    usb_wr(USB_GINTMSK, USB_GINTSTS_USBRST | USB_GINTSTS_ENUMDONE |
           USB_GINTSTS_IEPINT | USB_GINTSTS_OEPINT);
    /* Polling POC2: keep the DWC2 global IRQ gate disabled. */
    usb_wr(USB_DCTL, usb_rd(USB_DCTL) & ~USB_DCTL_SFTDISCON);
    /* Runtime device: no serial console requirement. */
    return 1;
}

void rdisk_usb_poll(struct RDiskState *s)
{
    uint32_t g;
    if (!usb_hw_ready) return;
    ++diag_polls;
    g = usb_rd(USB_GINTSTS);
    diag_last_gintsts=g;
    diag_last_masked=g & usb_rd(USB_GINTMSK);
    if (g & USB_GINTSTS_DISCONNINT) {
        ++diag_disconnects;
        usb_wr(USB_GINTSTS,USB_GINTSTS_DISCONNINT);
        /* The old media/transaction is no longer valid, even if the host
         * has not yet delivered a new USBRST/SET_CONFIGURATION sequence. */
        usb_reset_state(s);
    }
    if (g & USB_GINTSTS_USBSUSP) {
        ++diag_suspends;
        usb_wr(USB_GINTSTS,USB_GINTSTS_USBSUSP);
    }
    if (g & USB_GINTSTS_WKUPINT) {
        ++diag_wakeups;
        usb_wr(USB_GINTSTS,USB_GINTSTS_WKUPINT);
    }
    if (g & USB_GINTSTS_USBRST) {
        usb_wr(USB_GINTSTS, USB_GINTSTS_USBRST);
        ++diag_resets;diag_mark(s,RD_STAGE_USB_RESET);
        usb_on_reset(s);
    }
    if (g & USB_GINTSTS_ENUMDONE) {
        usb_wr(USB_GINTSTS, USB_GINTSTS_ENUMDONE);
        ++diag_enums;diag_mark(s,RD_STAGE_ENUM_DONE);
        ep0_arm_setup(s);
    }
    if (g & (USB_GINTSTS_IEPINT | USB_GINTSTS_OEPINT))
        usb_poll_epints(s);

    /* The first IDENT may be sent between SET_CONFIGURATION and the host's
     * WinUSB ReadPipe. It must never occupy the transport forever. */
    /* Do not replace an IN DMA transfer still owned by DWC2. If Windows has
     * not opened the pipe yet, the original IDENT remains queued in EP2 and
     * will complete when the host reads it. Only retry after IN completed but
     * the companion never supplied the corresponding OUT response. */
    if (g_txn.active && g_txn.internal_ident && !bulk_in_active &&
        bulk_in_stage == BIN_WAIT_RESPONSE &&
        (uint32_t)(diag_vblanks - g_ident_started_vblank) >=
            RDISK_IDENT_RETRY_VBLANKS) {
        ++g_ident_timeouts;
        usb_txn_fail(s,-42);
    }
    if (usb_configured && !g_txn.active && !bulk_in_active && g_total_blocks == 0)
        usb_start_txn(s,RDISK_CMD_IDENT, ++g_internal_seq, 0, 0, 0, 0, 1);
    if (usb_irq_added && usb_hw_ready)
        usb_wr(USB_GAHBCFG,usb_rd(USB_GAHBCFG)|USB_GAHBCFG_GLBL_INTR_EN);
}


/* EXTER IRQ is shared with other Amiga interrupt sources.  Claim USB only. */
#define BCM_IRQ_PENDING1_ADDR 0xF200B204u
#define BCM_IRQ_ENABLE1_ADDR  0xF200B210u
#define BCM_IRQ_DISABLE1_ADDR 0xF200B21Cu
#define BCM_IRQ_USB_BIT (1u<<9)
#define RDISK_GINTMSK (USB_GINTSTS_USBRST|USB_GINTSTS_ENUMDONE|USB_GINTSTS_IEPINT|USB_GINTSTS_OEPINT|USB_GINTSTS_DISCONNINT|USB_GINTSTS_USBSUSP|USB_GINTSTS_WKUPINT)
static ULONG __attribute__((used)) usb_irq_handler(APTR data asm("a1"))
{
    struct RDiskState *s=(struct RDiskState *)data;
    uint32_t pending;
    if(!s) return 0;
    ++diag_irq_calls;
    if(!usb_irq_added || !usb_hw_ready) return 0;
    if(!(raw_rd(BCM_IRQ_PENDING1_ADDR)&BCM_IRQ_USB_BIT))return 0;
    pending=usb_rd(USB_GINTSTS)&usb_rd(USB_GINTMSK)&RDISK_GINTMSK;
    if(!pending) { ++diag_irq_no_dwc_pending; return 0; }
    ++diag_irq_claimed;
    usb_wr(USB_GAHBCFG,usb_rd(USB_GAHBCFG)&~USB_GAHBCFG_GLBL_INTR_EN);
    if(wake_task && wake_mask)exec_Signal(s->m_sysbase,wake_task,wake_mask);
    *(volatile UWORD *)0xDFF09CUL=(UWORD)0x2000u;
    return 1;
}
static ULONG __attribute__((used)) usb_vblank_handler(APTR data asm("a1"))
{
    struct RDiskState *s=(struct RDiskState *)data;
    if(!s) return 0;
    ++diag_vblanks;
    if(wake_task && wake_mask)exec_Signal(s->m_sysbase,wake_task,wake_mask);
    return 0;
}
int rdisk_usb_init(struct RDiskState *s,struct Task *task,ULONG signal_mask)
{
    if(!s || !s->m_sysbase || !task || !signal_mask) return 0;
    diag_stage=RD_STAGE_WORKER;diag_failure=RD_FAIL_NONE;
    diag_snpsid=0;diag_resets=0;diag_enums=0;diag_configs=0;
    diag_irq_calls=diag_irq_claimed=diag_irq_no_dwc_pending=0;
    diag_vblanks=diag_worker_wakes=diag_polls=0;
    diag_ep0_setups=diag_ep0_in_done=diag_ep0_out_done=0;
    diag_last_ep0_in=diag_last_ep0_out=0;
    diag_last_gintsts=diag_last_masked=0;
    diag_last_setup_a=diag_last_setup_b=0;
    diag_disconnects=diag_suspends=diag_wakeups=0;
    wake_task=task;wake_mask=signal_mask;
    rdisk_done=0;rdisk_result=-1;
    usb_reset_state(s);
    if(!usb_hw_init(s))return 0;
    usb_irq.is_Node.ln_Type=NT_INTERRUPT;
    usb_irq.is_Node.ln_Pri=5;
    usb_irq.is_Node.ln_Name=(STRPTR)"l2scsi DWC2";
    usb_irq.is_Data=(APTR)s;
    usb_irq.is_Code=(void (*)())usb_irq_handler;
    usb_vblank.is_Node.ln_Type=NT_INTERRUPT;
    usb_vblank.is_Node.ln_Pri=0;
    usb_vblank.is_Node.ln_Name=(STRPTR)"l2scsi fallback";
    usb_vblank.is_Data=(APTR)s;
    usb_vblank.is_Code=(void (*)())usb_vblank_handler;
    exec_AddIntServer(s->m_sysbase,INTB_VERTB,&usb_vblank);
    vblank_added=1;
    exec_AddIntServer(s->m_sysbase,INTB_EXTER,&usb_irq);
    usb_irq_added=1;
    usb_wr(USB_GINTMSK,RDISK_GINTMSK);
    raw_wr(BCM_IRQ_ENABLE1_ADDR,BCM_IRQ_USB_BIT);
    usb_wr(USB_GAHBCFG,usb_rd(USB_GAHBCFG)|USB_GAHBCFG_GLBL_INTR_EN);
    diag_mark(s,RD_STAGE_IRQ_READY);
    return 1;
}
void rdisk_usb_note_worker_wake(struct RDiskState *s) { ++diag_worker_wakes; }
/* Called ONLY by the sole worker. Turns the first caller's request into its
 * own bounded bootstrap instead of returning an immediate "not ready" while
 * the IRQ/VBlank/WinUSB/IDENT state machines are still progressing. */
int rdisk_usb_wait_ready(struct RDiskState *s)
{
    const uint32_t started = diag_vblanks;
    if (!usb_hw_ready) return -1;
    for (;;) {
        rdisk_usb_poll(s);
        if (usb_hw_ready && usb_configured && g_total_blocks != 0 &&
            g_block_size == RDISK_POC_BLOCK_SIZE && !g_txn.active &&
            !bulk_in_active)
            return 0;
        if ((uint32_t)(diag_vblanks - started) >= RDISK_READY_VBLANKS)
            return -1;
        exec_Wait(s->m_sysbase,wake_mask); /* VBlank guarantees progress even without USB IRQ. */
    }
}
int rdisk_usb_geometry(struct RDiskState *s,uint32_t *blocks,uint32_t *block_size,uint32_t *features)
{
    if(!usb_hw_ready || !usb_configured || !g_total_blocks)return -1;
    *blocks=g_total_blocks;
    *block_size=g_block_size;
    *features=g_features;
    return 0;
}
/* Called only by the sole Exec worker. IRQs wake it; completion stays in task context.
 * Around 10 seconds at 50Hz fallback, NOT an unbounded spin inside BeginIO. */
int rdisk_usb_submit(struct RDiskState *s,uint32_t cmd,uint32_t seq,uint32_t lba,
                     uint32_t blocks,void *buffer,uint32_t length)
{
    const uint32_t started=diag_vblanks;
    uint32_t precheck=0;
    if(!usb_hw_ready) precheck|=RD_PRE_NO_HW;
    if(!usb_configured) precheck|=RD_PRE_NO_CONFIG;
    if(!g_total_blocks) precheck|=RD_PRE_NO_GEOMETRY;
    if(g_txn.active) precheck|=RD_PRE_TXN_ACTIVE;
    if(length>RDISK_USB_MAX_BYTES) precheck|=RD_PRE_TOO_LARGE;
    if(length && !buffer) precheck|=RD_PRE_NULL_BUFFER;
    if(precheck) {
        read_diag_latch_request(s,-100,cmd,seq,lba,blocks,length,precheck);
        return -1;
    }
    rdisk_done=0;rdisk_result=-1;
    usb_start_txn(s,cmd,seq,lba,blocks,length,(uint32_t)(uintptr_t)buffer,0);
    while(!rdisk_done &&
          (uint32_t)(diag_vblanks-started)<500u) {
        rdisk_usb_poll(s);
        if(!rdisk_done)exec_Wait(s->m_sysbase,wake_mask);
    }
    if(!rdisk_done){
        usb_txn_fail(s,-41);
        return -1;
    }
    return rdisk_result==0?0:-1;
}

/* The state-access aliases above intentionally use short names such as
 * read_fail_lba.  RDiskDiag exposes fields with those same names, so leaving
 * the aliases active here would make the preprocessor rewrite tokens in
 * expressions such as out->read_fail_lba.  The getter is the final user of
 * this diagnostic state in this translation unit: drop only the colliding
 * aliases and access the RDiskState members explicitly below. */
#undef read_fail_latched
#undef read_fail_code
#undef read_fail_lba
#undef read_fail_blocks
#undef read_fail_bytes
#undef read_fail_seq
#undef read_fail_rx_len
#undef read_fail_rx_expected
#undef read_fail_bulk_out_programmed
#undef read_fail_doeptsiz
#undef read_fail_resp_magic
#undef read_fail_resp_version
#undef read_fail_resp_cmd
#undef read_fail_resp_seq
#undef read_fail_resp_result
#undef read_fail_resp_length
#undef read_fail_precheck
#undef read_stale_seen
#undef read_stale_cmd
#undef read_stale_seq
#undef read_stale_result
#undef read_stale_length
#undef read_stale_rx_len
#undef read_stale_rx_expected

/* Query never touches MMIO until a successful hardware initialization.
 * If a timeout occurs while the worker is initializing, stage/failure still
 * remain readable through the synchronous BeginIO diagnostic escape hatch. */
void rdisk_usb_get_diag(struct RDiskState *s,struct RDiskDiag *out)
{
    if (!out) return;
    l2_memset(out,0,sizeof(*out));
    out->magic=RDISK_DIAG_MAGIC;out->version=RDISK_DIAG_VERSION;
    if (!s) return;
    out->stage=diag_stage;out->failure=diag_failure;
    out->hw_ready=usb_hw_ready;out->configured=usb_configured;
    out->blocks=g_total_blocks;out->snpsid=diag_snpsid;
    out->usb_resets=diag_resets;out->enum_dones=diag_enums;
    out->config_events=diag_configs;
    out->irq_calls=diag_irq_calls;out->irq_claimed=diag_irq_claimed;
    out->irq_no_dwc_pending=diag_irq_no_dwc_pending;
    out->vblanks=diag_vblanks;out->worker_wakes=diag_worker_wakes;
    out->usb_polls=diag_polls;
    out->ep0_setups=diag_ep0_setups;out->ep0_in_done=diag_ep0_in_done;
    out->ep0_out_done=diag_ep0_out_done;
    out->last_ep0_in=diag_last_ep0_in;out->last_ep0_out=diag_last_ep0_out;
    out->last_gintsts=diag_last_gintsts;out->last_masked=diag_last_masked;
    out->last_setup_a=diag_last_setup_a;out->last_setup_b=diag_last_setup_b;
    out->disconnects=diag_disconnects;out->suspends=diag_suspends;
    out->wakeups=diag_wakeups;out->ep0_state=ep0_sm_state;

    out->read_fail_latched=s->m_read_fail_latched;
    out->read_fail_transport_code=s->m_read_fail_code;
    out->read_fail_lba=s->m_read_fail_lba;
    out->read_fail_blocks=s->m_read_fail_blocks;
    out->read_fail_bytes=s->m_read_fail_bytes;
    out->read_fail_seq=s->m_read_fail_seq;
    out->read_fail_rx_len=s->m_read_fail_rx_len;
    out->read_fail_rx_expected=s->m_read_fail_rx_expected;
    out->read_fail_bulk_out_programmed=s->m_read_fail_bulk_out_programmed;
    out->read_fail_doeptsiz=s->m_read_fail_doeptsiz;
    out->read_fail_resp_magic=s->m_read_fail_resp_magic;
    out->read_fail_resp_version=s->m_read_fail_resp_version;
    out->read_fail_resp_cmd=s->m_read_fail_resp_cmd;
    out->read_fail_resp_seq=s->m_read_fail_resp_seq;
    out->read_fail_resp_result=s->m_read_fail_resp_result;
    out->read_fail_resp_length=s->m_read_fail_resp_length;
    out->read_fail_precheck=s->m_read_fail_precheck;
    out->read_stale_seen=s->m_read_stale_seen;
    out->read_stale_cmd=s->m_read_stale_cmd;
    out->read_stale_seq=s->m_read_stale_seq;
    out->read_stale_result=s->m_read_stale_result;
    out->read_stale_length=s->m_read_stale_length;
    out->read_stale_rx_len=s->m_read_stale_rx_len;
    out->read_stale_rx_expected=s->m_read_stale_rx_expected;
    if (usb_hw_ready) {
        out->gintsts=usb_rd(USB_GINTSTS);
        out->gintmsk=usb_rd(USB_GINTMSK);
        out->dcfg=usb_rd(USB_DCFG);
        out->daint=usb_rd(USB_DAINT);
        out->daintmsk=usb_rd(USB_DAINTMSK);
        out->diepmsk=usb_rd(USB_DIEPMSK);
        out->doepmsk=usb_rd(USB_DOEPMSK);
        out->diepctl0=usb_rd(USB_DIEPCTL(0));
        out->doepctl0=usb_rd(USB_DOEPCTL(0));
        out->doeptsiz0=usb_rd(USB_DOEPTSIZ(0));
        out->gahbcfg=usb_rd(USB_GAHBCFG);
        out->gusbcfg=usb_rd(USB_GUSBCFG);
        out->dctl=usb_rd(USB_DCTL);
        out->dsts=usb_rd(USB_DSTS);
        out->grstctl=usb_rd(USB_GRSTCTL);
    }
}
