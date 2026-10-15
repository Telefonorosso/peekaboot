#ifndef L2BOOT_DIAG_H
#define L2BOOT_DIAG_H

#include <exec/types.h>

/* The string remains a normal NUL-terminated Resident rt_IdString.
 * The diagnostic record follows at offset 96 (longword-aligned on m68k).
 * This interface is diagnostic-only, not part of the disk wire protocol. */
#define L2BOOT_DIAG_OFFSET 96UL
#define L2BOOT_DIAG_MAGIC 0x4c324433UL /* L2D3 */
#define L2BOOT_DIAG_VERSION 1UL

#define L2ST_ENTRY             1UL
#define L2ST_DEVICE            2UL
#define L2ST_PTABLE            3UL
#define L2ST_OPEN              4UL
#define L2ST_GEOMETRY_PENDING  5UL
#define L2ST_GEOMETRY_OK       6UL
#define L2ST_READ_PENDING      7UL
#define L2ST_READ_OK           8UL
#define L2ST_SCAN_PENDING      9UL
#define L2ST_SCAN_RETURNED    10UL
#define L2ST_COMPLETE         11UL

#define L2EXIT_NONE            0UL
#define L2EXIT_COMPLETE        1UL
#define L2EXIT_NO_SYSBASE      2UL
#define L2EXIT_NO_DEVICE       3UL
#define L2EXIT_WRONG_DEVICE    4UL
#define L2EXIT_NO_PTABLE       5UL
#define L2EXIT_OPEN_FAIL       6UL
#define L2EXIT_NO_SIGNAL       7UL
#define L2EXIT_NO_TASK         8UL
#define L2EXIT_GEOMETRY_FAIL   9UL
#define L2EXIT_READ_FAIL      10UL

struct L2BootDiag {
    ULONG magic;
    ULONG record_version;
    ULONG stage;
    ULONG exit_reason;
    ULONG device_version;
    ULONG device_revision;
    ULONG ptable_base;
    ULONG ptable_version;
    ULONG ptable_revision;
    ULONG ptable_lvo30_opcode;
    LONG open_rc;
    LONG geometry_rc;
    LONG geometry_error;
    ULONG geometry_actual;
    ULONG geometry_sector_size;
    ULONG geometry_sectors;
    LONG read_rc;
    LONG read_error;
    ULONG read_actual;
    ULONG block0_magic;
    ULONG mount_before;
    ULONG mount_after;
    ULONG scan_called;
    ULONG scan_returned;
    LONG scan_result;
};

struct L2BootPublished {
    char id_string[L2BOOT_DIAG_OFFSET];
    struct L2BootDiag diag;
};

#endif
