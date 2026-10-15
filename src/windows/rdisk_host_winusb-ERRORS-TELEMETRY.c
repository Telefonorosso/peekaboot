/* ERROR-ONLY + 500-ms telemetry candidate; baseline: PADDING-MATERIALIZE-FIX. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <usbiodef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "user32.lib")

#define VID_TEXT "vid_0525"
#define PID_TEXT "pid_a4ad"
#define EP_OUT 0x01
#define EP_IN  0x82
#define SECTOR 512u
/* Prevent a silent warm reboot from leaving a synchronous WinUSB read
 * pending indefinitely.  An idle header timeout is NOT a disconnect. */
#define COMPANION_READ_TIMEOUT_MS 2000u
#define COMPANION_WRITE_TIMEOUT_MS 5000u
#define RDB_SCAN_LIMIT 16u
#define MAX_FLAT_PADDING_BLOCKS 4096u
#define MAX_RDB_PARTITIONS 128u
#define PART_BOOTABLE_FLAG 1u
#define PART_FLAGS_WORD 5u
#define PART_BOOTPRI_WORD 47u
#define REQ_MAGIC  0x32534452u
#define RESP_MAGIC 0x32524452u
#define VERSION 1u
#define CMD_IDENT 1u
#define CMD_READ  2u
#define CMD_WRITE 3u
#define CMD_FLUSH 4u
#define STATS_INTERVAL_MS 500u

#pragma pack(push,1)
struct request_hdr {
    uint32_t magic, version, command, sequence;
    uint32_t lba, blocks, length, flags;
};
struct response_hdr {
    uint32_t magic, version, command, sequence;
    int32_t result;
    uint32_t length, total_blocks, block_size;
};
#pragma pack(pop)

/* Telemetry is produced solely by the USB worker: no lock, timer thread or
 * GUI call in the I/O path. --stats is opt-in; the Win32 front-end consumes
 * these lines without displaying them in the scrollback. */
struct io_stats {
    unsigned long long read_bytes, write_bytes, errors;
    ULONGLONG last_emit_ms;
    int enabled;
};

static void stats_emit(struct io_stats *s, ULONGLONG now)
{
    if (!s->enabled) return;
    printf("@L2STAT ms=%llu read=%llu write=%llu errors=%llu\n",
           (unsigned long long)now,s->read_bytes,s->write_bytes,s->errors);
    fflush(stdout);
    s->last_emit_ms=now;
}

static void stats_emit_due(struct io_stats *s)
{
    ULONGLONG now;
    if (!s->enabled) return;
    now=GetTickCount64();
    if (now-s->last_emit_ms>=STATS_INTERVAL_MS) stats_emit(s,now);
}

static void log_request_error(struct io_stats *s, const char *op,
                              const struct request_hdr *q, const char *why,
                              DWORD winerr)
{
    s->errors++;
    fprintf(stderr,"ERROR %s lba=%u blocks=%u seq=%08x: %s",
            op,q->lba,q->blocks,q->sequence,why);
    if (winerr!=ERROR_SUCCESS)
        fprintf(stderr," (Win32 error %lu)",(unsigned long)winerr);
    fputc('\n',stderr);
}

static int expected_usb_disconnect(DWORD error)
{
    return error==ERROR_DEVICE_NOT_CONNECTED ||
           error==ERROR_NO_SUCH_DEVICE ||
           error==ERROR_OPERATION_ABORTED ||
           error==ERROR_INVALID_HANDLE ||
           error==ERROR_HANDLE_EOF ||
           error==ERROR_GEN_FAILURE;
}

static int path_matches(const char *p)
{
    char lower[2048];
    size_t i, n = strlen(p);
    if (n >= sizeof(lower)) n = sizeof(lower)-1;
    for (i=0;i<n;i++) {
        char c=p[i];
        lower[i]=(c>='A'&&c<='Z')?(char)(c+('a'-'A')):c;
    }
    lower[n]=0;
    return strstr(lower, VID_TEXT) && strstr(lower, PID_TEXT);
}

static HANDLE find_device(void)
{
    HDEVINFO h = SetupDiGetClassDevs(&GUID_DEVINTERFACE_USB_DEVICE, NULL, NULL,
                                     DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    DWORD idx;
    if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    for (idx=0;;idx++) {
        SP_DEVICE_INTERFACE_DATA ifd;
        DWORD needed=0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail;
        HANDLE dev;
        memset(&ifd,0,sizeof(ifd));
        ifd.cbSize=sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(h,NULL,&GUID_DEVINTERFACE_USB_DEVICE,idx,&ifd))
            break;
        SetupDiGetDeviceInterfaceDetailA(h,&ifd,NULL,0,&needed,NULL);
        if (!needed) continue;
        detail=(PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(needed);
        if (!detail) continue;
        memset(detail,0,needed);
        detail->cbSize=sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailA(h,&ifd,detail,needed,NULL,NULL)) {
            free(detail); continue;
        }
        if (!path_matches(detail->DevicePath)) { free(detail); continue; }
        dev=CreateFileA(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,
                        FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED,NULL);
        free(detail);
        SetupDiDestroyDeviceInfoList(h);
        return dev;
    }
    SetupDiDestroyDeviceInfoList(h);
    return INVALID_HANDLE_VALUE;
}

static int read_exact(WINUSB_INTERFACE_HANDLE u, UCHAR ep, void *buf, ULONG len)
{
    ULONG done=0;
    while (done<len) {
        ULONG got=0;
        if (!WinUsb_ReadPipe(u,ep,(PUCHAR)buf+done,len-done,&got,NULL)) return 0;
        if (!got) { SetLastError(ERROR_HANDLE_EOF); return 0; }
        done+=got;
    }
    return 1;
}

/* 1 = complete header, 2 = idle (no bytes before pipe timeout),
 * 0 = broken connection / incomplete header / other transfer failure.
 * Never discard a partial header and then treat the rest as a new header. */
static int read_request_header(WINUSB_INTERFACE_HANDLE u, struct request_hdr *q,
                               ULONG *partial_bytes)
{
    ULONG done=0;
    *partial_bytes=0;
    while (done<sizeof(*q)) {
        ULONG got=0;
        if (!WinUsb_ReadPipe(u,EP_IN,(PUCHAR)q+done,
                             (ULONG)sizeof(*q)-done,&got,NULL)) {
            DWORD e=GetLastError();
            if ((e==ERROR_SEM_TIMEOUT || e==ERROR_TIMEOUT) && done==0)
                return 2;
            *partial_bytes=done;
            SetLastError(e);
            return 0;
        }
        if (!got) {
            *partial_bytes=done;
            SetLastError(ERROR_HANDLE_EOF);
            return 0;
        }
        done+=got;
    }
    return 1;
}

static int configure_session_pipes(WINUSB_INTERFACE_HANDLE u)
{
    ULONG t=COMPANION_READ_TIMEOUT_MS;
    if (!WinUsb_SetPipePolicy(u,EP_IN,PIPE_TRANSFER_TIMEOUT,sizeof(t),&t))
        return 0;
    t=COMPANION_WRITE_TIMEOUT_MS;
    if (!WinUsb_SetPipePolicy(u,EP_OUT,PIPE_TRANSFER_TIMEOUT,sizeof(t),&t))
        return 0;
    return 1;
}

static int write_exact(WINUSB_INTERFACE_HANDLE u, UCHAR ep, const void *buf, ULONG len)
{
    ULONG done=0;
    while (done<len) {
        ULONG sent=0;
        if (!WinUsb_WritePipe(u,ep,(PUCHAR)buf+done,len-done,&sent,NULL)) return 0;
        if (!sent) { SetLastError(ERROR_WRITE_FAULT); return 0; }
        done+=sent;
    }
    return 1;
}

static int image_seek(HANDLE f, uint64_t off)
{
    LARGE_INTEGER li; li.QuadPart=(LONGLONG)off;
    return SetFilePointerEx(f,li,NULL,FILE_BEGIN)!=0;
}

static int send_response(WINUSB_INTERFACE_HANDLE u, uint32_t cmd, uint32_t seq,
                         int32_t result, const void *payload, uint32_t length,
                         uint32_t total_blocks)
{
    struct response_hdr r;
    r.magic=RESP_MAGIC; r.version=VERSION; r.command=cmd; r.sequence=seq;
    r.result=result; r.length=length; r.total_blocks=total_blocks; r.block_size=SECTOR;
    if (!write_exact(u,EP_OUT,&r,sizeof(r))) return 0;
    if (length && !write_exact(u,EP_OUT,payload,length)) return 0;
    return 1;
}

static int send_response_checked(struct io_stats *s, WINUSB_INTERFACE_HANDLE u,
                                 const struct request_hdr *q, int32_t result,
                                 const void *payload, uint32_t length,
                                 uint32_t total_blocks)
{
    DWORD e;
    if (send_response(u,q->command,q->sequence,result,payload,length,
                      total_blocks)) return 1;
    e=GetLastError();
    s->errors++;
    fprintf(stderr,"ERROR USB reply cmd=%u seq=%08x result=%ld: Win32 error %lu\n",
            q->command,q->sequence,(long)result,(unsigned long)e);
    SetLastError(e);
    return 0;
}

/* -------------------------------------------------------------------------
 * Offline image creation. This code does not open WinUSB or change the
 * hardware-validated request/response path below.
 *
 * create       : completely blank logical disk, without an RDB.
 * create-rdb   : blank logical disk plus RDSK at LBA 0, PART at LBA 1.
 * The partition is intentionally UNFORMATTED; the Amiga creates FFS with
 * Format. All on-disk RDB ULONG fields are big-endian (unlike USB headers).
 * ------------------------------------------------------------------------- */
#define RDB_UNUSED 0xffffffffu
#define RDB_HEADS 16u
#define RDB_SECTORS 32u
#define RDB_CYLBLKS (RDB_HEADS * RDB_SECTORS)

static void disk_be32(uint8_t *p, uint32_t n)
{
    p[0]=(uint8_t)(n>>24); p[1]=(uint8_t)(n>>16);
    p[2]=(uint8_t)(n>>8);  p[3]=(uint8_t)n;
}

static uint32_t disk_get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|(uint32_t)p[3];
}

static void disk_word(uint8_t *sector, uint32_t word, uint32_t value)
{
    disk_be32(sector+word*4u,value);
}

static void disk_checksum(uint8_t *sector, uint32_t words)
{
    uint32_t sum=0, i;
    disk_word(sector,2,0);
    for (i=0;i<words;i++) sum+=disk_get_be32(sector+i*4u);
    disk_word(sector,2,0u-sum);
}

static int image_write_sector(HANDLE file, uint32_t lba, const uint8_t *sector)
{
    LARGE_INTEGER pos;
    DWORD wrote=0;
    pos.QuadPart=(LONGLONG)lba*SECTOR;
    return SetFilePointerEx(file,pos,NULL,FILE_BEGIN) &&
           WriteFile(file,sector,SECTOR,&wrote,NULL) && wrote==SECTOR;
}

static int image_create(const char *path, const char *size_arg, int with_rdb)
{
    char *end=NULL;
    unsigned long mib;
    uint64_t bytes;
    HANDLE file;
    LARGE_INTEGER size;
    uint8_t sector[SECTOR];
    uint32_t cylinders;
    DWORD err=0;

    if (!size_arg || !*size_arg || *size_arg=='-' || *size_arg=='+') {
        fprintf(stderr,"invalid size: specify MiB as a positive integer\n");
        return 2;
    }
    mib=strtoul(size_arg,&end,10);
    if (!end || *end || mib<2 || mib>2048) {
        fprintf(stderr,"size must be an integer from 2 to 2048 MiB\n");
        return 2;
    }
    bytes=(uint64_t)mib*1024u*1024u;
    cylinders=(uint32_t)(bytes/(RDB_CYLBLKS*SECTOR));
    if (with_rdb && cylinders<2) {
        fprintf(stderr,"disk too small for RDB and partition\n");
        return 2;
    }

    /* CREATE_NEW never overwrites a valuable existing test image. */
    file=CreateFileA(path,GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_NEW,
                     FILE_ATTRIBUTE_NORMAL,NULL);
    if (file==INVALID_HANDLE_VALUE) {
        fprintf(stderr,"cannot create '%s' (Win32 error %lu); existing files are never overwritten\n",
                path,GetLastError());
        return 2;
    }
    size.QuadPart=(LONGLONG)bytes;
    if (!SetFilePointerEx(file,size,NULL,FILE_BEGIN) || !SetEndOfFile(file))
        goto fail;

    if (with_rdb) {
        /* RigidDiskBlock (AmigaOS hardblocks.h word layout). */
        memset(sector,0,sizeof(sector));
        disk_word(sector,0,0x5244534bu);      /* RDSK */
        disk_word(sector,1,64);               /* summed ULONGs */
        disk_word(sector,3,7);                /* host ID */
        disk_word(sector,4,SECTOR);           /* bytes/block */
        disk_word(sector,6,RDB_UNUSED);       /* bad block list */
        disk_word(sector,7,1);                /* PART at LBA 1 */
        disk_word(sector,8,RDB_UNUSED);       /* no embedded FS header */
        disk_word(sector,9,RDB_UNUSED);       /* no drive init */
        disk_word(sector,16,cylinders);
        disk_word(sector,17,RDB_SECTORS);
        disk_word(sector,18,RDB_HEADS);
        disk_word(sector,19,1);              /* interleave */
        disk_word(sector,32,0);              /* RDB reserved region */
        disk_word(sector,33,RDB_CYLBLKS-1);
        disk_word(sector,34,1);              /* first data cylinder */
        disk_word(sector,35,cylinders-1);
        disk_word(sector,36,RDB_CYLBLKS);
        disk_word(sector,38,1);              /* highest RDB block used */
        memcpy(sector+160,"L2BOOT  ",8);
        memcpy(sector+168,"USB REMOTE DISK ",16);
        memcpy(sector+184,"0001",4);
        disk_checksum(sector,64);
        if (!image_write_sector(file,0,sector)) goto fail;

        /* PartitionBlock: name BSTR, DosEnvec starts at word 32. */
        memset(sector,0,sizeof(sector));
        disk_word(sector,0,0x50415254u);      /* PART */
        disk_word(sector,1,64);
        disk_word(sector,3,7);               /* host ID */
        disk_word(sector,4,RDB_UNUSED);      /* end of PART chain */
        sector[36]=4;                       /* BSTR: UDH0 */
        memcpy(sector+37,"UDH0",4);
        disk_word(sector,32+0,16);          /* de_TableSize */
        disk_word(sector,32+1,128);         /* de_SizeBlock (ULONGs) */
        disk_word(sector,32+2,0);           /* de_SecOrg */
        disk_word(sector,32+3,RDB_HEADS);   /* de_Surfaces */
        disk_word(sector,32+4,1);           /* de_SectorPerBlock */
        disk_word(sector,32+5,RDB_SECTORS); /* de_BlocksPerTrack */
        disk_word(sector,32+6,2);           /* de_Reserved */
        disk_word(sector,32+7,0);           /* de_PreAlloc */
        disk_word(sector,32+8,0);           /* de_Interleave */
        disk_word(sector,32+9,1);           /* de_LowCyl */
        disk_word(sector,32+10,cylinders-1);/* de_HighCyl */
        disk_word(sector,32+11,30);         /* de_NumBuffers */
        disk_word(sector,32+12,1);          /* de_BufMemType MEMF_PUBLIC */
        disk_word(sector,32+13,0x7fffffffu);/* de_MaxTransfer */
        disk_word(sector,32+14,0xfffffffeu);/* de_Mask */
        disk_word(sector,32+15,0);          /* de_BootPri */
        disk_word(sector,32+16,0x444f5303u);/* DOS\\3: FFS */
        disk_checksum(sector,64);
        if (!image_write_sector(file,1,sector)) goto fail;
    }

    if (!FlushFileBuffers(file)) goto fail;
    CloseHandle(file);
    printf("created: %s (%lu MiB), %s\n",path,mib,
           with_rdb ? "RDB + unformatted UDH0 DOS/3" : "blank, no RDB");
    if (with_rdb)
        printf("geometry: %lu cylinders x %u heads x %u sectors; UDH0 cylinders 1..%lu\n",
               (unsigned long)cylinders,RDB_HEADS,RDB_SECTORS,
               (unsigned long)(cylinders-1));
    return 0;
fail:
    err=GetLastError();
    CloseHandle(file);
    DeleteFileA(path);
    fprintf(stderr,"image creation failed (%lu); incomplete file removed\n",err);
    return 2;
}


/* -------------------------------------------------------------------------
 * Image adapter: physical HDF bytes -> logical disk sectors.
 * Native RDB images are served byte-for-byte. Flat DOS/0..7 files receive
 * an in-memory RDB/PART wrapper; virtual headers never modify the source.
 * Filesystem sectors may be read-write when the host image handle allows it.
 * Unrecognized/blank images are exposed RAW and writable when possible;
 * invalid RDBs and ambiguous DOS images remain read-only.
 * The USB endpoint loop remains the v43.103 milestone implementation.
 * ------------------------------------------------------------------------- */
struct disk_view {
    HANDLE image;
    uint32_t source_blocks;
    uint32_t total_blocks;
    uint32_t partition_blocks;
    uint32_t partition_start;
    uint32_t heads, sectors, padding;
    uint8_t dostype[4];
    uint8_t rdb[SECTOR], part[SECTOR];
    int virtual_rdb, readonly, native_rdb, verified;
    int raw_write_eligible; /* unrecognized/blank RAW, not ambiguous DOS or damaged RDB */
    int bootpri_override, bootpri;
    uint32_t native_part_lbas[MAX_RDB_PARTITIONS], native_part_count;
    uint32_t bootable_part_count;
    char reason[160];
};

static int read_at(HANDLE f, uint64_t offset, void *data, DWORD count)
{
    DWORD done=0;
    if (!image_seek(f,offset) || !ReadFile(f,data,count,&done,NULL)) return 0;
    if (done!=count) { SetLastError(ERROR_HANDLE_EOF); return 0; }
    return 1;
}

static int write_at(HANDLE f, uint64_t offset, const void *data, DWORD count)
{
    DWORD done=0;
    if (!image_seek(f,offset) || !WriteFile(f,data,count,&done,NULL)) return 0;
    if (done!=count) { SetLastError(ERROR_WRITE_FAULT); return 0; }
    return 1;
}

static int rdb_checksum_valid(const uint8_t *sector)
{
    uint32_t n=disk_get_be32(sector+4), i, sum=0;
    if (n<16 || n>SECTOR/4) return 0;
    for (i=0;i<n;i++) sum+=disk_get_be32(sector+i*4u);
    return sum==0;
}

/* PART field overrides are applied only to the response image, never to
 * the source HDF. PART and checksum ULONGs are big-endian. */
static void override_part_bootpri(uint8_t *sector, int bootpri)
{
    uint32_t words=disk_get_be32(sector+4u);
    if (memcmp(sector,"PART",4)!=0 || words<PART_BOOTPRI_WORD+1u ||
        words>SECTOR/4u || !rdb_checksum_valid(sector) ||
        !(disk_get_be32(sector+PART_FLAGS_WORD*4u)&PART_BOOTABLE_FLAG))
        return;
    disk_word(sector,PART_BOOTPRI_WORD,(uint32_t)(int32_t)bootpri);
    disk_checksum(sector,words);
}

/* Discover the actual native PART chain, not arbitrary sectors containing
 * the bytes PART. Incomplete/cyclic chains fail closed for --bootpri. */
static int discover_native_parts(struct disk_view *v, uint32_t first_part)
{
    uint8_t sector[SECTOR];
    uint32_t next=first_part, i;
    while (next!=RDB_UNUSED) {
        if (next>=v->source_blocks || v->native_part_count>=MAX_RDB_PARTITIONS)
            return 0;
        for (i=0;i<v->native_part_count;i++)
            if (v->native_part_lbas[i]==next) return 0;
        if (!read_at(v->image,(uint64_t)next*SECTOR,sector,SECTOR) ||
            memcmp(sector,"PART",4)!=0 || !rdb_checksum_valid(sector))
            return 0;
        v->native_part_lbas[v->native_part_count++]=next;
        if (disk_get_be32(sector+PART_FLAGS_WORD*4u)&PART_BOOTABLE_FLAG)
            v->bootable_part_count++;
        next=disk_get_be32(sector+4u*4u); /* pb_Next */
    }
    return 1;
}

static int is_ff_root(const uint8_t *block)
{
    uint32_t i, sum=0;
    if (disk_get_be32(block)!=2 ||
        disk_get_be32(block+SECTOR-4)!=1 ||
        disk_get_be32(block+12)!=72) return 0;
    for (i=0;i<SECTOR;i+=4) sum+=disk_get_be32(block+i);
    return sum==0;
}

/* Confirm the root block, not merely a convenient divisor of the file size.
 * The expected FFS root is at half of the filesystem's logical block count.
 * Never use a candidate outside a small terminal-padding window.
 */
static int detect_flat_geometry(struct disk_view *v)
{
    static const uint16_t geometries[][2] = {
        {16,63},{8,63},{4,63},{2,63},{1,63},
        {16,32},{8,32},{4,32},{2,32},{1,32},
        {1,16},{1,8},{1,4},{1,2},{1,1}
    };
    uint8_t root[SECTOR];
    uint32_t padding, g;
    uint32_t limit=v->source_blocks<MAX_FLAT_PADDING_BLOCKS ?
                   v->source_blocks : MAX_FLAT_PADDING_BLOCKS;
    /* Only even padding can preserve the centered classic DOS root block. */
    for (padding=0;padding<=limit;padding+=2) {
        uint32_t blocks=v->source_blocks-padding;
        if (blocks<4) break;
        /* Validate on-disk root BEFORE selecting a geometry. */
        if (!read_at(v->image,(uint64_t)(blocks/2)*SECTOR,root,SECTOR) ||
            !is_ff_root(root)) continue;
        for (g=0;g<sizeof(geometries)/sizeof(geometries[0]);g++) {
            uint32_t cyl=(uint32_t)geometries[g][0]*geometries[g][1];
            if (blocks%cyl==0 && blocks/cyl>0) {
                v->partition_blocks=blocks;
                v->heads=geometries[g][0];
                v->sectors=geometries[g][1];
                v->padding=padding;
                v->verified=1;
                return 1;
            }
        }
    }
    return 0;
}

static void create_virtual_rdb(struct disk_view *v)
{
    uint32_t cylblocks=v->heads*v->sectors;
    uint32_t cylinders=v->partition_blocks/cylblocks;
    uint32_t total_cylinders=cylinders+2u;
    v->partition_start=2u*cylblocks;
    v->total_blocks=v->partition_start+v->partition_blocks;
    memset(v->rdb,0,SECTOR);
    memset(v->part,0,SECTOR);
    disk_word(v->rdb,0,0x5244534bu); /* RDSK */
    disk_word(v->rdb,1,64);
    disk_word(v->rdb,3,7);
    disk_word(v->rdb,4,SECTOR);
    disk_word(v->rdb,5,0x17);
    disk_word(v->rdb,6,RDB_UNUSED);
    disk_word(v->rdb,7,1); /* PART at 1 */
    disk_word(v->rdb,8,RDB_UNUSED);
    disk_word(v->rdb,9,RDB_UNUSED);
    disk_word(v->rdb,16,total_cylinders);
    disk_word(v->rdb,17,v->sectors);
    disk_word(v->rdb,18,v->heads);
    disk_word(v->rdb,19,1);
    disk_word(v->rdb,20,total_cylinders);
    disk_word(v->rdb,24,total_cylinders);
    disk_word(v->rdb,25,total_cylinders);
    disk_word(v->rdb,26,3);
    disk_word(v->rdb,32,0);
    disk_word(v->rdb,33,v->partition_start-1);
    disk_word(v->rdb,34,2);
    disk_word(v->rdb,35,total_cylinders-1);
    disk_word(v->rdb,36,cylblocks);
    disk_word(v->rdb,37,0);
    disk_word(v->rdb,38,1);
    memcpy(v->rdb+160,"L2BOOT  ",8);
    memcpy(v->rdb+168,"VIRTUAL HDF     ",16);
    disk_checksum(v->rdb,64);

    disk_word(v->part,0,0x50415254u); /* PART */
    disk_word(v->part,1,64);
    disk_word(v->part,3,7);
    disk_word(v->part,4,RDB_UNUSED);
    disk_word(v->part,5,1); /* bootable (source may be bootable) */
    v->part[36]=3;
    memcpy(v->part+37,"DH0",3);
    disk_word(v->part,32,16);
    disk_word(v->part,33,128);
    disk_word(v->part,34,0);
    disk_word(v->part,35,v->heads);
    disk_word(v->part,36,1);
    disk_word(v->part,37,v->sectors);
    disk_word(v->part,38,2);
    disk_word(v->part,39,0);
    disk_word(v->part,40,0);
    disk_word(v->part,41,2);
    disk_word(v->part,42,total_cylinders-1);
    disk_word(v->part,43,30);
    disk_word(v->part,44,0);
    disk_word(v->part,45,0x00ffffffu);
    disk_word(v->part,46,0x7ffffffeu);
    disk_word(v->part,47,v->bootpri_override ?
              (uint32_t)(int32_t)v->bootpri : 0u); /* de_BootPri */
    disk_word(v->part,48,disk_get_be32(v->dostype));
    disk_checksum(v->part,64);
}

static int init_disk_view(struct disk_view *v, HANDLE image, uint32_t physical_blocks,
                          int handle_writable, int bootpri_override, int bootpri)
{
    uint8_t sector[SECTOR];
    uint32_t lba;
    int saw_bad_rdb=0;
    memset(v,0,sizeof(*v));
    v->image=image;
    v->source_blocks=physical_blocks;
    v->total_blocks=physical_blocks;
    v->readonly=!handle_writable;
    v->bootpri_override=bootpri_override;
    v->bootpri=bootpri;
    for (lba=0;lba<RDB_SCAN_LIMIT && lba<physical_blocks;lba++) {
        if (!read_at(image,(uint64_t)lba*SECTOR,sector,SECTOR)) return 0;
        if (memcmp(sector,"RDSK",4)!=0) continue;
        if (rdb_checksum_valid(sector) && disk_get_be32(sector+16)!=0 &&
            disk_get_be32(sector+4*4)==SECTOR) {
            uint32_t first_part=disk_get_be32(sector+7u*4u);
            v->native_rdb=1;
            v->verified=1;
            if (first_part==RDB_UNUSED) {
                /* An initialized but not yet partitioned disk can be edited. */
                strcpy(v->reason,"native RDB, no partitions yet");
            } else if (first_part>=physical_blocks ||
                       !read_at(image,(uint64_t)first_part*SECTOR,sector,SECTOR) ||
                       memcmp(sector,"PART",4)!=0 ||
                       !rdb_checksum_valid(sector)) {
                v->readonly=1;
                v->verified=0;
                strcpy(v->reason,"native RDB, first PART invalid: RAW read-only");
            } else {
                strcpy(v->reason,"native RDB and first PART validated");
            }
            if (v->bootpri_override && v->verified &&
                !discover_native_parts(v,first_part)) {
                fprintf(stderr,"--bootpri: invalid, cyclic or oversized native PART chain; refusing override\n");
                return 0;
            }
            return 1;
        }
        saw_bad_rdb=1;
    }
    if (!read_at(image,0,sector,SECTOR)) return 0;
    memcpy(v->dostype,sector,4);
    if (!saw_bad_rdb && memcmp(sector,"DOS",3)==0 && sector[3]<=7) {
        v->readonly=1; /* fail closed until DOS root and geometry validate */
        if (detect_flat_geometry(v)) {
            if ((uint64_t)v->partition_blocks+
                    2u*(uint64_t)v->heads*v->sectors<=0xffffffffULL) {
                v->virtual_rdb=1;
                create_virtual_rdb(v);
                v->bootable_part_count=1;
                v->readonly=!handle_writable;
                strcpy(v->reason,handle_writable ?
                       "flat DOS filesystem: validated root, virtual RDB, read-write partition" :
                       "flat DOS filesystem: validated root, virtual RDB, read-only image handle");
                return 1;
            }
            strcpy(v->reason,"flat DOS filesystem too large for 32-bit virtual disk");
        } else {
            strcpy(v->reason,"flat DOS filesystem: root/geometry ambiguous, RAW read-only");
        }
        return 1;
    }
    /* RAW is writable by default when the image is not a damaged RDB and
     * the host file was opened with write access. A blank image can thus be
     * initialized by HDToolBox without an extra command-line switch. */
    v->raw_write_eligible=!saw_bad_rdb;
    v->readonly=saw_bad_rdb || !handle_writable;
    strcpy(v->reason,saw_bad_rdb ? "invalid RDB: RAW read-only" :
           handle_writable ? "unrecognized/blank image: RAW read-write" :
                             "unrecognized/blank image: RAW read-only image handle");
    return 1;
}

static int view_read(struct disk_view *v, uint32_t lba, uint32_t blocks, uint8_t *dst)
{
    uint32_t i;
    if (!v->virtual_rdb) {
        uint32_t j;
        if (!read_at(v->image,(uint64_t)lba*SECTOR,dst,blocks*SECTOR))
            return 0;
        if (v->native_rdb && v->bootpri_override) {
            for (i=0;i<blocks;i++) {
                uint32_t at=lba+i;
                for (j=0;j<v->native_part_count;j++) {
                    if (at==v->native_part_lbas[j]) {
                        override_part_bootpri(dst+i*SECTOR,v->bootpri);
                        break;
                    }
                }
            }
        }
        return 1;
    }
    for (i=0;i<blocks;i++) {
        uint32_t at=lba+i;
        uint8_t *out=dst+i*SECTOR;
        if (at==0) memcpy(out,v->rdb,SECTOR);
        else if (at==1) memcpy(out,v->part,SECTOR);
        else if (at<v->partition_start) memset(out,0,SECTOR);
        else if (!read_at(v->image,(uint64_t)(at-v->partition_start)*SECTOR,
                          out,SECTOR)) return 0;
    }
    return 1;
}

/* All write requests use the LBA of the disk exposed to AmigaOS. For
 * virtual RDBs, reject every write touching the synthetic RDB, PART or
 * reserved cylinders. Only map complete writes inside the actual flat DOS
 * partition back into the unmodified source image (LBA - partition_start).
 * There is deliberately no partial write for a request crossing a boundary.
 */
static int view_write(struct disk_view *v, uint32_t lba, uint32_t blocks,
                      const uint8_t *src)
{
    uint32_t local_lba;

    if (v->readonly || !blocks) return 0;
    if (!v->virtual_rdb)
        return write_at(v->image,(uint64_t)lba*SECTOR,src,blocks*SECTOR);

    if (lba<v->partition_start) return 0;
    local_lba=lba-v->partition_start;
    if (local_lba>=v->partition_blocks ||
        blocks>v->partition_blocks-local_lba) return 0;

    return write_at(v->image,(uint64_t)local_lba*SECTOR,src,blocks*SECTOR);
}

/* Create a NEW, genuinely RDB-partitioned HDF from a validated flat AmigaDOS
 * image. The old flat image is unchanged. The synthetic RDB/PART and 1022
 * reserved sectors become real storage, so HDToolBox may safely edit them.
 * Do not change LBA mapping in the online companion: this is an offline copy.
 */
static int materialize_rdb(const char *src_path, const char *dst_path)
{
    HANDLE source=INVALID_HANDLE_VALUE, target=INVALID_HANDLE_VALUE;
    LARGE_INTEGER size, result_size;
    struct disk_view v;
    uint8_t *chunk=NULL;
    uint8_t check[SECTOR];
    uint32_t at;
    int ok=0, output_created=0;

    source=CreateFileA(src_path,GENERIC_READ,FILE_SHARE_READ,NULL,
                       OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (source==INVALID_HANDLE_VALUE) {
        fprintf(stderr,"cannot open source HDF (Win32 error %lu)\n",GetLastError());
        goto done;
    }
    if (!GetFileSizeEx(source,&size) || size.QuadPart<SECTOR ||
        (size.QuadPart%SECTOR)!=0 ||
        (uint64_t)(size.QuadPart/SECTOR)>0xffffffffULL) {
        fprintf(stderr,"source size is invalid or exceeds 32-bit LBA range\n");
        goto done;
    }
    if (!init_disk_view(&v,source,(uint32_t)(size.QuadPart/SECTOR),
                        0,0,0) || !v.virtual_rdb || !v.verified ||
        v.partition_blocks>v.source_blocks ||
        v.source_blocks-v.partition_blocks!=v.padding ||
        v.total_blocks<v.partition_blocks ||
        v.total_blocks-v.partition_blocks!=v.partition_start) {
        fprintf(stderr,"source is not a validated flat AmigaDOS HDF; "
                       "nothing converted\n");
        goto done;
    }
    chunk=(uint8_t *)malloc(256u*SECTOR);
    if (!chunk) { fprintf(stderr,"out of memory\n"); goto done; }

    /* The flat geometry detector validated the filesystem root and extent.
     * Bytes beyond that extent are NOT part of the DOS partition.  Never
     * silently drop nonzero trailing data: retain the original unchanged and
     * refuse conversion.  Auto mode retains the entire original in its backup.
     */
    for (at=v.partition_blocks;at<v.source_blocks;) {
        uint32_t n=v.source_blocks-at;
        uint32_t i;
        if (n>256u) n=256u;
        if (!read_at(source,(uint64_t)at*SECTOR,chunk,n*SECTOR)) {
            fprintf(stderr,"cannot inspect trailing padding at source LBA %u\n",at);
            goto done;
        }
        for (i=0;i<n*SECTOR;i++) {
            if (chunk[i]!=0) {
                fprintf(stderr,"nonzero data in trailing padding at source LBA %u "
                               "(byte %u); refusing lossy conversion\n",
                        at+i/SECTOR,i%SECTOR);
                goto done;
            }
        }
        at+=n;
    }
    if (v.padding)
        printf("validated %u zero-filled trailing padding blocks; "
               "normalizing extent during RDB materialization\n",v.padding);

    /* CREATE_NEW guarantees that neither the original image nor an existing
     * destination can be overwritten, even if paths accidentally coincide. */
    target=CreateFileA(dst_path,GENERIC_READ|GENERIC_WRITE,0,NULL,
                       CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if (target==INVALID_HANDLE_VALUE) {
        fprintf(stderr,"cannot create NEW destination HDF (Win32 error %lu)\n",
                GetLastError());
        goto done;
    }
    output_created=1;
    if (!write_at(target,0,v.rdb,SECTOR) ||
        !write_at(target,SECTOR,v.part,SECTOR)) {
        fprintf(stderr,"cannot write native RDB/PART\n");
        goto done;
    }
    memset(chunk,0,256u*SECTOR);
    for (at=2u;at<v.partition_start;) {
        uint32_t n=v.partition_start-at;
        if (n>256u) n=256u;
        if (!write_at(target,(uint64_t)at*SECTOR,chunk,n*SECTOR)) {
            fprintf(stderr,"cannot write reserved RDB region\n");
            goto done;
        }
        at+=n;
    }
    for (at=0u;at<v.partition_blocks;) {
        uint32_t n=v.partition_blocks-at;
        if (n>256u) n=256u;
        if (!read_at(source,(uint64_t)at*SECTOR,chunk,n*SECTOR) ||
            !write_at(target,((uint64_t)at+v.partition_start)*SECTOR,
                      chunk,n*SECTOR)) {
            fprintf(stderr,"cannot copy filesystem at source LBA %u\n",at);
            goto done;
        }
        at+=n;
    }
    if (!FlushFileBuffers(target) || !GetFileSizeEx(target,&result_size) ||
        (uint64_t)result_size.QuadPart!=(uint64_t)v.total_blocks*SECTOR ||
        !read_at(target,0,check,SECTOR) ||
        memcmp(check,v.rdb,SECTOR)!=0 ||
        !read_at(target,SECTOR,check,SECTOR) ||
        memcmp(check,v.part,SECTOR)!=0) {
        fprintf(stderr,"output verification/flush failed\n");
        goto done;
    }
    printf("created native RDB HDF: %s\n",dst_path);
    printf("source=%u sectors; native=%u sectors; "
           "RDB=0 PART=1 filesystem=%u..%u; source preserved\n",
           v.source_blocks,v.total_blocks,v.partition_start,v.total_blocks-1u);
    if (v.padding)
        printf("normalized %u trailing zero-padding blocks; "
               "filesystem copied=%u blocks; original retained in auto-mode backup\n",
               v.padding,v.partition_blocks);
    printf("Use the new HDF with no --bootpri override to allow persistent "
           "HDToolBox partition edits.\n");
    ok=1;
done:
    if (target!=INVALID_HANDLE_VALUE) CloseHandle(target);
    if (source!=INVALID_HANDLE_VALUE) CloseHandle(source);
    free(chunk);
    if (!ok && output_created && !DeleteFileA(dst_path))
        fprintf(stderr,"WARNING: failed to remove incomplete output (%lu)\n",
                GetLastError());
    return ok?0:2;
}

/* One-time conversion at startup, before WinUSB is opened.  Preserve the
 * original flat image in a sibling backup and install the native RDB image
 * under the ORIGINAL name, so future companion invocations use it directly.
 * Every path is on the same volume; no existing backup/staging file is ever
 * overwritten.  A failed second rename attempts to restore the original.
 */
static int auto_materialize_rdb(const char *path)
{
    size_t len=strlen(path);
    char *temp=(char *)malloc(len+64u);
    char *backup=(char *)malloc(len+64u);
    const char *dot=strrchr(path,'.');
    const char *slash=strrchr(path,'\\');
    const char *fslash=strrchr(path,'/');
    DWORD error;
    int ok=0;

    if (!temp || !backup) {
        fprintf(stderr,"auto materialize: out of memory\n");
        goto done;
    }
    sprintf(temp,"%s.l2boot.tmp",path);
    if (dot && (!slash || dot>slash) && (!fslash || dot>fslash)) {
        sprintf(backup,"%.*s-flat-backup%s",(int)(dot-path),path,dot);
    } else {
        sprintf(backup,"%s-flat-backup.hdf",path);
    }
    if (GetFileAttributesA(temp)!=INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesA(backup)!=INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr,"auto materialize: staging or backup file already exists; "
                       "refusing to overwrite it.\n  staging: %s\n  backup: %s\n",
                temp,backup);
        goto done;
    }

    printf("converting flat HDF into real RDB (original protected): %s\n",path);
    fflush(stdout);
    if (materialize_rdb(path,temp)!=0) goto done;

    if (!MoveFileA(path,backup)) {
        error=GetLastError();
        fprintf(stderr,"auto materialize: cannot move source to backup "
                       "(Win32 error %lu); original kept.\n",error);
        if (!DeleteFileA(temp))
            fprintf(stderr,"WARNING: temporary native HDF retained at %s\n",temp);
        goto done;
    }
    if (!MoveFileA(temp,path)) {
        error=GetLastError();
        fprintf(stderr,"auto materialize: cannot install converted HDF "
                       "(Win32 error %lu); restoring original...\n",error);
        if (!MoveFileA(backup,path)) {
            fprintf(stderr,"CRITICAL: original was NOT restored. It is SAFE "
                           "at %s; converted copy at %s.\n"
                           "Do not start the companion until recovered.\n",
                    backup,temp);
        } else if (!DeleteFileA(temp)) {
            fprintf(stderr,"WARNING: temporary native HDF retained at %s\n",temp);
        }
        goto done;
    }
    printf("RDB MATERIALIZED: %s\noriginal flat HDF backup: %s\n",
           path,backup);
    printf("Future starts will see a native RDB; HDToolBox PART changes are persistent.\n");
    fflush(stdout);
    ok=1;
done:
    free(temp);
    free(backup);
    return ok;
}

int main(int argc, char **argv)
{
    HANDLE image;
    LARGE_INTEGER size;
    uint32_t total_blocks;
    uint8_t *buffer;
    struct disk_view view;
    int writable=1, force_readonly=0, allow_raw_write=0, bootpri_override=0, bootpri=0;
    int no_materialize=0, materialize_prompted=0;
    struct io_stats stats;
    int argi;

    memset(&stats,0,sizeof(stats));

    if (argc == 4 && strcmp(argv[1],"materialize-rdb")==0)
        return materialize_rdb(argv[2],argv[3]);
    if (argc == 4 && strcmp(argv[1],"create")==0)
        return image_create(argv[2],argv[3],0);
    if (argc == 4 && strcmp(argv[1],"create-rdb")==0)
        return image_create(argv[2],argv[3],1);

    if (argc<2) goto usage;
    for (argi=2;argi<argc;argi++) {
        if (strcmp(argv[argi],"--stats")==0) {
            if (stats.enabled) goto usage;
            stats.enabled=1;
        } else if (strcmp(argv[argi],"--no-materialize")==0) {
            if (no_materialize) goto usage;
            no_materialize=1;
        } else if (strcmp(argv[argi],"--readonly")==0) {
            if (force_readonly) goto usage;
            force_readonly=1;
        } else if (strcmp(argv[argi],"--allow-raw-write")==0) {
            if (allow_raw_write) goto usage;
            allow_raw_write=1;
        } else if (strcmp(argv[argi],"--bootpri")==0) {
            char *end=NULL;
            long n;
            if (bootpri_override || argi+1>=argc) goto usage;
            errno=0;
            n=strtol(argv[++argi],&end,10);
            if (errno==ERANGE || !end || end==argv[argi] || *end ||
                n< -128 || n>127) {
                fprintf(stderr,"--bootpri requires an integer from -128 to 127\n");
                return 2;
            }
            bootpri=(int)n;
            bootpri_override=1;
        } else goto usage;
    }

reopen_image:
    writable=1;
    image=CreateFileA(argv[1],GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,NULL,
                      OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (image==INVALID_HANDLE_VALUE) {
        writable=0;
        image=CreateFileA(argv[1],GENERIC_READ,FILE_SHARE_READ,NULL,
                          OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    }
    if (image==INVALID_HANDLE_VALUE) {
        fprintf(stderr,"cannot open image (Win32 error %lu)\n",GetLastError());
        return 2;
    }

    if (!GetFileSizeEx(image,&size) || size.QuadPart<SECTOR ||
        (size.QuadPart%SECTOR)!=0 ||
        (uint64_t)(size.QuadPart/SECTOR)>0xffffffffULL) {
        fprintf(stderr,"image must be a non-zero multiple of 512 bytes (<= 2 TiB)\n");
        CloseHandle(image);
        return 2;
    }

    if (!init_disk_view(&view,image,(uint32_t)(size.QuadPart/SECTOR),writable,
                        bootpri_override,bootpri)) {
        fprintf(stderr,"cannot inspect image\n");
        CloseHandle(image);
        return 2;
    }
    /* A flat HDF is exposed via synthetic RDB only until the user explicitly
     * approves a one-time disk conversion.  Never prompt on an active USB
     * session, a read-only source, ambiguous DOS, a damaged RDB, or a native
     * RDB. Trailing padding is inspected during conversion; nonzero bytes
     * cause a safe refusal without changing the original image.
     */
    if (view.virtual_rdb && view.verified && writable &&
        !no_materialize && !force_readonly && !materialize_prompted) {
        int response;
        materialize_prompted=1;
        printf("flat DOS HDF detected: source=%u sectors; exposed=%u sectors "
               "(RDB offset=%u).\n",
               view.source_blocks,view.total_blocks,view.partition_start);
        printf("Offer: create a real RDB with a preserved flat-image backup; "
               "same path after conversion; trailing padding=%u blocks.\n",
               view.padding);
        fflush(stdout);
        response=MessageBoxA(NULL,
            "This image contains a flat AmigaDOS filesystem, not a real RDB.\n\n"
            "Create a genuine RDB now, before connecting to the Amiga?\n"
            "Trailing zero padding, if present, will be normalized.\n"
            "Nonzero trailing data will abort conversion safely.\n\n"
            "YES: preserve the original as *-flat-backup.hdf, install the "
            "converted HDF under the same name, then start the companion.\n"
            "NO: use the current temporary virtual RDB (HDToolBox cannot "
            "save its partition metadata).\n\n"
            "No files are modified unless you select YES.",
            "L2Boot USB - Materialize RDB?",
            MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2|MB_SETFOREGROUND);
        if (response==IDYES) {
            CloseHandle(image);
            image=INVALID_HANDLE_VALUE;
            if (!auto_materialize_rdb(argv[1])) {
                fprintf(stderr,"conversion not completed; refusing to serve "
                               "a potentially ambiguous HDF.\n");
                return 2;
            }
            goto reopen_image;
        }
        printf("materialize declined; flat HDF left unchanged; virtual RDB active.\n");
        fflush(stdout);
    }
    if (allow_raw_write && view.raw_write_eligible && writable && !force_readonly) {
        view.readonly=0;
        strcpy(view.reason,"explicit --allow-raw-write: RAW image read-write");
        fprintf(stderr,"NOTE: --allow-raw-write is retained for compatibility; eligible RAW images are now writable by default.\n");
    } else if (allow_raw_write && view.raw_write_eligible && !writable && !force_readonly) {
        fprintf(stderr,"--allow-raw-write requested, but image handle is read-only; retaining protection.\n");
    } else if (allow_raw_write && !view.raw_write_eligible && !view.native_rdb && !force_readonly) {
        fprintf(stderr,"--allow-raw-write does not override DOS virtual RDB, ambiguous DOS or damaged RDB protection.\n");
    }
    if (force_readonly) {
        view.readonly=1;
        strcpy(view.reason,"explicit --readonly requested");
    }
    total_blocks=view.total_blocks;
    buffer=(uint8_t*)malloc(256u*SECTOR);
    if (!buffer) {
        CloseHandle(image);
        return 2;
    }

    printf("command line: %s\n",GetCommandLineA());
    printf("image: %s; source=%u sectors; presented=%u sectors\n",
           argv[1],view.source_blocks,total_blocks);
    printf("mode: %s; %s\n",view.readonly?"READ-ONLY":"READ-WRITE",view.reason);
    if (bootpri_override) {
        printf("bootpri override: %d; %u bootable PART(s); %s\n",bootpri,
               view.bootable_part_count,
               view.native_rdb ? "native RDB READ overlay" :
               (view.virtual_rdb ? "virtual RDB" : "RAW image, no PART override"));
        fflush(stdout);
    }
    if (view.virtual_rdb) {
        printf("virtual geometry: %u heads x %u sectors; padding=%u blocks; partition=%u blocks\n",
               view.heads,view.sectors,view.padding,view.partition_blocks);
        printf("virtual disk: RDB/PART/reserved LBAs 0..%u always write-protected; "
               "filesystem LBAs %u..%u mapped to image LBAs 0..%u\n",
               view.partition_start-1,view.partition_start,view.total_blocks-1,
               view.partition_blocks-1);
    }
    printf("IDENT FEATURE_WRITE=%u\n",view.readonly?0u:1u);
    fflush(stdout);

    /* Every reconnection has new WinUSB and device handles.  Do not issue a
     * hardware USB reset from Windows: a disconnected device cannot receive
     * one, and resetting a newly enumerated pipe could erase Amiga's IDENT. */
    for (;;) {
        HANDLE dev=INVALID_HANDLE_VALUE;
        WINUSB_INTERFACE_HANDLE usb=NULL;

        printf("waiting for 0525:a4ad...\n");
        fflush(stdout);

        while (dev==INVALID_HANDLE_VALUE) {
            dev=find_device();
            if (dev==INVALID_HANDLE_VALUE)
                Sleep(500);
        }

        if (!WinUsb_Initialize(dev,&usb)) {
            DWORD e=GetLastError();
            stats.errors++;
            fprintf(stderr,"ERROR WinUsb_Initialize (Win32 error %lu); retrying...\n",
                    (unsigned long)e);
            CloseHandle(dev);
            Sleep(500);
            continue;
        }

        if (!configure_session_pipes(usb)) {
            DWORD e=GetLastError();
            stats.errors++;
            fprintf(stderr,"ERROR WinUSB pipe timeout setup (Win32 error %lu); retrying...\n",
                    (unsigned long)e);
            WinUsb_Free(usb);
            CloseHandle(dev);
            Sleep(500);
            continue;
        }

        printf("connected (fresh WinUSB session)\n");
        fflush(stdout);

        /* The disk view/USB wire path is unchanged: only diagnostic output
         * and optional cumulative accounting have been added. */
        stats_emit(&stats,GetTickCount64());
        for (;;) {
            struct request_hdr q;
            ULONG partial=0;

            {
                int r=read_request_header(usb,&q,&partial);
                if (r==2) {
                    /* Publish short transfers even if the disk then goes idle. */
                    stats_emit_due(&stats);
                    continue; /* idle timeout: NOT an error/disconnect */
                }
                if (r==0) {
                    DWORD e=GetLastError();
                    if (partial) {
                        stats.errors++;
                        fprintf(stderr,"ERROR USB partial request header: "
                                       "%lu/%lu bytes (Win32 error %lu)\n",
                                (unsigned long)partial,(unsigned long)sizeof(q),
                                (unsigned long)e);
                    } else if (!expected_usb_disconnect(e)) {
                        stats.errors++;
                        fprintf(stderr,"ERROR USB request header: Win32 error %lu\n",
                                (unsigned long)e);
                    }
                    SetLastError(e);
                    break; /* open a fresh WinUSB session */
                }
            }

            if (q.magic!=REQ_MAGIC || q.version!=VERSION) {
                stats.errors++;
                fprintf(stderr,"ERROR USB invalid header: magic=%08x version=%u "
                               "cmd=%u seq=%08x; discarding session\n",
                        q.magic,q.version,q.command,q.sequence);
                SetLastError(ERROR_INVALID_DATA);
                break; /* Never scan through a desynchronized stream. */
            }

            if (q.command==CMD_IDENT) {
                uint32_t features=view.readonly?0u:1u;
                if (!send_response_checked(&stats,usb,&q,0,
                                           &features,sizeof(features),total_blocks))
                    break;
                continue;
            }

            if (q.blocks > 256u || q.length != q.blocks*SECTOR ||
                q.lba>=total_blocks || q.blocks>total_blocks-q.lba) {
                log_request_error(&stats,"REQUEST",&q,"invalid LBA/blocks/length",
                                  ERROR_SUCCESS);
                if (!send_response_checked(&stats,usb,&q,-2,NULL,0,total_blocks))
                    break;
                stats_emit_due(&stats);
                continue;
            }

            if (q.command==CMD_READ) {
                if (!view_read(&view,q.lba,q.blocks,buffer)) {
                    DWORD e=GetLastError();
                    log_request_error(&stats,"READ",&q,"HDF read failed",e);
                    if (!send_response_checked(&stats,usb,&q,-3,NULL,0,total_blocks))
                        break;
                } else {
                    if (!send_response_checked(&stats,usb,&q,0,buffer,q.length,
                                               total_blocks)) break;
                    /* Count only bytes successfully delivered over USB. */
                    stats.read_bytes+=q.length;
                }
            } else if (q.command==CMD_WRITE) {
                if (!read_exact(usb,EP_IN,buffer,q.length)) {
                    DWORD e=GetLastError();
                    log_request_error(&stats,"USB WRITE payload",&q,
                                      "incomplete transfer",e);
                    SetLastError(e);
                    break;
                }
                if (!view_write(&view,q.lba,q.blocks,buffer)) {
                    DWORD write_error=GetLastError();
                    int synthetic=view.virtual_rdb && q.lba<view.partition_start;
                    int outside=view.virtual_rdb && !synthetic &&
                        (q.lba-view.partition_start>=view.partition_blocks ||
                         q.blocks>view.partition_blocks-(q.lba-view.partition_start));
                    const char *why=view.readonly ? "disk read-only" :
                        synthetic ? "synthetic RDB/reserved region protected" :
                        outside ? "outside virtual partition" : "HDF write failed";
                    DWORD e=(!view.readonly && !synthetic && !outside) ?
                            write_error : ERROR_SUCCESS;
                    log_request_error(&stats,"WRITE",&q,why,e);
                    if (!send_response_checked(&stats,usb,&q,-4,NULL,0,total_blocks))
                        break;
                } else {
                    /* Count successful host writes even if their ACK fails. */
                    stats.write_bytes+=q.length;
                    if (!send_response_checked(&stats,usb,&q,0,NULL,0,total_blocks))
                        break;
                }
            } else if (q.command==CMD_FLUSH) {
                if (!view.readonly && !FlushFileBuffers(image)) {
                    DWORD e=GetLastError();
                    log_request_error(&stats,"FLUSH",&q,"HDF flush failed",e);
                    if (!send_response_checked(&stats,usb,&q,-5,NULL,0,total_blocks))
                        break;
                } else {
                    if (!send_response_checked(&stats,usb,&q,0,NULL,0,total_blocks))
                        break;
                }
            } else {
                log_request_error(&stats,"COMMAND",&q,"unknown command",
                                  ERROR_SUCCESS);
                if (!send_response_checked(&stats,usb,&q,-1,NULL,0,total_blocks))
                    break;
            }
            stats_emit_due(&stats);
        }

        {
            DWORD why=GetLastError();
            stats_emit(&stats,GetTickCount64());
            printf("USB session ended (Win32 error %lu); HDF retained; waiting for reconnect\n",
                   (unsigned long)why);
            fflush(stdout);
        }
        /* No request / frame parser state is retained across iterations. */
        WinUsb_Free(usb);
        CloseHandle(dev);
        usb=NULL;
        dev=INVALID_HANDLE_VALUE;
        Sleep(500);
    }
usage:
    fprintf(stderr,"usage: %s disk.hdf [--readonly] [--no-materialize] [--allow-raw-write] [--bootpri -128..127] [--stats]\n",argv[0]);
    fprintf(stderr,"       %s materialize-rdb flat.hdf native-rdb.hdf\n",argv[0]);
    fprintf(stderr,"       %s create blank.hdf size_MiB\n",argv[0]);
    fprintf(stderr,"       %s create-rdb test.hdf size_MiB\n",argv[0]);
    return 2;
}
