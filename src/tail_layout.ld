/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
OUTPUT_FORMAT("elf32-shl")
ENTRY(_xdj_dispatch_trampoline)

SECTIONS
{
    . = 0x091BD788;
    .text 0x091BD788 : SUBALIGN(4)
    {
        KEEP(*(.text.xdj_dispatch_trampoline))
        KEEP(*(.text.xdj_pcm_trampolines))
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss (NOLOAD) :
    {
        __xdj_tail_bss_start = .;
        *(.bss*)
        *(COMMON)
        __xdj_tail_bss_end = .;
    }

    /DISCARD/ :
    {
        *(.stack*)
        *(.comment*)
        *(.debug*)
    }
}
