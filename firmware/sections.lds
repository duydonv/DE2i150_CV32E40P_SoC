ENTRY(_start)

MEMORY {
    RAM (xrw) : ORIGIN = 0x00000000, LENGTH = 0x8000
}

SECTIONS {
    . = ORIGIN(RAM);

    .text : {
        KEEP(*(.text._start))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > RAM

    .data : {
        *(.data*)
        *(.sdata*)
        . = ALIGN(4);
    } > RAM

    .bss (NOLOAD) : {
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(4);
    } > RAM

    _stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
