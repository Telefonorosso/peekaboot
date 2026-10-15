/* Kickstart Extended-ROM Resident for l2scsi.device 43.111 ROMSAFE.
 * DevInit runs before l2remote.boot.  The linker-owned _ROMModuleEnd
 * makes rt_EndSkip cover the complete ROM module, not merely the ROMTag. */
        .section .text.romtag,"ax"
        .globl  _ROMTag
        .globl  _DevInitTab
        .globl  _ROMModuleEnd

_ROMTag:
        .word   0x4afc
        .long   _ROMTag
        .long   _ROMModuleEnd
        .byte   0x81              /* RTF_AUTOINIT | RTF_COLDSTART */
        .byte   43                /* version */
        .byte   3                 /* NT_DEVICE */
        .byte   21                /* before l2remote.boot */
        .long   _DevName
        .long   _DevId
        .long   _DevInitTab

_DevName:
        .asciz  "l2scsi.device"
        .even

_DevId:
        .asciz  "l2scsi.device 43.111 ROMSAFE Direct-DWC2 COLDSTART"
        .even
