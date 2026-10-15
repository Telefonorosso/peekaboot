OUTPUT_FORMAT(amiga)
SECTIONS
{
  .text 0 :
  {
    KEEP(*(.text.romtag))
    KEEP(*(.text))
    KEEP(*(.text.*))
    *(.rodata)
    *(.rodata.*)
    *(.data)
    *(.data.*)
    *(.bss)
    *(.bss.*)
    *(COMMON)
    . = ALIGN(4);
    _ROMModuleEnd = .;
  }
}
