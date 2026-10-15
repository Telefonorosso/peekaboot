# Peekaboot / L2BOOT USB — minimal source audit snapshot

This source-only package is a **curated extract of unchanged files**, not a new
build or a newly validated release. It contains the AmigaOS USB-disk driver,
boot scanner, Direct-DWC2 transport, and the Windows WinUSB HDF companion + GUI.

## Evidence and provenance

**AmigaOS (hardware validated, 28–29 September 2026):** Files copied byte-for-byte
from `L2BOOT_USB_v43r115_ROMSAFE_DIRTY_FLUSH_FIX_DUAL_DEPLOYMENT_L2HUD_V31_PREBOOT_MANAGER_HARDWARE_VALIDATED_MILESTONE(5).zip`.
The supplied milestone records the v43r115 dirty-flush fix and successful
LoadModule/chord-reset deployment on hardware. The matching `l2remote.boot`
requires `l2scsi.device` revision **43.115**.

**Windows (historical source snapshot, 25 September 2026):** Files copied byte-for-byte
from `L2BOOT_USB_2026-09-25_RDB_PADDING_ERROR_ONLY_THROUGHPUT_NO_RAW_SOURCE_MILESTONE(1).zip`.
The milestone records hardware observation of the HDF materialization path, but
**does not independently establish hardware validation of this exact
ERRORS-TELEMETRY revision or of the GUI**. The later v43r115 milestone states
that the Windows companion and USB wire protocol were unchanged; it does not
identify the executable checksum tested. Accordingly, this ZIP does **not**
claim that these exact Windows sources were jointly hardware validated with
v43r115. Rebuild and binary/source matching are outside the evidence provided.

Validated platform as reported for the present release:
Amiga 600 + PiStorm Classic + Emu68 1.1 + Raspberry Pi 3A+.

## Files

- `amigaos/l2scsi-device-romsafe-v43r115.c`: SANA-independent SCSI/TrackDisk
  device frontend and cache/dirty-flush behavior.
- `amigaos/rdisk_direct.c`, `rdisk_direct.h`, `rdisk_diag.h`:
  Direct-DWC2 USB transport and diagnostic definitions.
- `amigaos/l2remote-boot-v3.4-ROMSAFE.c`, `l2boot-diag.h`:
  boot-time RDB discovery and related diagnostics.
- `amigaos/l2scsi-resident-ROM.s`, `l2scsi-singlehunk-rom.ld`:
  Resident entry metadata and linker layout used by this exact source branch.
- `amigaos/build-romsafe.sh`: historical build commands and symbol checks.
- `windows/rdisk_host_winusb-ERRORS-TELEMETRY.c`: WinUSB host, HDF
  identification, RDB materialization, READ/WRITE/FLUSH, reconnection and stats.
- `windows/l2boot-gui-THROUGHPUT-ERRORS-NO-RAW.c`:
  standalone native Win32 GUI wrapper.
- `windows/build-windows.bat`: historical Windows build commands.

## Deliberately excluded

No binaries, disk images, ROMs, HUD/Framethrower UI, early candidates,
legacy baselines, test logs, patches, or unrelated utilities.
`ptable.library` **is an external runtime dependency** for the boot scanner,
not included as binary or source in the supplied milestone. Therefore this
archive cannot by itself provide a complete source audit of that third-party
library. Windows SDK/WinUSB and AmigaOS NDK are likewise external dependencies.

The resident assembly file retains an older `43.111` descriptive ID string;
that is present in the unmodified original despite the device C revision
43.115. It must not be mistaken for a different source snapshot.

## Integrity

`SHA256SUMS.txt` lists SHA-256 digests of every included source/build file.
All source files are copied without modification.
