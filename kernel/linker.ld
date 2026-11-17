/*
 * RadiaNT kernel image layout.
 * The Multiboot2 stub lives at 1 MiB (identity mapped); everything else is
 * linked in the top 2 GiB and loaded immediately behind the stub.
 */
ENTRY(_start)
KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    . = 1M;
    .boot : {
        KEEP(*(.multiboot))
        *(.boot.text)
        *(.boot.data)
    }
    .boot.bss (NOLOAD) : ALIGN(4K) { *(.boot.bss) }

    . = ALIGN(4K);
    . += KERNEL_VMA;
    KiImageStart = .;

    .text : AT(ADDR(.text) - KERNEL_VMA) { *(.text .text.*) }
    . = ALIGN(4K);
    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) { *(.rodata .rodata.*) }
    . = ALIGN(4K);
    .data : AT(ADDR(.data) - KERNEL_VMA) { *(.data .data.*) }
    . = ALIGN(4K);
    .bss : AT(ADDR(.bss) - KERNEL_VMA) { *(COMMON) *(.bss .bss.*) }
    . = ALIGN(4K);
    KiImageEnd = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
