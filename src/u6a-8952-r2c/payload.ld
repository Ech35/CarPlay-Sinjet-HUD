ENTRY(hook_ident)
SECTIONS {
  .payload 0x21000 : ALIGN(4) {
    KEEP(*(.text.entry))
    *(.text .text.*)
    *(.rodata .rodata.*)
  }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.exidx*) *(.ARM.extab*) }
  ASSERT(SIZEOF(.payload) < 0xe000, "payload too large")
}
