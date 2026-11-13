ENTRY(run_tests)
MEMORY { flash (rx) : ORIGIN = 0x10000, LENGTH = 512K
         ram (rw) : ORIGIN = 0x20000000, LENGTH = 512K }
SECTIONS {
 .text : { *(.text*) *(.rodata*) } > flash
 .data : { *(.data*) } > ram
 .bss (NOLOAD) : { *(.bss*) *(COMMON) } > ram
 /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.note*) *(.comment*) }
}
