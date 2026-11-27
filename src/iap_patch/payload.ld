ENTRY(probe_outbound)
SECTIONS {
  . = 0x1e000;
  .payload : {
    *(.text.entry) *(.text .text.*) *(.rodata .rodata.*)
  }
  /DISCARD/ : {
    *(.ARM.exidx*) *(.ARM.extab*) *(.eh_frame*) *(.note*)
  }
}
