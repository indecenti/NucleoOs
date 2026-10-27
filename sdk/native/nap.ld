/* NucleoOS native app (.nap), ABI v1 - linker script. Spec: docs/native-apps.md.
 *
 * Derived from binutils' default Xtensa "-shared" script (FSF permissive notice below), reduced to the
 * layout the loader accepts: exactly three load segments, in this order, starting at offset 0:
 *
 *   text    R+X   code, PLT stubs, literal pools     mapped on the instruction bus (from flash)
 *   rodata  R     constants, strings, jump tables    mapped on the data bus (same flash pages)
 *   data    RW    GOT, init arrays, .data, .bss      copied to / zeroed in RAM at every launch
 *
 * The loader relocates every pointer, so the image is position independent and nothing is linked against
 * a firmware address: imports are resolved by name against the firmware's ABI export table at launch.
 *
 * Copyright (C) 2014-2024 Free Software Foundation, Inc.
 * Copying and distribution of this script, with or without modification, are permitted in any medium
 * without royalty provided the copyright notice and this notice are preserved. */
ENTRY(nucleo_native_main)
PHDRS
{
  text    PT_LOAD FILEHDR PHDRS FLAGS(5);
  rodata  PT_LOAD FLAGS(4);
  data    PT_LOAD FLAGS(6);
  dynamic PT_DYNAMIC FLAGS(6);
}
SECTIONS
{
  . = 0 + SIZEOF_HEADERS;
  .hash           : { *(.hash) }                                         :text
  .dynsym         : { *(.dynsym) }                                       :text
  .dynstr         : { *(.dynstr) }                                       :text
  .rela.dyn       :
    {
      *(.rela.init) *(.rela.text .rela.text.*) *(.rela.fini)
      *(.rela.rodata .rela.rodata.*) *(.rela.data .rela.data.*)
      *(.rela.ctors) *(.rela.dtors) *(.rela.got) *(.rela.bss .rela.bss.*)
    }                                                                    :text
  .rela.plt       : { *(.rela.plt) }                                     :text
  .text           :
  {
    *(.got.plt* .plt*)
    *(.literal .text .stub .literal.* .text.*)
  }                                                                      :text =0
  .got.loc        : { *(.got.loc) }                                      :text

  . = ALIGN(16);
  .rodata         : { *(.rodata .rodata.* .rodata1) }                    :rodata
  .eh_frame       : { KEEP (*(.eh_frame)) }                              :rodata
  .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) }       :rodata

  . = ALIGN(16);
  /* GCC for Xtensa emits constructors in .ctors / destructors in .dtors: they join the init / fini arrays the
     loader runs (DT_INIT_ARRAY / DT_FINI_ARRAY). Order across files is unspecified, as in C++ itself. */
  .init_array     : { KEEP (*(SORT(.init_array.*))) KEEP (*(.init_array)) KEEP (*(SORT(.ctors.*))) KEEP (*(.ctors)) } :data
  .fini_array     : { KEEP (*(SORT(.fini_array.*))) KEEP (*(.fini_array)) KEEP (*(SORT(.dtors.*))) KEEP (*(.dtors)) } :data
  .data.rel.ro    : { *(.data.rel.ro.local* .data.rel.ro .data.rel.ro.*) } :data
  .dynamic        : { *(.dynamic) }                                      :data :dynamic
  .got            : { *(.got) }                                          :data
  .data           : { *(.data .data.* .data1) }                          :data
  .bss            : { *(.dynbss) *(.bss .bss.*) *(COMMON) . = ALIGN(4); } :data
  _end = .;

  /* Thread-local storage has no meaning here: the loader refuses a PT_TLS segment. */
  .tdata          : { *(.tdata .tdata.*) }
  .tbss           : { *(.tbss .tbss.*) *(.tcommon) }

  .comment      0 : { *(.comment) }
  .xt.lit       0 : { KEEP (*(.xt.lit .xt.lit.*)) }
  .xt.insn      0 : { KEEP (*(.xt.insn)) }
  .xt.prop      0 : { KEEP (*(.xt.prop .xt.prop.*)) }
  .xtensa.info  0 : { *(.xtensa.info) }
  /DISCARD/       : { *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) }
}
