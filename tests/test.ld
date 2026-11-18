ENTRY(test_entry)
MEMORY {
 flash (rx) : ORIGIN = 0x10000, LENGTH = 1M
 ram (rwx) : ORIGIN = 0x20000000, LENGTH = 1M
}
SECTIONS {
 .text : { *(.text*) *(.rodata*) } > flash
 .data : { *(.data*) } > ram
 .bss : { *(.bss*) *(COMMON) } > ram
 /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
