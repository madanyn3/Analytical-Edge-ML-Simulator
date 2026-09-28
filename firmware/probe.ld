ENTRY(probe_entry)
MEMORY {
 FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 2M
 SRAM1 (rw) : ORIGIN = 0x20000000, LENGTH = 192K
 SRAM3 (rw) : ORIGIN = 0x20040000, LENGTH = 384K
}
SECTIONS {
 .text : { *(.text*) *(.rodata*) } > FLASH
 .ARM.exidx : { *(.ARM.exidx*) } > FLASH
 .data : { *(.data*) } > SRAM1 AT> FLASH
 .bss (NOLOAD) : { *(.bss*) *(COMMON) } > SRAM1
 .tensor_input (NOLOAD) : { *(.tensor_input) } > SRAM3
 .tensor_output (NOLOAD) : { *(.tensor_output) } > SRAM3
 .stack (NOLOAD) : { . = ALIGN(8); . += 4096; } > SRAM1
}
