/* position-independent SD binary: header at 0, entry at 8 */
ENTRY(f286_header)
SECTIONS
{
    . = 0;
    .text : {
        KEEP(*(.f286hdr))
        KEEP(*(.text.entry))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(8);
    }
    /* -fropi has no RW relocation support: writable data must not exist */
    .data : { *(.data .data.* .bss .bss.* COMMON) }
    ASSERT(SIZEOF(.data) == 0, "app must not have writable data; use APP_STATE")
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
