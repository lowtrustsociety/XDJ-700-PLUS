/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Where FLAC decoded on the audio processor goes (src/chip_flac.c,
 * chip_flac_glue.S; addresses as in chip_flac.h).
 */
OUTPUT_FORMAT("elf32-tic6x-le")
xdj_chip_stock = 0xC003C558;
xdj_chip_stack = 0xC1F5C000 - 8;
SECTIONS
{
    /* The block, after the status words: the entry code first (XDJ_CHIP_ENTRY). */
    .text 0xC1F40200 : { KEEP(*(.entry)) *(.text*) }
    .rodata : { *(.rodata*) *(.const*) *(.switch*) }
    .data : { *(.data*) *(.fardata*) *(.neardata*) KEEP(*(.entry_data)) }
    __xdj_chip_load_end = .;
    .bss (NOLOAD) : { *(.bss*) *(.far*) *(COMMON) }
    __xdj_chip_end = .;
    /DISCARD/ : { *(.c6xabi.attributes) *(.comment) *(.note*) *(.c6xabi.exidx*) *(.c6xabi.extab*) }
}
ASSERT(__xdj_chip_end <= 0xC1F58000, "the block does not fit its place")
