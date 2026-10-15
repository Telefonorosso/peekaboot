/*
 * l2scsi-device-romsafe-v43r115.c
 *
 * Kickstart Extended-ROM-safe COLDSTART fork of the hardware-validated v43r108 Direct-DWC2 device.
 *
 * Deliberately contains NO:
 *  - 3Com / card.resource code
 *  - Ethernet / L2HD framing
 *  - legacy Ethernet worker and framing
 *  - caches
 *  - legacy Ethernet diagnostics (USB diagnostics retained)
 *
 * Backend:
 *  Direct BCM2837 DWC2 in AmigaOS, vendor-specific USB bulk / WinUSB.
 *
 * BeginIO queues requests to an Exec worker; the worker alone owns DWC2
 * transactions and replies to the I/O request after completion.
 */

#include <exec/types.h>
#include <exec/devices.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/ports.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <stdint.h>

/* ROMSAFE: do not pull libnix memcpy/memset into a ROM Resident.
 * libnix' implementations use the traditional global SysBase, which is
 * deliberately absent in this ROM-safe build. Keep these primitives fully
 * local and freestanding. */
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
    const UBYTE *s=(const UBYTE *)src;
    while(bytes--) *d++=*s++;
    return dst;
}

/* Direct Exec LVO wrappers retained from the hardware-validated 43.102
 * worker.  Transport is implemented separately in rdisk_direct.c. */
static void exec_ReplyMsg(struct ExecBase *SysBase, struct Message *msg)
{
    register struct ExecBase *a6 __asm("a6") = SysBase;
    register struct Message *a1 __asm("a1") = msg;
    __asm volatile ("jsr -378(a6)"
                    :
                    : "r"(a6), "r"(a1)
                    : "d0","d1","a0","a1","memory");
}

/* Exec wrappers used by the private worker; no implicit ExecBase. */
static APTR exec_AllocMem(struct ExecBase *sys, ULONG bytes, ULONG flags)
{
    register struct ExecBase *a6 __asm("a6") = sys;
    register ULONG d0 __asm("d0") = bytes;
    register ULONG d1 __asm("d1") = flags;
    register APTR out __asm("d0");
    __asm volatile ("jsr -198(a6)" : "=r"(out)
                    : "r"(a6), "r"(d0), "r"(d1)
                    : "a0", "a1", "cc", "memory");
    return out;
}

static void exec_AddTask(struct ExecBase *sys, struct Task *task, APTR entry)
{
    register struct ExecBase *a6 __asm("a6") = sys;
    register struct Task *a1 __asm("a1") = task;
    register APTR a2 __asm("a2") = entry;
    register APTR a3 __asm("a3") = NULL;
    __asm volatile ("jsr -282(a6)" :
                    : "r"(a6), "r"(a1), "r"(a2), "r"(a3)
                    : "d0", "d1", "a0", "cc", "memory");
}

static struct Task *exec_FindTask(struct ExecBase *sys)
{
    register struct ExecBase *a6 __asm("a6") = sys;
    register STRPTR a1 __asm("a1") = NULL;
    register struct Task *out __asm("d0");
    __asm volatile ("jsr -294(a6)" : "=r"(out)
                    : "r"(a6), "r"(a1)
                    : "d1", "a0", "cc", "memory");
    return out;
}

static BYTE exec_AllocSignal(struct ExecBase *sys)
{
    register struct ExecBase *a6 __asm("a6") = sys;
    register LONG d0 __asm("d0") = -1;
    __asm volatile ("jsr -330(a6)" : "+r"(d0)
                    : "r"(a6)
                    : "d1", "a0", "a1", "cc", "memory");
    return (BYTE)d0;
}

static ULONG exec_Wait(struct ExecBase *sys, ULONG mask)
{
    register struct ExecBase *a6 __asm("a6") = sys;
    register ULONG d0 __asm("d0") = mask;
    __asm volatile ("jsr -318(a6)" : "+r"(d0)
                    : "r"(a6)
                    : "d1", "a0", "a1", "cc", "memory");
    return d0;
}

static struct Message *exec_GetMsg(struct ExecBase *sys, struct MsgPort *port)
{
    register struct ExecBase *a6 __asm("a6") = sys;
    register struct MsgPort *a0 __asm("a0") = port;
    register struct Message *out __asm("d0");
    __asm volatile ("jsr -372(a6)" : "=r"(out)
                    : "r"(a6), "r"(a0)
                    : "d1", "a1", "cc", "memory");
    return out;
}

static void exec_PutMsg(struct ExecBase *sys, struct MsgPort *port,
                        struct Message *msg)
{
    register struct ExecBase *a6 __asm("a6") = sys;
    register struct MsgPort *a0 __asm("a0") = port;
    register struct Message *a1 __asm("a1") = msg;
    __asm volatile ("jsr -366(a6)" :
                    : "r"(a6), "r"(a0), "r"(a1)
                    : "d0", "d1", "cc", "memory");
}

static void exec_NewList(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

#define DEVNAME "l2scsi.device"
#define DEVVER  43
#define DEVREV  115
#define L2_WORKER_STACK_LONGS 4096UL

#define RDISK_CMD_IDENT  1UL
#define RDISK_CMD_READ   2UL
#define RDISK_CMD_WRITE  3UL
#define RDISK_CMD_FLUSH  4UL

#define ST_READY 0x01UL
#define ST_BUSY  0x02UL
#define ST_DONE  0x04UL
#define ST_ERROR 0x08UL

#define FEATURE_WRITE 0x00000001UL

#define SECTOR_SIZE 512UL
#define MAX_BLOCKS_PER_IO 256UL
#ifndef PA_IGNORE
#define PA_IGNORE 2 /* Queue messages before worker signal is allocated. */
#endif

#ifndef IOERR_OPENFAIL
#define IOERR_OPENFAIL   (-1)
#endif
#ifndef IOERR_NOCMD
#define IOERR_NOCMD      (-3)
#endif
#ifndef IOERR_BADLENGTH
#define IOERR_BADLENGTH  (-4)
#endif
#ifndef IOERR_BADADDRESS
#define IOERR_BADADDRESS (-5)
#endif
/* ptable.library uses the new-style 64-bit trackdisk READ command. */
#ifndef NSCMD_TD_READ64
#define NSCMD_TD_READ64 0xc000u
#endif

#ifndef HD_SCSICMD
#define HD_SCSICMD 0x1c
#endif
#ifndef HFERR_BadStatus
#define HFERR_BadStatus 45
#endif

#ifndef TD_FORMAT
#define TD_FORMAT 11
#endif

#ifndef TD_GETGEOMETRY
#define TD_GETGEOMETRY 22
#endif

#ifndef TD_PROTSTATUS
#define TD_PROTSTATUS 15
#endif

#ifndef TD_CHANGESTATE
#define TD_CHANGESTATE 14
#endif

#ifndef TD_CHANGENUM
#define TD_CHANGENUM 13
#endif

#ifndef CMD_FLUSH
#define CMD_FLUSH 9
#endif

struct L2WorkerBlock {
    struct Task task;
    struct MsgPort port;
    ULONG stack[L2_WORKER_STACK_LONGS]; /* 16 KiB, RAM, aligned */
};

struct L2ScsiBase {
    struct Device dev;
    struct ExecBase *SysBase;
    struct Unit unit;
    struct L2WorkerBlock *worker;
    APTR rdisk_mem;                  /* raw MEMF_PUBLIC block, retained for device lifetime */
    struct RDiskState *rdisk;       /* 128-byte-aligned mutable backend state in RAM */
    volatile ULONG worker_ready; /* message queue published and safe to use */
    volatile ULONG worker_failed; /* DWC2 initialisation could not complete */


    ULONG sequence;
    ULONG total_blocks;
    ULONG block_size;
    ULONG features;
    ULONG open_count;
    /* Logical trackdisk motor state. There is no physical motor on the
     * remote HDF, but TD_MOTOR requires the previous state in io_Actual.
     * Media access automatically turns this state back on, matching
     * trackdisk.device semantics. */
    ULONG motor_on;

    /* v43r115: the remote HDF has no device-side write-back cache, but the
     * host OS may still cache successful WriteFile() data. Track whether a
     * successful remote WRITE has occurred since the last successful FLUSH.
     * Clean CMD_UPDATE/CMD_FLUSH/SYNCHRONIZE CACHE therefore complete locally
     * instead of issuing a redundant, potentially long FlushFileBuffers(). */
    ULONG media_dirty;

    /* v43r114: first READ failure at the device/AmigaDOS boundary. */
    ULONG diag_read_fail_latched;
    ULONG diag_read_fail_stage;
    LONG  diag_read_fail_io_error;
    ULONG diag_read_fail_offset;
    ULONG diag_read_fail_length;
    ULONG diag_read_fail_actual;
    ULONG diag_read_fail_lba;
    ULONG diag_read_fail_blocks;

    UBYTE sense_key, sense_asc, sense_ascq;
};

extern struct Resident ROMTag;
extern const ULONG DevInitTab[];

static const char dev_id[] =
    "$VER: l2scsi.device 43.115 (28.09.2026) ROMSAFE DIRTY-FLUSH FIX READFAIL-DIAG";

/* Direct DWC2 transport, no Emu68 Z3 MMIO mailbox. */
#include "rdisk_direct.h"

static void l2_motor_note_access(struct L2ScsiBase *b)
{
    if (b) b->motor_on = 1;
}

static void do_motor(struct L2ScsiBase *b, struct IOStdReq *io)
{
    ULONG previous = b->motor_on ? 1UL : 0UL;

    /* trackdisk.device specifies io_Length 0=off, 1=on. Treat any
     * non-zero value as on rather than rejecting legacy callers. */
    b->motor_on = io->io_Length ? 1UL : 0UL;
    io->io_Actual = previous;
}

static void driver_read_fail_latch(struct L2ScsiBase *b, ULONG stage,
                                   LONG io_error, ULONG offset, ULONG length,
                                   ULONG actual, ULONG lba, ULONG blocks)
{
    if (!b || b->diag_read_fail_latched) return;
    b->diag_read_fail_latched=1;
    b->diag_read_fail_stage=stage;
    b->diag_read_fail_io_error=io_error;
    b->diag_read_fail_offset=offset;
    b->diag_read_fail_length=length;
    b->diag_read_fail_actual=actual;
    b->diag_read_fail_lba=lba;
    b->diag_read_fail_blocks=blocks;
}

static void driver_diag_export(struct L2ScsiBase *b, struct RDiskDiag *d)
{
    if (!b || !d) return;
    d->driver_read_fail_latched=b->diag_read_fail_latched;
    d->driver_read_fail_stage=b->diag_read_fail_stage;
    d->driver_read_fail_io_error=b->diag_read_fail_io_error;
    d->driver_read_fail_offset=b->diag_read_fail_offset;
    d->driver_read_fail_length=b->diag_read_fail_length;
    d->driver_read_fail_actual=b->diag_read_fail_actual;
    d->driver_read_fail_lba=b->diag_read_fail_lba;
    d->driver_read_fail_blocks=b->diag_read_fail_blocks;
}
static LONG direct_submit(struct L2ScsiBase *b,ULONG cmd,ULONG lba,
                           ULONG blocks,APTR buffer,ULONG bytes)
{
    if(!b || !b->worker_ready || !b->rdisk)return -1;
    return rdisk_usb_submit(b->rdisk,cmd,++b->sequence,lba,blocks,buffer,bytes);
}

/* v43r115: SCSI/trackdisk-style cache discipline. A clean device has nothing
 * to synchronize, so acknowledge UPDATE/FLUSH locally. A dirty device sends
 * exactly one remote FLUSH and becomes clean only after its successful reply.
 * This is not a retry and does not alter READ timing or the wire protocol. */
static LONG direct_flush_dirty(struct L2ScsiBase *b)
{
    if (!b || !(b->features & FEATURE_WRITE) || !b->media_dirty)
        return 0;
    if (direct_submit(b,RDISK_CMD_FLUSH,0,0,NULL,0)!=0)
        return -1;
    b->media_dirty=0;
    return 0;
}
static LONG refresh_geometry(struct L2ScsiBase *b)
{
    uint32_t blocks,size,features;
    if (b->worker_failed || !b->rdisk || rdisk_usb_wait_ready(b->rdisk) != 0 ||
        rdisk_usb_geometry(b->rdisk,&blocks,&size,&features)!=0) return -1;
    b->total_blocks=blocks;b->block_size=size;b->features=features;
    return size==SECTOR_SIZE && blocks!=0?0:-1;
}

/* Minimal SCSI direct-access layer based on Ethernet v43r23, not a separate
 * USB SCSI protocol: all media I/O still goes through the same sole worker.
 * HDToolBox opens unit 0 and uses HD_SCSICMD rather than TD_GETGEOMETRY. */
#define L2_SCSI_GOOD    0x00u
#define L2_SCSI_CHECK   0x02u
#define L2_SENSE_READY  0x02u
#define L2_SENSE_MEDIUM 0x03u
#define L2_SENSE_ILLEGAL 0x05u
#define L2_SENSE_PROTECT 0x07u
static void scsi_be32_put(UBYTE *p, ULONG v)
{ p[0]=(UBYTE)(v>>24);p[1]=(UBYTE)(v>>16);p[2]=(UBYTE)(v>>8);p[3]=(UBYTE)v; }
static ULONG scsi_be32_get(const UBYTE *p)
{ return ((ULONG)p[0]<<24)|((ULONG)p[1]<<16)|((ULONG)p[2]<<8)|p[3]; }
static UWORD scsi_be16_get(const UBYTE *p)
{ return (UWORD)(((UWORD)p[0]<<8)|p[1]); }
static void scsi_sense(struct L2ScsiBase *b, UBYTE key, UBYTE asc)
{ b->sense_key=key;b->sense_asc=asc;b->sense_ascq=0; }
static ULONG scsi_request_sense(struct L2ScsiBase *b, UBYTE *data, ULONG size)
{
    ULONG n=size<18 ? size : 18;
    if(n && data) {
        l2_memset(data,0,n);
        if(n>0)data[0]=0x70;
        if(n>2)data[2]=b->sense_key;
        if(n>7)data[7]=10;
        if(n>12)data[12]=b->sense_asc;
        if(n>13)data[13]=b->sense_ascq;
    }
    scsi_sense(b,0,0);
    return n;
}
static int scsi_rw(struct L2ScsiBase *b, UBYTE *data, ULONG capacity,
                   ULONG lba, ULONG blocks, int write, ULONG *actual)
{
    ULONG done=0;
    *actual=0;
    if (!blocks) return L2_SCSI_GOOD;
    l2_motor_note_access(b);
    if (!data || blocks>0xffffffffUL/SECTOR_SIZE ||
        blocks*SECTOR_SIZE>capacity || lba>=b->total_blocks ||
        blocks>b->total_blocks-lba) {
        scsi_sense(b,L2_SENSE_ILLEGAL,0x21);
        return L2_SCSI_CHECK;
    }
    if (write && !(b->features & FEATURE_WRITE)) {
        scsi_sense(b,L2_SENSE_PROTECT,0x27);
        return L2_SCSI_CHECK;
    }
    while (blocks) {
        ULONG chunk=blocks>MAX_BLOCKS_PER_IO?MAX_BLOCKS_PER_IO:blocks;
        ULONG bytes=chunk*SECTOR_SIZE;
        if (direct_submit(b,write?RDISK_CMD_WRITE:RDISK_CMD_READ,
                          lba,chunk,data+done,bytes)!=0) {
            *actual=done;
            if (!write)
                driver_read_fail_latch(b,3,0,lba*SECTOR_SIZE,
                                       blocks*SECTOR_SIZE,done,lba,chunk);
            scsi_sense(b,L2_SENSE_MEDIUM,write?0x0c:0x11);
            return L2_SCSI_CHECK;
        }
        if (write) b->media_dirty=1;
        done+=bytes;lba+=chunk;blocks-=chunk;
    }
    *actual=done;
    scsi_sense(b,0,0);
    return L2_SCSI_GOOD;
}
static void do_scsi(struct L2ScsiBase *b, struct IOStdReq *io)
{
    struct SCSICmd *sc;
    const UBYTE *c;
    UBYTE *d;
    ULONG actual=0, lba, blocks, n;
    int status=L2_SCSI_GOOD;
    if (!io->io_Data || (io->io_Length &&
                         io->io_Length<sizeof(struct SCSICmd))) {
        io->io_Error=IOERR_BADLENGTH;
        return;
    }
    sc=(struct SCSICmd *)io->io_Data;
    sc->scsi_Actual=0;
    sc->scsi_SenseActual=0;
    sc->scsi_Status=L2_SCSI_GOOD;
    if (!sc->scsi_Command || !sc->scsi_CmdLength ||
        (sc->scsi_Length && !sc->scsi_Data)) {
        io->io_Error=IOERR_BADADDRESS;
        return;
    }
    c=(const UBYTE *)sc->scsi_Command;
    d=(UBYTE *)sc->scsi_Data;
    /* REQUEST SENSE must still work if IDENT has failed. */
    if(c[0]==0x03 && sc->scsi_CmdLength>=6) {
        n=c[4]<sc->scsi_Length?c[4]:sc->scsi_Length;
        actual=scsi_request_sense(b,d,n);
        goto complete;
    }
    if (refresh_geometry(b)!=0) {
        scsi_sense(b,L2_SENSE_READY,0x04);
        status=L2_SCSI_CHECK;
        goto complete;
    }
    switch(c[0]) {
    case 0x00: /* TEST UNIT READY */
        if(sc->scsi_CmdLength<6) goto invalid;
        break;
    case 0x12: /* INQUIRY */
        if(sc->scsi_CmdLength<6) goto invalid;
        n=c[4];if(n>sc->scsi_Length)n=sc->scsi_Length;
        if(n>36)n=36;
        if(n)l2_memset(d,0,n);
        if(n>2)d[2]=2;
        if(n>3)d[3]=2;
        if(n>4)d[4]=31;
        if(n>=16)l2_memcpy(d+8,"L2BOOT  ",8);
        if(n>=32)l2_memcpy(d+16,"USB-HDF         ",16);
        if(n>=36)l2_memcpy(d+32,"0113",4);
        actual=n;
        break;
    case 0x25: /* READ CAPACITY (10) */
        if(sc->scsi_CmdLength<10) goto invalid;
        if(sc->scsi_Length>=8) {
            scsi_be32_put(d,b->total_blocks-1);
            scsi_be32_put(d+4,b->block_size);
            actual=8;
        }
        break;
    case 0x1a: /* MODE SENSE (6), header + optional block descriptor */
        if(sc->scsi_CmdLength<6) goto invalid;
        n=c[4];if(n>sc->scsi_Length)n=sc->scsi_Length;
        if(n)l2_memset(d,0,n);
        if(n>=4) {
            d[0]=(UBYTE)((n>=12)?11:3);
            d[2]=(b->features & FEATURE_WRITE)?0:0x80;
            if(n>=12) {
                d[3]=8;
                d[5]=(UBYTE)(b->total_blocks>0xffffffUL?0xff:(b->total_blocks>>16));
                d[6]=(UBYTE)(b->total_blocks>0xffffffUL?0xff:(b->total_blocks>>8));
                d[7]=(UBYTE)(b->total_blocks>0xffffffUL?0xff:b->total_blocks);
                d[10]=(UBYTE)(b->block_size>>8);
                d[11]=(UBYTE)b->block_size;
            }
            actual=n>=12?12:4;
        }
        break;
    case 0x5a: /* MODE SENSE (10) */
        if(sc->scsi_CmdLength<10) goto invalid;
        n=scsi_be16_get(c+7);if(n>sc->scsi_Length)n=sc->scsi_Length;
        if(n)l2_memset(d,0,n);
        if(n>=8) {
            d[1]=6;
            d[3]=(b->features & FEATURE_WRITE)?0:0x80;
            actual=8;
        }
        break;
    case 0x08: case 0x0a: /* READ/WRITE (6) */
        if(sc->scsi_CmdLength<6) goto invalid;
        lba=((ULONG)(c[1]&0x1f)<<16)|((ULONG)c[2]<<8)|c[3];
        blocks=c[4]?c[4]:256;
        status=scsi_rw(b,d,sc->scsi_Length,lba,blocks,c[0]==0x0a,&actual);
        break;
    case 0x28: case 0x2a: /* READ/WRITE (10) */
        if(sc->scsi_CmdLength<10) goto invalid;
        lba=scsi_be32_get(c+2);blocks=scsi_be16_get(c+7);
        status=scsi_rw(b,d,sc->scsi_Length,lba,blocks,c[0]==0x2a,&actual);
        break;
    case 0x35: /* SYNCHRONIZE CACHE */
        if(sc->scsi_CmdLength<10) goto invalid;
        if (direct_flush_dirty(b)!=0) {
            scsi_sense(b,L2_SENSE_MEDIUM,0x0c);
            status=L2_SCSI_CHECK;
        }
        break;
    case 0x1b: /* START STOP UNIT: no removable medium */
    case 0x2f: /* VERIFY: media present, no per-block verifier */
        if(sc->scsi_CmdLength<6) goto invalid;
        break;
    default:
    invalid:
        scsi_sense(b,L2_SENSE_ILLEGAL,0x20);
        status=L2_SCSI_CHECK;
        break;
    }
complete:
    sc->scsi_Status=(UBYTE)status;
    sc->scsi_Actual=actual;
    if(status==L2_SCSI_CHECK && sc->scsi_SenseData && sc->scsi_SenseLength)
        sc->scsi_SenseActual=scsi_request_sense(b,
                              (UBYTE *)sc->scsi_SenseData,sc->scsi_SenseLength);
    io->io_Actual=sizeof(struct SCSICmd);
    io->io_Error=status==L2_SCSI_GOOD?0:HFERR_BadStatus;
}

static void finish_io(struct L2ScsiBase *b, struct IOStdReq *io)
{
    if (!(io->io_Flags & IOF_QUICK))
        exec_ReplyMsg(b->SysBase, &io->io_Message);
}

static void do_rw(struct L2ScsiBase *b, struct IOStdReq *io, int write)
{
    ULONG off = io->io_Offset;
    ULONG len = io->io_Length;
    UBYTE *ptr = (UBYTE *)io->io_Data;
    ULONG lba;
    ULONG blocks;
    ULONG done = 0;

    /* NSCMD_TD_READ64 passes byte offset high 32 bits in io_Actual on entry.
     * This backend currently supports only 32-bit byte offsets. Never silently
     * wrap a nonzero high word and read the wrong sector. */
    if (io->io_Command == NSCMD_TD_READ64) {
        ULONG offset_high = io->io_Actual;
        io->io_Actual = 0; /* output is a byte count, not the input offset */
        if (offset_high != 0) {
            io->io_Error = IOERR_BADADDRESS;
            return;
        }
    }

    if ((off & 511UL) || (len & 511UL) || (len && !ptr)) {
        io->io_Error = IOERR_BADLENGTH;
        return;
    }

    if (len == 0)
        return;

    /* A real trackdisk turns the motor on automatically when media access
     * is requested. Keep the same externally visible TD_MOTOR state. */
    l2_motor_note_access(b);

    if (refresh_geometry(b) != 0) {
        io->io_Error = IOERR_OPENFAIL;
        if (!write)
            driver_read_fail_latch(b,1,io->io_Error,off,len,done,
                                   off>>9,len>>9);
        return;
    }

    lba = off >> 9;
    blocks = len >> 9;

    if (lba >= b->total_blocks ||
        blocks > (b->total_blocks - lba)) {
        io->io_Error = IOERR_BADLENGTH;
        return;
    }

    if (write && !(b->features & FEATURE_WRITE)) {
        io->io_Error = TDERR_WriteProt;
        return;
    }

    while (blocks) {
        ULONG n = blocks;
        ULONG bytes;

        if (n > MAX_BLOCKS_PER_IO)
            n = MAX_BLOCKS_PER_IO;

        bytes = n << 9;

        if (direct_submit(b,
                           write ? RDISK_CMD_WRITE : RDISK_CMD_READ,
                           lba,
                           n,
                           ptr + done,
                           bytes) != 0) {
            io->io_Error = TDERR_NotSpecified;
            if (!write)
                driver_read_fail_latch(b,2,io->io_Error,off,len,done,lba,n);
            return;
        }

        if (write) b->media_dirty=1;
        done += bytes;
        io->io_Actual = done; /* preserve partial count on later failure */
        lba += n;
        blocks -= n;
    }

    io->io_Actual = done;
}

static void do_getgeometry(struct L2ScsiBase *b, struct IOStdReq *io)
{
    struct DriveGeometry *dg;

    if (!io->io_Data || io->io_Length < sizeof(struct DriveGeometry)) {
        io->io_Error = IOERR_BADLENGTH;
        return;
    }

    if (refresh_geometry(b) != 0) {
        io->io_Error = IOERR_OPENFAIL;
        return;
    }

    dg = (struct DriveGeometry *)io->io_Data;
    l2_memset(dg, 0, sizeof(*dg));

    dg->dg_SectorSize = SECTOR_SIZE;
    dg->dg_TotalSectors = b->total_blocks;
    dg->dg_Cylinders = b->total_blocks / (16UL * 63UL);
    if (dg->dg_Cylinders == 0)
        dg->dg_Cylinders = 1;
    dg->dg_CylSectors = 16UL * 63UL;
    dg->dg_Heads = 16;
    dg->dg_TrackSectors = 63;
    dg->dg_BufMemType = MEMF_PUBLIC;
    dg->dg_DeviceType = DG_DIRECT_ACCESS;
    dg->dg_Flags = 0;

    io->io_Actual = sizeof(*dg);
}

ULONG DevOpen(register struct IOStdReq *io __asm("a1"),
              register ULONG unitnum __asm("d0"),
              register ULONG flags __asm("d1"),
              register struct L2ScsiBase *b __asm("a6"))
{
    (void)flags;

    if (!io || !b || unitnum != 0)
        return IOERR_OPENFAIL;

    /* Presence of remote media is NOT a prerequisite to opening the device.
     * Required for pre-enumeration diagnostics and late WinUSB attachment. */
    b->open_count++;
    b->dev.dd_Library.lib_OpenCnt++;

    io->io_Device = (struct Device *)b;
    io->io_Unit = &b->unit;
    io->io_Error = 0;

    return 0;
}

ULONG DevClose(register struct IOStdReq *io __asm("a1"),
               register struct L2ScsiBase *b __asm("a6"))
{
    if (b) {
        if (b->open_count)
            b->open_count--;
        if (b->dev.dd_Library.lib_OpenCnt)
            b->dev.dd_Library.lib_OpenCnt--;
    }

    if (io) {
        io->io_Device = NULL;
        io->io_Unit = NULL;
    }

    return 0;
}

ULONG DevExpunge(register struct L2ScsiBase *b __asm("a6"))
{
    (void)b;
    return 0;
}

ULONG DevReserved(void)
{
    return 0;
}

/* Sole transport owner: one pending IORequest is fully completed before
 * the next can touch geometry, USB transport, or its caller's buffer.
 * Never ReplyMsg() for IOF_QUICK here: BeginIO clears QUICK before PutMsg.
 */
static void l2_worker_process(struct L2ScsiBase *b, struct IOStdReq *io)
{
    io->io_Error = 0;
    /* Preserve READ64 offset high word until do_rw() consumes it. */
    if (io->io_Command != NSCMD_TD_READ64)
        io->io_Actual = 0;
    switch (io->io_Command) {
        case RDISK_DIAG_COMMAND:
            if (!io->io_Data || io->io_Length < sizeof(struct RDiskDiag)) {
                io->io_Error=IOERR_BADLENGTH;
            } else {
                rdisk_usb_get_diag(b->rdisk,(struct RDiskDiag *)io->io_Data);
                ((struct RDiskDiag *)io->io_Data)->worker_ready=b->worker_ready;
                driver_diag_export(b,(struct RDiskDiag *)io->io_Data);
                io->io_Actual=sizeof(struct RDiskDiag);
            }
            break;
        case CMD_READ:
        case NSCMD_TD_READ64:
            do_rw(b, io, 0);
            break;

        case CMD_WRITE:
        case TD_FORMAT:
            /* Trackdisk format writes the supplied buffer to the given offset. */
            do_rw(b, io, 1);
            break;

        case CMD_FLUSH:
        case CMD_UPDATE:
            if (refresh_geometry(b) != 0)
                io->io_Error = IOERR_OPENFAIL;
            else if (direct_flush_dirty(b) != 0)
                io->io_Error = TDERR_NotSpecified;
            break;

        case CMD_CLEAR:
        case CMD_STOP:
        case CMD_START:
        case CMD_RESET:
            break;

        case TD_MOTOR:
            do_motor(b, io);
            break;

        case TD_GETGEOMETRY:
            do_getgeometry(b, io);
            break;

        case HD_SCSICMD:
            do_scsi(b,io);
            break;

        case TD_PROTSTATUS:
            if (refresh_geometry(b) != 0) {
                io->io_Error = IOERR_OPENFAIL;
            } else {
                io->io_Actual = (b->features & FEATURE_WRITE) ? 0 : (ULONG)-1;
            }
            break;

        case TD_CHANGESTATE:
            io->io_Actual = 0;
            break;

        case TD_CHANGENUM:
            io->io_Actual = 0;
            break;

        default:
            io->io_Error = IOERR_NOCMD;
            break;
    }

 }

/* AddTask enters here on an Exec task stack, not an AmigaDOS process stack. */
__attribute__((used)) void l2_worker_entry(void)
{
    struct Task *me = exec_FindTask(*((struct ExecBase **)4));
    struct L2ScsiBase *b;
    struct L2WorkerBlock *w;
    BYTE bit,usb_bit;
    ULONG mask,usb_mask;
    struct IOStdReq *io;

    if (!me) return;
    b = (struct L2ScsiBase *)me->tc_UserData;
    if (!b || !b->worker) return;
    w = b->worker;
    bit = exec_AllocSignal(b->SysBase);
    if (bit < 0) {
        b->worker_failed=1;
        b->worker_ready=0;
        /* Cannot receive a signal, so answer all queued early requests.
         * Subsequent BeginIO requests fail immediately instead of hanging. */
        while ((io=(struct IOStdReq *)exec_GetMsg(b->SysBase,&w->port))!=NULL) {
            io->io_Error=IOERR_OPENFAIL;
            io->io_Actual=0;
            exec_ReplyMsg(b->SysBase,&io->io_Message);
        }
        return;
    }

    mask = 1UL << (UBYTE)bit;
    usb_bit=exec_AllocSignal(b->SysBase);
    if(usb_bit<0) {
        b->worker_failed=1;
        b->worker_ready=0;
        /* No safe Wait signal: fail all early requests while they are still
         * queued, then exit. This is an unrecoverable Exec allocation error. */
        while ((io=(struct IOStdReq *)exec_GetMsg(b->SysBase,&w->port))!=NULL) {
            io->io_Error=IOERR_OPENFAIL;
            io->io_Actual=0;
            exec_ReplyMsg(b->SysBase,&io->io_Message);
        }
        return;
    }
    usb_mask=1UL << (UBYTE)usb_bit;
    w->port.mp_SigBit=(UBYTE)bit;
    w->port.mp_SigTask=me;
    w->port.mp_Flags=PA_SIGNAL;
    /* The message list already exists: NEVER reset it here, as BeginIO may
     * have queued the first request while mp_Flags was PA_IGNORE. */
    if(!b->rdisk || !rdisk_usb_init(b->rdisk,me,usb_mask))
        b->worker_failed = 1; /* still drain/respond to queued requests */

    for (;;) {
        /* Drain before sleeping: no lost wakeup if a PutMsg races Wait(). */
        while ((io = (struct IOStdReq *)exec_GetMsg(b->SysBase, &w->port)) != NULL) {
            l2_worker_process(b, io);
            exec_ReplyMsg(b->SysBase, &io->io_Message);
        }
        exec_Wait(b->SysBase, mask|usb_mask);
        rdisk_usb_note_worker_wake(b->rdisk);
        rdisk_usb_poll(b->rdisk);
    }
}

void DevBeginIO(register struct IOStdReq *io __asm("a1"))
{
    struct L2ScsiBase *b;

    if (!io) return;
    b = (struct L2ScsiBase *)io->io_Device;
    io->io_Error = 0;
    /* io_Actual carries READ64's upper 32 bits of the byte offset on input. */
    if (io->io_Command != NSCMD_TD_READ64)
        io->io_Actual = 0;

    /* Always answer a diagnostic request, even while worker is initializing
     * or after rdisk_usb_init has failed.  No DWC2 MMIO in this path. */
    if (b && io->io_Command == RDISK_DIAG_COMMAND) {
        if (!io->io_Data || io->io_Length < sizeof(struct RDiskDiag))
            io->io_Error=IOERR_BADLENGTH;
        else {
            rdisk_usb_get_diag(b->rdisk,(struct RDiskDiag *)io->io_Data);
            ((struct RDiskDiag *)io->io_Data)->worker_ready=b->worker_ready;
            driver_diag_export(b,(struct RDiskDiag *)io->io_Data);
            io->io_Actual=sizeof(struct RDiskDiag);
        }
        finish_io(b,io);
        return;
    }
    if (!b || !b->worker || !b->worker_ready || b->worker_failed) {
        io->io_Error = IOERR_OPENFAIL;
        if (b) finish_io(b, io);
        return;
    }

    /* Queue-only: no blocking Wait, semaphore, MMIO or USB in BeginIO.
     * A queued request MUST receive ReplyMsg, even if originally QUICK.
     */
    io->io_Flags &= (UBYTE)~IOF_QUICK;
    exec_PutMsg(b->SysBase, &b->worker->port, &io->io_Message);
}

LONG DevAbortIO(register struct IOStdReq *io __asm("a1"))
{
    if (!io)
        return IOERR_NOCMD;

    /* Abort not supported in this diagnostic: request completes in worker.
     * Never reply here; worker alone owns completion. */
    return IOERR_NOCMD;
}

struct L2ScsiBase *DevInit(register struct L2ScsiBase *b __asm("d0"),
                           register ULONG seglist __asm("a0"),
                           register struct ExecBase *sysbase __asm("a6"))
{
    (void)seglist;

    if (!b || !sysbase)
        return NULL;

    b->SysBase = sysbase;
    b->worker = NULL;
    b->rdisk_mem = NULL;
    b->rdisk = NULL;
    b->worker_ready = 0;
    b->worker_failed = 0;
    b->sense_key = b->sense_asc = b->sense_ascq = 0;
    l2_memset(&b->unit, 0, sizeof(b->unit));

    b->sequence = 0;
    b->total_blocks = 0;
    b->block_size = 0;
    b->features = 0;
    b->open_count = 0;
    b->motor_on = 0;
    b->media_dirty = 0;
    b->diag_read_fail_latched = 0;
    b->diag_read_fail_stage = 0;
    b->diag_read_fail_io_error = 0;
    b->diag_read_fail_offset = 0;
    b->diag_read_fail_length = 0;
    b->diag_read_fail_actual = 0;
    b->diag_read_fail_lba = 0;
    b->diag_read_fail_blocks = 0;

    b->dev.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    b->dev.dd_Library.lib_Node.ln_Name = (STRPTR)DEVNAME;
    b->dev.dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    b->dev.dd_Library.lib_Version = DEVVER;
    b->dev.dd_Library.lib_Revision = DEVREV;
    b->dev.dd_Library.lib_IdString = (APTR)dev_id;

    /* ROMSAFE: the original v43r108 backend kept DMA buffers, IRQ state,
     * geometry and the proto/exec SysBase variable in the LoadModule RAM
     * hunk.  In an Extended ROM those objects would be read-only. Allocate
     * the complete backend state explicitly in RAM before the worker starts.
     * The raw allocation is intentionally retained: Expunge is disabled. */
    b->rdisk_mem = exec_AllocMem(sysbase, rdisk_usb_state_bytes(),
                                 MEMF_PUBLIC | MEMF_CLEAR);
    if (!b->rdisk_mem) return NULL;
    b->rdisk = rdisk_usb_state_init(b->rdisk_mem, sysbase);
    if (!b->rdisk) return NULL;

    /* Establish a private queue/stack during AUTOINIT, not from BeginIO.
     * The worker is created LAST, after all mutable device fields are ready.
     * Keep the block allocated for this experimental device lifetime.
     * Expunge is disabled until safe worker teardown is implemented.
     */
    b->worker = (struct L2WorkerBlock *)exec_AllocMem(
        sysbase, sizeof(struct L2WorkerBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!b->worker) return NULL;
    b->worker->task.tc_Node.ln_Type = NT_TASK;
    b->worker->task.tc_Node.ln_Name = (STRPTR)"L2SCSI_USB_IO";
    b->worker->task.tc_Node.ln_Pri = 5;
    b->worker->task.tc_SPLower = (APTR)&b->worker->stack[0];
    b->worker->task.tc_SPUpper = (APTR)&b->worker->stack[L2_WORKER_STACK_LONGS];
    b->worker->task.tc_SPReg = b->worker->task.tc_SPUpper;
    b->worker->task.tc_UserData = (APTR)b;
    exec_NewList(&b->worker->task.tc_MemEntry);
    /* DevInit publishes an independent PA_IGNORE message queue before
     * AddTask. An immediate first BeginIO must NEVER fail only because the
     * worker has not yet run AllocSignal or DWC2 initialization. */
    b->worker->port.mp_Node.ln_Type=NT_MSGPORT;
    b->worker->port.mp_Node.ln_Name=(STRPTR)"L2SCSI_USB_IO_Port";
    b->worker->port.mp_Flags=PA_IGNORE;
    b->worker->port.mp_SigBit=0;
    b->worker->port.mp_SigTask=NULL;
    exec_NewList(&b->worker->port.mp_MsgList);
    b->worker_ready=1;
    exec_AddTask(sysbase, &b->worker->task, (APTR)l2_worker_entry);
    return b;
}

/* Function vector table and AutoInit table live in the single HUNK_CODE.
 * The disk-load Resident is emitted at offset 0 by the standalone assembly.
 */
const APTR DevFuncTab[] = {
    (APTR)DevOpen,
    (APTR)DevClose,
    (APTR)DevExpunge,
    (APTR)DevReserved,
    (APTR)DevBeginIO,
    (APTR)DevAbortIO,
    (APTR)-1
};

const ULONG DevInitTab[] = {
    sizeof(struct L2ScsiBase),
    (ULONG)DevFuncTab,
    0,
    (ULONG)DevInit
};
