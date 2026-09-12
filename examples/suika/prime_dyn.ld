OUTPUT_ARCH(arm)
ENTRY(main)

PHDRS
{
    load    PT_LOAD    FLAGS(7);
    dynamic PT_DYNAMIC FLAGS(6);
}

SECTIONS
{
    . = 0x00000000;

    .text : ALIGN(4)
    {
        KEEP(*(.text.entrypad))
        KEEP(*(.text.main))
        *(.text*)
    } :load

    .rodata : ALIGN(4)
    {
        *(.rodata*)
    } :load

    .dynsym : ALIGN(4)
    {
        *(.dynsym)
    } :load

    .dynstr : ALIGN(1)
    {
        *(.dynstr)
    } :load

    .hash : ALIGN(4)
    {
        *(.hash)
    } :load

    .data : ALIGN(4)
    {
        *(.data*)
        *(.got*)
    } :load

    .dynamic : ALIGN(4)
    {
        *(.dynamic)
    } :load :dynamic

    .rel.dyn : ALIGN(4)
    {
        *(.rel.dyn)
    } :load

    .bss : ALIGN(8)
    {
        *(.bss*)
        *(COMMON)
    } :load

    /DISCARD/ :
    {
        *(.interp)
        *(.note*)
        *(.eh_frame*)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
