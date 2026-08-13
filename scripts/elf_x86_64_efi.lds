/* EFI linker script for x86_64 (based on gnu-efi elf_x86_64_efi.lds) */
OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
SECTIONS
{
  . = 0;
  ImageBase = .;
  /* .hash and/or .gnu.hash MUST come first! */
  .hash : { *(.hash) }
  .gnu.hash : { *(.gnu.hash) }
  . = ALIGN(4096);
  .eh_frame : { *(.eh_frame) }
  .gcc_except_table : { *(.gcc_except_table*) }
  . = ALIGN(4096);
  .text :
  {
   _text = .;
   *(.text)
   *(.text.*)
   *(.gnu.linkonce.t.*)
   *(.plt)
   . = ALIGN(16);
  }
  _etext = .;
  _text_size = _etext - _text;
  . = ALIGN(4096);
  .reloc :
  {
   KEEP (*(.reloc))
  }

  . = ALIGN(4096);
  .data :
  {
   _data = .;
   *(.got.plt)
   *(.got)
   *(.data*)
   *(.sdata)

   /*
    * Note that these aren't the using the GNU "CONSTRUCTOR" output section
    * command, so they don't start with a size.  Because of p2align and the
    * end/END definitions, and the fact that they're mergeable, they can also
    * have NULLs which aren't guaranteed to be at the end.
    */
   . = ALIGN(16);
   __init_array_start = .;
   /* KEEP is load-bearing. Nothing REFERENCES an .init_array entry -- that is
      the point of one -- so --gc-sections collects the section and the crt0
      walker finds __init_array_start == __init_array_end and runs nothing.
      A C++ global object's constructor then silently never runs: measured as
      `magic=0x0` where the constructor sets 0xABCD, with no diagnostic
      anywhere. It also kept ios_base::Init out of the image entirely, which
      is why std::cout faulted in sentry() on a never-constructed stream. */
   KEEP(*(SORT(.init_array.*)))
   KEEP(*(.init_array))
   __init_array_end = .;
  . = ALIGN(16);
   __CTOR_LIST__ = .;
   KEEP(*(SORT(.ctors.*)))
   KEEP(*(.ctors))
   __CTOR_END__ = .;
  . = ALIGN(16);
   __DTOR_LIST__ = .;
   *(SORT(.dtors.*))
   *(.dtors)
   __DTOR_END__ = .;
   . = ALIGN(16);
   __fini_array_start = .;
   *(SORT(.fini_array.*))
   *(.fini_array)
   __fini_array_end = .;

   *(.rel.local)
  }
  .note.gnu.build-id : { *(.note.gnu.build-id) }

  _edata = .;
  _data_size = _edata - _etext;
  . = ALIGN(4096);

  /* Real uninitialized .bss. This output section holds only NOBITS inputs,
     so it stays SHT_NOBITS; objcopy carries it (via -j .bss) into the PE as
     an IMAGE_SCN_CNT_UNINITIALIZED_DATA section with SizeOfRawData == 0, so the
     loader maps the span but it carries no file content. This replaces the
     historical "fold .bss into .data" workaround, which materialized every
     zero byte as literal file content (an N-byte static array added N bytes to
     the .efi). The crt0 _start ZEROES [_bss, _bss_end) before any C code runs
     (src/crt0/axl-crt0-gcc-*.S) — the loader's zero-fill is NOT relied upon
     (fresh AllocatePages memory can be dirty; `make check-bss-clear` guards the
     crt0 clear). _bss/_bss_end are the bounds the crt0 reads. Page-aligned so
     it lands on its own PE section boundary. */
  .bss (NOLOAD) :
  {
   _bss = .;
   *(.sbss)
   *(.scommon)
   *(.dynbss)
   *(.bss*)
   *(COMMON)
   . = ALIGN(16);
   _bss_end = .;
  }
  _bss_size = _bss_end - _bss;
  . = ALIGN(4096);
  .dynamic  : { *(.dynamic) }
  . = ALIGN(4096);
  .rela :
  {
    *(.rela.text*)
    *(.rela.data*)
    *(.rela.got)
    *(.rela.dyn)
    *(.rela.stab)
    *(.rela.init_array*)
    *(.rela.fini_array*)
    *(.rela.ctors*)
    *(.rela.dtors*)
  }
  . = ALIGN(4096);
  .rela.plt : { *(.rela.plt) }
  . = ALIGN(4096);
  .rodata : { *(.rodata*) }
  . = ALIGN(4096);
  .dynsym   : { *(.dynsym) }
  . = ALIGN(4096);
  .dynstr   : { *(.dynstr) }
  . = ALIGN(4096);
  /* KEEP — pe-set-debug requires this section to be present even if
     no symbol references it. --gc-sections (recently enabled) would
     otherwise strip it as unreachable. */
  .dbgdir : { KEEP (*(.dbgdir)) }
  . = ALIGN(4096);
  .ignored.reloc :
  {
    *(.rela.reloc)
    *(.note.GNU-stack)
  }
  .comment 0 : { *(.comment) }
}
