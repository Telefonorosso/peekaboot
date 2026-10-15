# L2BOOT USB — Installation and User Guide

**Release:** v43r115 ROMSAFE / l2hud.boot 3.1  
**Platform:** Amiga 600/1200 with PiStorm and Emu68; Windows PC hosting the disk image over USB  
**Status:** Hardware validated on the documented Amiga 1200 / PiStorm / Raspberry Pi 3 A+ setup (28–30 September 2026)

L2BOOT USB lets an Amiga use an HDF file stored on a Windows PC as a disk attached to **`l2scsi.device`, unit 0**. It can boot AmigaOS directly from that image, or boot a local disk and access the remote HDF as additional storage. There is no TCP/IP stack or 3Com Ethernet card involved in this USB edition.

This guide is for **installation and everyday operation only**. It does not cover software compilation or ROM generation.

> **Package note:** The accompanying v43r115 milestone ZIP is primarily a source and validation archive. It includes `known-good-module/ptable.library`, but **does not contain the complete ready-to-run binary set**, a Windows companion executable, or a finished 2 MiB Kickstart image. Obtain matching prebuilt files separately before following the steps below. The older, Ethernet-based L2BOOT release is **not** a substitute for the USB binaries.

## 1. What you need

### Hardware

- An Amiga with a PiStorm running a compatible Emu68 configuration.
- The validated reference setup is an **Amiga 1200 with Raspberry Pi 3 A+**.
- A Windows PC and the working USB data connection used for Emu68's Direct-DWC2 device mode.
- For the on-screen pre-boot manager, a supported **Framethrower / HVS RGB565 display path**, as used in the hardware validation.

### Ready-to-run software

| File | Purpose |
| --- | --- |
| `rdisk_host_winusb.exe` | Windows WinUSB companion; serves the HDF over USB |
| `disk.hdf` | Your Amiga hard disk image, preferably with a native RDB |
| `ptable.library` **1.1** | Discovers RDB partitions and creates boot nodes |
| `l2scsi.device` **43.115 ROMSAFE** | Amiga USB block device, unit 0 |
| **One** boot resident: `l2remote.boot` **3.4**, paired with revision 115, **or** `l2hud.boot` **3.1** | Automatic boot or interactive boot manager |
| `LoadModule` | Required **only** for the RAM-resident installation method |
| A prepared **2 MiB Extended Kickstart image** containing the matching resident set | Required **only** for the Extended-ROM method |

The proprietary Amiga Kickstart ROM is not included in the milestone ZIP. Use a legally obtained Kickstart appropriate to your machine. Do not mix the supplied older GitHub Ethernet-driver binaries or earlier `l2scsi.device`/boot-resident revisions with this release.

## 2. Choose how L2BOOT starts

There are **two installation methods** and **two alternative boot front-ends**:

| Choice | What happens | Recommended use |
| --- | --- | --- |
| **Extended ROM + `l2remote.boot`** | The USB disk is scanned automatically at cold start | Normal automatic USB boot; no AmigaOS-side loader setup |
| **LoadModule + `l2remote.boot`** | The USB disk is scanned automatically after the loader's reset | Stock Kickstart, testing or alternate deployment |
| **LoadModule + `l2hud.boot`** | An early boot menu offers USB or local boot | Interactive boot selection with Framethrower/HVS |
| **Extended ROM + `l2hud.boot`** | The menu resident is included instead of the automatic resident | Use only a prepared image containing the matching HUD stack |

**Important:** `l2remote.boot` and `l2hud.boot` are replacements for one another. **Never load both together.** The driver is always `l2scsi.device` **43.115** and the library is `ptable.library` **1.1**.

## 3. Prepare the HDF on Windows

1. Make a backup of the original HDF before exposing it as writable storage.
2. Prefer an HDF with a valid **Amiga Rigid Disk Block (RDB)** and one or more Amiga partitions.
3. For USB boot, at least one partition must be bootable, contain a working AmigaOS installation and have an appropriate **BootPri**.
4. Put the HDF in a convenient Windows folder, for example `C:\L2BOOT\disk.hdf`.
5. Do not open the same HDF for simultaneous read/write access in another emulator, disk editor or mounting tool.

Some versions of the Windows companion support converting a flat DOS-style HDF into a native RDB-backed image. This is **companion-version-specific**; do not assume that every binary supports it. For the most predictable first test, start with an existing native RDB HDF.

## 4. Connect and start the Windows companion

1. Connect the PiStorm/Emu68 USB device-mode interface to the Windows PC using your known-working USB data setup.
2. Ensure Windows has a **WinUSB** driver associated with the L2BOOT USB device. If a driver must be assigned manually, identify the correct L2BOOT interface before modifying its driver binding; do not change unrelated USB devices.
3. Open Command Prompt or PowerShell in the folder containing the matching companion executable.
4. Start the server with your HDF path:

```bat
rdisk_host_winusb.exe "C:\L2BOOT\disk.hdf"
```

Keep this program running while using the remote disk. The companion waits for the Amiga USB device and handles disk commands. In a working session, its diagnostics can show the connection and requests such as **IDENT**, **READ**, **WRITE** and **FLUSH**.

The normal validated HDF workflow is **read/write**. For initial experiments, use a copy of the HDF. If your companion build supports the `--readonly` switch, you can use it for a read-only session:

```bat
rdisk_host_winusb.exe "C:\L2BOOT\disk.hdf" --readonly
```

A read-only image can be inspected and read, but AmigaOS changes cannot be saved to it. Do not assume that options from the older Ethernet server apply to the Windows USB companion.

## 5. Installation A — Extended-ROM boot

Use this method when you already have a **prepared, matching 2 MiB Extended Kickstart image** containing `ptable.library`, `l2scsi.device` 43.115, and exactly one boot resident.

1. Copy the prepared ROM image to the Emu68 boot partition.
2. Edit the Emu68 configuration that selects the Kickstart image. For the conventional `config.txt` setup, use an entry of this form (adjust the filename):

```ini
initramfs kick-l2boot-romsafe-full-2mb.rom
```

3. **Remove or disable any previous L2BOOT `LoadModule` command** from the Amiga startup configuration. Do not load a second copy of `l2scsi.device` from `DEVS:` over the ROM-resident driver.
4. Start the Windows companion with the intended HDF.
5. Power on or reset the Amiga.

With the **automatic** front-end, Kickstart probes the USB disk, scans its RDB and offers its bootable partitions to the normal boot process. If the HDF has a suitable boot priority, the Amiga boots from Windows-hosted storage. If the companion is unavailable, the validated automatic setup can continue with local boot candidates.

**Note:** The historical `kick-l2boot-romsafe-full-2mb.rom` filename denotes the conventional **automatic** ROM image. A ROM containing the **interactive HUD** must specifically include `l2hud.boot` **instead of** `l2remote.boot`; changing the filename alone does not enable the menu.

## 6. Installation B — LoadModule boot

This method uses the stock Kickstart and the matching ready-to-run Amiga modules. **v43r115 was hardware validated with LoadModule followed by a keyboard chord reset.**

1. Boot your existing local AmigaOS installation.
2. Create a directory for the matching modules, for example `SYS:L2BOOT/`, and copy these files into it:
   - `ptable.library` 1.1
   - `l2scsi.device` 43.115 ROMSAFE
   - **Either** `l2remote.boot` 3.4 (revision-115 matched) **or** `l2hud.boot` 3.1
3. Ensure `LoadModule` is installed and available from the Amiga Shell.
4. Start the Windows companion on the PC.
5. Open an Amiga Shell and enter:

```amiga
CD SYS:L2BOOT
```

For **automatic USB boot**:

```amiga
LoadModule ptable.library l2scsi.device l2remote.boot noreboot
```

For the **interactive boot menu**:

```amiga
LoadModule ptable.library l2scsi.device l2hud.boot noreboot
```

6. Perform a **Ctrl + Left Amiga + Right Amiga** reset. The modules remain available for the ensuing COLDSTART in the validated configuration.
7. Continue with automatic boot or the HUD menu, depending on which resident you selected.

Do not issue the two `LoadModule` commands together. Avoid duplicated loader invocations in startup scripts. If you later switch to the Extended-ROM installation, disable the RAM-loader setup first.

## 7. Using the interactive pre-boot manager

With `l2hud.boot` **3.1** installed, the HUD appears during early boot on the validated Framethrower/HVS display setup. It probes `l2scsi.device`, checks disk geometry, reads block zero and checks the available mode before offering boot choices.

### What the screen tells you

| Screen text | Meaning |
| --- | --- |
| `COMPANION CONNECTED` | USB disk geometry was obtained successfully |
| `COMPANION NOT READY` | The remote disk was not ready during the probe |
| `RDB FOUND` | Block zero was read and its RDB signature recognized |
| `BLOCK ZERO READ OK` | Block zero was readable, but did not contain that RDB signature |
| `MODE READ WRITE` / `MODE READ ONLY` | Remote medium access mode reported by the device |
| `PTABLE READY` / `PTABLE MISSING` | Availability of the partition-table library |
| `USB BOOT UNAVAILABLE` | Required pre-boot checks did not all succeed |

### Keyboard controls

| Key | Action |
| --- | --- |
| **Up / Down arrow** | Change the selected boot option |
| **Return / numeric Enter** | Confirm the selection |
| **Esc** | Select **LOCAL BOOT USB MOUNT** directly |

After a selection, **release all keys** when prompted. The HUD deliberately clears pending keyboard events before handing control back to Kickstart.

### `USB BOOT`

Select **USB BOOT** to register the USB HDF's RDB partitions and continue with Kickstart's normal boot selection. A bootable HDF partition must still have a suitable BootPri; the menu does **not** overwrite the disk's BootPri or forcibly replace Kickstart's boot priority rules.

### `LOCAL BOOT USB MOUNT`

Select **LOCAL BOOT USB MOUNT** to keep the **local** disk as the preferred boot source **while also registering the USB HDF partitions**.

The HUD moves only the newly registered USB BootNodes behind the existing local boot candidates and assigns them priority **-128** in the in-memory boot list. This does **not** rewrite the HDF's partition table. After AmigaOS starts, the USB partitions can appear as normal mounted volumes and the disk remains available to HDToolBox.

If the USB companion is unavailable, choose local boot; automatic mounting of the missing remote disk is not possible. If the HUD cannot safely register USB nodes, it prioritizes preserving local boot rather than risking an unintended USB boot.

## 8. Working with the HDF after boot

### Access files

1. Keep the Windows companion running.
2. Open the remote HDF's mounted partition icons in Workbench, or use `Info` in the Shell to list mounted volumes.
3. Read and copy files as with another Amiga disk.
4. When in read/write mode, create a small test file and verify that it survives a subsequent clean reboot.

For the HUD's **LOCAL BOOT USB MOUNT** option, RDB registration is automatic; there is no need to run `l2mount-rdb` just to make those USB partitions available. The separate `l2mount-rdb` utility remains compatible for workflows that require a manual runtime mount, but it is **not included as a ready-to-run executable in this milestone ZIP**.

### Inspect partitions in HDToolBox

1. Boot AmigaOS locally or from USB, with the companion connected.
2. Start HDToolBox and configure its device name as **`l2scsi.device`**.
3. Select **unit 0**.
4. Inspect the detected drive and its RDB partitions.
5. Before altering partitions, make a complete backup of the HDF. Saving partition changes modifies the disk image.
6. After intentional RDB or BootPri changes, reboot and check that the new partition configuration is recognized.

### Change which disk boots automatically

For the **automatic** resident (`l2remote.boot`), the remote and local disks participate in normal boot priority selection. To prefer USB, ensure the intended remote system partition is bootable and has an adequate BootPri. To prefer local boot, lower the remote priority or stop the companion before booting. The interactive HUD provides a session-specific choice without requiring an RDB edit.

### End a session or change HDF

1. Finish any active AmigaOS file operations and allow pending disk updates to complete.
2. Shut down or reboot into a state that is not using the remote disk.
3. Stop the Windows companion **only after disk activity has ceased**.
4. To change HDF files, restart the companion with the new path and reset/reboot the Amiga as needed.

**Hot media replacement is not a supported workflow.** Do not replace a live HDF or unplug the USB connection while writes are in progress.

## 9. Quick functional check

With the companion online and a remote RDB disk available:

```amiga
Version l2scsi.device
Info
List ALL
```

- Verify that the active driver is **43.115**.
- Confirm that the expected disk/volumes are visible.
- Run `List ALL` several times. The v43r115 hardware validation specifically addressed the earlier small-READ failure seen during this workload.
- If you have the matching, separately provided `rdisk-readfail-diag` executable, run:

```amiga
rdisk-readfail-diag
```

The normal clean diagnostic result includes:

```text
DEVICE READ FAILURE: latched=0
TRANSPORT READ FAILURE: latched=0
LAST STALE RESPONSE DURING READ: seen=0
```

For a writable image, also verify that a test file persists after a proper disk update and reboot. Read tests alone do not prove write persistence.

## 10. Troubleshooting

| Symptom | What to check |
| --- | --- |
| Windows companion never connects | Correct USB device-mode connection, correct WinUSB driver binding, companion running, and Amiga power/reset state |
| `COMPANION NOT READY` | HDF path, companion readiness, WinUSB connection, USB cable/device interface; retry with the companion running before reboot |
| `USB BOOT UNAVAILABLE` | Driver 43.115, `ptable.library` 1.1, readable block zero and valid geometry; consult the HUD status lines |
| USB disk found but local AmigaOS boots instead | The HDF may not have a bootable system partition or its BootPri may be lower than the local disk's |
| Remote partitions do not appear after local boot | Use the HUD's **LOCAL BOOT USB MOUNT** option; verify the companion and RDB are accessible; check `Info` |
| HUD not visible | Use the validated Framethrower/HVS-compatible display path, or return to the automatic `l2remote.boot` front-end; HUD rendering is not guaranteed on other video paths |
| No RDB detected | Verify that the HDF is a native RDB image rather than an unsupported flat image; check the image in a disk utility before altering it |
| Write or format fails | Check read-only mode, image permissions, free Windows disk space and the companion's error output |
| Unstable boot or unexpected resident version | Remove mixed/duplicate ROM or LoadModule installations and use a matched v43r115 set with **one** boot resident |
| READ failure reappears | Run the matching `rdisk-readfail-diag`, record the first error, and preserve the Windows companion output for diagnosis |

**Compatibility boundary:** This guide reflects the documented hardware-validated milestone; it does not claim validation for every Amiga model, PiStorm board, display mode, HDF layout or Windows USB configuration.

## Credits

**L2BOOT project and USB adaptation:** [Francesco Talarico / Telefonorosso](https://github.com/Telefonorosso/l2boot). **`ptable.library`:** Jaroslav Pulchart. **Original `compactflash.device` / CFD project:** Torsten Jager. **LoadModule:** Thomas Richter and Etienne Vogt ([Aminet](https://aminet.net/package/util/boot/LoadModule)). The project also builds on the work of the **Emu68**, **PiStorm** and Amiga development communities; **WinUSB** is a Microsoft Windows interface. The original [L2BOOT repository](https://github.com/Telefonorosso/l2boot) documents the project's Ethernet ancestry; this guide applies specifically to the later **USB v43r115 ROMSAFE** milestone.
