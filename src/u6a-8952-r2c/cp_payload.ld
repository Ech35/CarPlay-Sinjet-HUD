ENTRY(hook_carplay_send)
SECTIONS {
  .payload 0xf000 : ALIGN(4) {
    KEEP(*(.text.entry))
    *(.text .text.*)
    *(.rodata .rodata.*)
  }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.exidx*) *(.ARM.extab*) }
  ASSERT(SIZEOF(.payload) < 0x8000, "CarPlay diagnostic payload too large")
}
