#!/bin/sh
set -eu
mkdir -p romsafe

# ROMSAFE build must not depend on the traditional global SysBase or libnix
# memory routines.  Fail before linking if either regression is reintroduced.
if grep -nE '#include <proto/exec.h>|\b(memcpy|memset|CachePreDMA|CachePostDMA|AddIntServer|Signal|Wait)\s*\(' rdisk_direct.c | grep -vE 'l2_memcpy|l2_memset|exec_CachePreDMA|exec_CachePostDMA|exec_AddIntServer|exec_Signal|exec_Wait'; then
  echo 'ERROR: implicit Exec/libc call remains in rdisk_direct.c' >&2
  exit 1
fi
if grep -nE '#include <string.h>|[^A-Za-z0-9_](memcpy|memset)\s*\(' l2scsi-device-romsafe-v43r115.c; then
  echo 'ERROR: libnix memory call remains in device source' >&2
  exit 1
fi

m68k-amigaos-gcc -O2 -m68020 -Wall -Wextra -noixemul -nostartfiles \
  -fno-builtin -fno-tree-loop-distribute-patterns -ffunction-sections -fdata-sections \
  l2scsi-device-romsafe-v43r115.c rdisk_direct.c l2scsi-resident-ROM.s \
  -Wl,-T,l2scsi-singlehunk-rom.ld -o romsafe/l2scsi.device

m68k-amigaos-gcc -Os -m68020 -Wall -Wextra -noixemul -nostartfiles -nostdlib \
  -fno-builtin -ffunction-sections -fdata-sections \
  l2remote-boot-v3.4-ROMSAFE.c \
  -Wl,-T,l2scsi-singlehunk-rom.ld -o romsafe/l2remote.boot

m68k-amigaos-objdump -h romsafe/l2scsi.device romsafe/l2remote.boot
m68k-amigaos-nm romsafe/l2scsi.device | grep -E '_ROMTag|_ROMModuleEnd|_DevInit|_DevInitTab'
m68k-amigaos-nm romsafe/l2remote.boot | grep -E '_L2BootROMTag|_ROMModuleEnd|_l2boot_entry'

echo
echo 'ROM-safety symbol check (must print only the label below, no mutable globals):'
echo '--- mutable globals ---'
m68k-amigaos-nm romsafe/l2scsi.device | grep -E '(^|[[:space:]])(_SysBase|_g_payload|_g_rx|_g_prop|_dma_maps|_usb_irq|_usb_vblank)$' && {
  echo 'ERROR: mutable backend globals still present'; exit 1;
} || true

echo
echo 'Expected: l2scsi.device should now be dramatically smaller than the old ~274 KiB LoadModule image.'
ls -lh romsafe/l2scsi.device romsafe/l2remote.boot
