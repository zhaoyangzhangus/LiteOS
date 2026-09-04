OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)

PHDRS
{
    text PT_LOAD FLAGS(5);  /* R + X */
    rodata PT_LOAD FLAGS(4); /* R */
    data PT_LOAD FLAGS(6);  /* R + W */
}

SECTIONS
{
    . = 0xffffffff80000000;

    .text ALIGN(0x1000) :
    {
        KEEP(*(.text.kernel_entry))
        KEEP(*(.text.entry))
        *(.text .text.* .text$*)
    } :text

    .rodata ALIGN(0x1000) :
    {
        *(.rodata .rodata.*)
        *(.rdata .rdata.* .rdata$*)
        *(.pdata .pdata.* .xdata .xdata.*)
    } :rodata

    .data ALIGN(0x1000) :
    {
        *(.data .data.* .data$*)
    } :data

    .bss ALIGN(0x1000) (NOLOAD) :
    {
        *(COMMON)
        *(.bss .bss.* .bss$*)
    } :data

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame)
        *(.debug*)
    }
}
