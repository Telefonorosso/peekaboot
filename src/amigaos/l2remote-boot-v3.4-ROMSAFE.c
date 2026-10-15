/*
 * l2remote.boot 3.4 - ROMSAFE companion bootstrap for l2scsi.device v43r115.
 * Fork of the user's USB boot v3.1. Does NOT discover/patch Emu68 ROM mapping,
 * poll an Emu68 mailbox or call InitResident on l2scsi.device.
 * LoadModule owns the device resident; its priority 21 precedes this priority 9.
 * Never use a DEVS: fallback to conceal a missing LoadModule device.
 * Build/validation details in README.md.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <exec/devices.h>
#include <exec/nodes.h>
#include <exec/resident.h>
#include <devices/trackdisk.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <libraries/configvars.h>
#include "l2boot-diag.h"
#include <proto/exec.h>

#define L2_USED __attribute__((used))
#define L2BOOT_PRIORITY 9
#define L2SCSI_NAME "l2scsi.device"
#define PTABLE_NAME "ptable.library"
#define PTABLE_MINVER 1
#define EXPECT_DEV_VERSION 43
#define EXPECT_DEV_REVISION 115
#define RDISK_SECTOR_SIZE 512UL
#ifndef USB_BOOT_VISUAL_DIAG
#define USB_BOOT_VISUAL_DIAG 1
#endif
#if USB_BOOT_VISUAL_DIAG
#define L2BOOT_COLOR(n) (*(volatile UWORD *)0x00dff180UL=(UWORD)(n))
#else
#define L2BOOT_COLOR(n) ((void)(n))
#endif
/* COLOR00 milestones: purple entry; blue device; cyan geometry; white read;
 * green RDB scan; orange missing/wrong device; magenta ptable error;
 * red I/O error. No infinite failure trap: local boot must remain possible. */
#define COL_ENTRY 0x0707u
#define COL_DEVICE 0x000au
#define COL_GEOMETRY 0x00ffu
#define COL_READ 0x0fffu
#define COL_SCAN 0x00f0u
#define COL_NO_DEVICE 0x0f80u
#define COL_PTABLE 0x0f0fu
#define COL_IOERROR 0x0f00u

static char l2boot_name[] = "l2remote.boot";
static char l2boot_id_string[] =
    "$VER: l2remote.boot 3.4 (28.09.2026) ROMSAFE v43r115";
static char l2boot_ptable_name[] = PTABLE_NAME;
static char l2boot_device_name[] = L2SCSI_NAME;

/* ROMSAFE: v3.3 published a writable diagnostic structure inside its
 * LoadModule RAM hunk. In Extended ROM that storage is read-only. The ROM
 * branch keeps diagnostics on the COLDSTART stack; COLOR00 remains the
 * persistent-enough early-boot indication. */
#define DIAG_STAGE(n) do { diag->stage = (n); } while(0)
#define DIAG_EXIT(n) do { diag->exit_reason = (n); return 0; } while(0)

/* Direct Exec calls keep this resident independent of startup code/libc. */
static struct Resident *l2boot_FindResident(struct ExecBase *sys,
                                             const char *name)
{
    register struct Resident *d0 __asm("d0");
    register const char *a1 __asm("a1") = name;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-96:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}

/* FindName() in Exec DeviceList, using the same LVO as original L2Boot. */
static struct Node *l2boot_FindName(struct ExecBase *sys,
                                    struct List *list, const char *name)
{
    register struct Node *d0 __asm("d0");
    register struct List *a0 __asm("a0") = list;
    register const char *a1 __asm("a1") = name;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-276:W)"
        : "=r"(d0) : "r"(a6), "r"(a0), "r"(a1)
        : "d1", "cc", "memory");
    return d0;
}

static APTR l2boot_InitResident(struct ExecBase *sys,
                                struct Resident *resident)
{
    register APTR d0 __asm("d0");
    register APTR a0 __asm("a0") = (APTR)0;
    register struct Resident *a1 __asm("a1") = resident;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-102:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a0), "r"(a1)
        : "d1", "cc", "memory");
    return d0;
}

static struct Library *l2boot_OpenLibrary(struct ExecBase *sys,
                                           const char *name,
                                           ULONG version)
{
    register struct Library *d0 __asm("d0");
    register ULONG rd0 __asm("d0") = version;
    register const char *a1 __asm("a1") = name;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-552:W)"
        : "=r"(d0)
        : "r"(a6), "r"(rd0), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}

static void l2boot_CloseLibrary(struct ExecBase *sys, struct Library *lib)
{
    register struct Library *a1 __asm("a1") = lib;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-414:W)"
        :
        : "r"(a6), "r"(a1)
        : "d0", "d1", "a0", "cc", "memory");
}

/* Exec OpenDevice LVO -444: A0 name, D0 unit, A1 IORequest, D1 flags. */
static LONG l2boot_OpenDevice(struct ExecBase *sys, const char *name,
                             ULONG unit, struct IOStdReq *io)
{
    register LONG d0 __asm("d0") = (LONG)unit;
    register const char *a0 __asm("a0") = name;
    register struct IOStdReq *a1 __asm("a1") = io;
    register ULONG d1 __asm("d1") = 0;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-444:W)" : "+r"(d0)
                      : "r"(a0), "r"(a1), "r"(d1), "r"(a6)
                      : "cc", "memory");
    return d0;
}

/* Direct Exec I/O helpers: no libc and no DOS dependencies. */
static LONG l2boot_DoIO(struct ExecBase *sys, struct IOStdReq *io)
{
    register LONG d0 __asm("d0");
    register struct IOStdReq *a1 __asm("a1") = io;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-456:W)" : "=r"(d0)
                      : "r"(a1), "r"(a6)
                      : "d1", "a0", "cc", "memory");
    return d0;
}
static LONG l2boot_AllocSignal(struct ExecBase *sys)
{
    register LONG d0 __asm("d0") = -1;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-330:W)" : "+r"(d0)
                      : "r"(a6) : "d1", "a0", "a1", "cc", "memory");
    return d0;
}
static void l2boot_FreeSignal(struct ExecBase *sys, LONG sig)
{
    register LONG d0 __asm("d0") = sig;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-336:W)" : : "r"(d0), "r"(a6)
                      : "d1", "a0", "a1", "cc", "memory");
}
static struct Task *l2boot_FindTask(struct ExecBase *sys)
{
    register struct Task *d0 __asm("d0");
    register APTR a1 __asm("a1") = (APTR)0;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-294:W)" : "=r"(d0)
                      : "r"(a1), "r"(a6) : "d1", "a0", "cc", "memory");
    return d0;
}
static void l2boot_zero(void *dst, ULONG len)
{
    volatile UBYTE *p = (volatile UBYTE *)dst;
    while (len--) *p++ = 0;
}
static void l2boot_CloseDevice(struct ExecBase *sys, struct IOStdReq *io)
{
    register struct IOStdReq *a1 __asm("a1") = io;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-450:W)" : : "r"(a1), "r"(a6)
                      : "d0", "d1", "a0", "cc", "memory");
}

/* Original L2Boot / ptable.library call ABI: A1=device name, D0=unit,
 * A6=ptable.library; first public vector at LVO -30. */
static LONG l2boot_BootScanRDB(struct Library *ptable,
                               const char *device_name, ULONG unit)
{
    register LONG d0 __asm("d0") = (LONG)unit;
    register const char *a1 __asm("a1") = device_name;
    register struct Library *a6 __asm("a6") = ptable;
    __asm__ volatile ("jsr a6@(-30:W)"
        : "+r"(d0) : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}

/* Diagnostic only: the expansion.library MountList is the pre-DOS
 * BootNode staging list. Never change it or dereference any DeviceNode.
 * UINT32_MAX means expansion.library was not available; 256 is the
 * conservative traversal cap to avoid looping forever on a corrupt list. */
static ULONG l2boot_count_bootnodes(struct ExpansionBase *exp)
{
    struct Node *node;
    ULONG n = 0;
    if (!exp) return 0xffffffffUL;
    node = exp->MountList.lh_Head;
    while (node && node->ln_Succ && n < 256UL) {
        ++n;
        node = node->ln_Succ;
    }
    return n;
}

L2_USED static LONG l2boot_entry(register struct ExecBase *sysbase __asm("a6"))
{
    struct Node *devnode;
    struct Device *dev;
    struct Library *ptable;
    struct Resident *ptable_resident;
    struct IOStdReq io;
    struct MsgPort reply;
    struct DriveGeometry geometry;
    UBYTE block0[512];
    LONG signal = -1, result;
    LONG open_err;
    LONG scan_result;
    struct ExpansionBase *exp = 0;
    struct L2BootDiag diag_storage;
    struct L2BootDiag *diag = &diag_storage;
    int opened = 0;

    l2boot_zero(diag, sizeof(*diag));
    diag->magic = L2BOOT_DIAG_MAGIC;
    diag->record_version = L2BOOT_DIAG_VERSION;
    diag->mount_before = 0xffffffffUL;
    diag->mount_after = 0xffffffffUL;
    diag->scan_result = 0x7fffffffUL; /* not called */
    if (!sysbase) sysbase = *(struct ExecBase **)4;
    if (!sysbase) DIAG_EXIT(L2EXIT_NO_SYSBASE);
    DIAG_STAGE(L2ST_ENTRY);
    L2BOOT_COLOR(COL_ENTRY);

    /* A visible DeviceList entry is required. Do NOT fall back to DEVS: via
     * OpenDevice and do NOT call InitResident on a device Exec may still init.
     * Its higher priority makes absence a bootstrap ordering/config failure. */
    devnode = l2boot_FindName(sysbase, &sysbase->DeviceList,
                              l2boot_device_name);
    if (!devnode) {
        L2BOOT_COLOR(COL_NO_DEVICE);
        DIAG_EXIT(L2EXIT_NO_DEVICE);
    }
    dev = (struct Device *)devnode;
    diag->device_version = dev->dd_Library.lib_Version;
    diag->device_revision = dev->dd_Library.lib_Revision;
    if (dev->dd_Library.lib_Version != EXPECT_DEV_VERSION ||
        dev->dd_Library.lib_Revision != EXPECT_DEV_REVISION) {
        L2BOOT_COLOR(COL_NO_DEVICE);
        DIAG_EXIT(L2EXIT_WRONG_DEVICE);
    }
    DIAG_STAGE(L2ST_DEVICE);
    L2BOOT_COLOR(COL_DEVICE);

    /* Proven v3.1 pattern: open ptable, optionally init its Resident, and
     * use the same public BootScanRDB LVO (-30) after read-only probes. */
    ptable = l2boot_OpenLibrary(sysbase,l2boot_ptable_name,PTABLE_MINVER);
    if (!ptable) {
        ptable_resident = l2boot_FindResident(sysbase,l2boot_ptable_name);
        if (ptable_resident) {
            (void)l2boot_InitResident(sysbase,ptable_resident);
            ptable = l2boot_OpenLibrary(sysbase,l2boot_ptable_name,PTABLE_MINVER);
        }
    }
    if (!ptable) {
        L2BOOT_COLOR(COL_PTABLE);
        DIAG_EXIT(L2EXIT_NO_PTABLE);
    }

    diag->ptable_base = (ULONG)ptable;
    diag->ptable_version = ptable->lib_Version;
    diag->ptable_revision = ptable->lib_Revision;
    diag->ptable_lvo30_opcode = *(volatile const UWORD *)((const UBYTE *)ptable - 30);
    DIAG_STAGE(L2ST_PTABLE);

    l2boot_zero(&io,sizeof(io));
    io.io_Message.mn_Length=sizeof(io);
    open_err=l2boot_OpenDevice(sysbase,l2boot_device_name,0,&io);
    diag->open_rc = open_err;
    if (open_err || !io.io_Device) {
        L2BOOT_COLOR(COL_IOERROR);
        diag->exit_reason = L2EXIT_OPEN_FAIL;
        goto close_ptable;
    }
    opened=1;
    DIAG_STAGE(L2ST_OPEN);

    /* v3.1's valid Exec reply port: do not submit worker I/O with a NULL port.
     * The entire port, IORequest and data survive until each DoIO finishes. */
    signal=l2boot_AllocSignal(sysbase);
    if (signal < 0) {
        L2BOOT_COLOR(COL_IOERROR);
        diag->exit_reason = L2EXIT_NO_SIGNAL;
        goto close_device;
    }
    l2boot_zero(&reply,sizeof(reply));
    reply.mp_Node.ln_Type=NT_MSGPORT;
    reply.mp_Flags=PA_SIGNAL;
    reply.mp_SigBit=(UBYTE)signal;
    reply.mp_SigTask=l2boot_FindTask(sysbase);
    if (!reply.mp_SigTask) {
        L2BOOT_COLOR(COL_IOERROR);
        diag->exit_reason = L2EXIT_NO_TASK;
        goto close_device;
    }
    reply.mp_MsgList.lh_Head=(struct Node *)&reply.mp_MsgList.lh_Tail;
    reply.mp_MsgList.lh_Tail=0;
    reply.mp_MsgList.lh_TailPred=(struct Node *)&reply.mp_MsgList.lh_Head;
    io.io_Message.mn_ReplyPort=&reply;

    /* The v43r115 ROMSAFE worker owns enumeration and IDENT. No Emu68 busy poll. */
    l2boot_zero(&geometry,sizeof(geometry));
    io.io_Command=TD_GETGEOMETRY;
    io.io_Data=&geometry;
    io.io_Length=sizeof(geometry);
    io.io_Offset=0;
    io.io_Actual=0;
    io.io_Error=0;
    io.io_Flags=0;
    DIAG_STAGE(L2ST_GEOMETRY_PENDING);
    result=l2boot_DoIO(sysbase,&io);
    diag->geometry_rc = result;
    diag->geometry_error = (LONG)io.io_Error;
    diag->geometry_actual = io.io_Actual;
    diag->geometry_sector_size = geometry.dg_SectorSize;
    diag->geometry_sectors = geometry.dg_TotalSectors;
    if (result || io.io_Error ||
        geometry.dg_SectorSize != RDISK_SECTOR_SIZE ||
        geometry.dg_TotalSectors == 0) {
        L2BOOT_COLOR(COL_IOERROR);
        diag->exit_reason = L2EXIT_GEOMETRY_FAIL;
        goto close_device;
    }
    DIAG_STAGE(L2ST_GEOMETRY_OK);
    L2BOOT_COLOR(COL_GEOMETRY);

    l2boot_zero(block0,sizeof(block0));
    io.io_Command=CMD_READ;
    io.io_Data=block0;
    io.io_Length=sizeof(block0);
    io.io_Offset=0;
    io.io_Actual=0;
    io.io_Error=0;
    io.io_Flags=0;
    DIAG_STAGE(L2ST_READ_PENDING);
    result=l2boot_DoIO(sysbase,&io);
    diag->read_rc = result;
    diag->read_error = (LONG)io.io_Error;
    diag->read_actual = io.io_Actual;
    diag->block0_magic = ((ULONG)block0[0]<<24) | ((ULONG)block0[1]<<16) |
                         ((ULONG)block0[2]<<8) | (ULONG)block0[3];
    if (result || io.io_Error || io.io_Actual != sizeof(block0)) {
        L2BOOT_COLOR(COL_IOERROR);
        diag->exit_reason = L2EXIT_READ_FAIL;
        goto close_device;
    }
    DIAG_STAGE(L2ST_READ_OK);
    L2BOOT_COLOR(COL_READ);

    /* ptable.library must open/close its own handle. Drop our own handle and
     * stack-backed message port first, as in the validated v3.1 bootstrap. */
    l2boot_CloseDevice(sysbase,&io);
    opened=0;
    l2boot_FreeSignal(sysbase,signal);
    signal=-1;

    /* Capture the expansion pre-DOS boot-node list before/after scanning.
     * Count ONLY; no changes to ptable or its reference count in v3.4. */
    exp=(struct ExpansionBase *)l2boot_OpenLibrary(sysbase,"expansion.library",0);
    diag->mount_before=l2boot_count_bootnodes(exp);
    DIAG_STAGE(L2ST_SCAN_PENDING);
    L2BOOT_COLOR(COL_SCAN);
    diag->scan_called = 1;
    scan_result=l2boot_BootScanRDB(ptable,l2boot_device_name,0);
    diag->scan_result = scan_result;
    diag->scan_returned = 1;
    DIAG_STAGE(L2ST_SCAN_RETURNED);
    diag->mount_after=l2boot_count_bootnodes(exp);
    if (exp) l2boot_CloseLibrary(sysbase,(struct Library *)exp);
    l2boot_CloseLibrary(sysbase,ptable);
    diag->exit_reason = L2EXIT_COMPLETE;
    DIAG_STAGE(L2ST_COMPLETE);
    return 0;

close_device:
    if (opened) l2boot_CloseDevice(sysbase,&io);
    if (signal >= 0) l2boot_FreeSignal(sysbase,signal);
close_ptable:
    l2boot_CloseLibrary(sysbase,ptable);
    return 0; /* permit local boot on failed remote disk */
}

extern struct Resident L2BootROMTag;
extern UBYTE ROMModuleEnd;
L2_USED __attribute__((section(".text.romtag"))) struct Resident L2BootROMTag = {
    RTC_MATCHWORD,
    &L2BootROMTag,
    (APTR)&ROMModuleEnd,
    RTF_COLDSTART,
    3,
    NT_UNKNOWN,
    L2BOOT_PRIORITY,
    l2boot_name,
    l2boot_id_string,
    (APTR)l2boot_entry
};
