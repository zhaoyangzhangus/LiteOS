# NASM 3.02 assembler translation units.  Generated sources are shipped in
# the upstream source archive, so the LiteOS build does not require Perl.
NASM_SOURCE_FILES := \
  stdlib/snprintf.c \
  stdlib/strlcpy.c \
  stdlib/strnlen.c \
  stdlib/vsnprintf.c \
  nasmlib/ver.c \
  nasmlib/alloc.c \
  nasmlib/asprintf.c \
  nasmlib/crc32b.c \
  nasmlib/crc64.c \
  nasmlib/md5c.c \
  nasmlib/string.c \
  nasmlib/nctype.c \
  nasmlib/file.c \
  nasmlib/fileio.c \
  nasmlib/mmap.c \
  nasmlib/realpath.c \
  nasmlib/path.c \
  nasmlib/ilog2.c \
  nasmlib/numstr.c \
  nasmlib/rlimit.c \
  nasmlib/zerobuf.c \
  nasmlib/bsi.c \
  nasmlib/rbtree.c \
  nasmlib/hashtbl.c \
  nasmlib/raa.c \
  nasmlib/saa.c \
  nasmlib/strlist.c \
  nasmlib/perfhash.c \
  nasmlib/badenum.c \
  nasmlib/readnum.c \
  common/common.c \
  common/errstubs.c \
  x86/insnsa.c \
  x86/insnsb.c \
  x86/insnsn.c \
  x86/regs.c \
  x86/regvals.c \
  x86/regflags.c \
  x86/iflag.c \
  asm/error.c \
  asm/floats.c \
  asm/directiv.c \
  asm/pragma.c \
  asm/assemble.c \
  asm/labels.c \
  asm/parser.c \
  asm/preproc.c \
  asm/quote.c \
  asm/listing.c \
  asm/eval.c \
  asm/exprlib.c \
  asm/exprdump.c \
  asm/stdscan.c \
  asm/getbool.c \
  asm/strfunc.c \
  asm/segalloc.c \
  asm/rdstrnum.c \
  asm/srcfile.c \
  asm/directbl.c \
  asm/pptok.c \
  asm/tokhash.c \
  asm/uncompress.c \
  asm/warnings.c \
  macros/macros.c \
  output/outform.c \
  output/outlib.c \
  output/nulldbg.c \
  output/nullout.c \
  output/outbin.c \
  output/outaout.c \
  output/outcoff.c \
  output/outelf.c \
  output/outobj.c \
  output/outas86.c \
  output/outdbg.c \
  output/outieee.c \
  output/outmacho.c \
  output/codeview.c \
  zlib/adler32.c \
  zlib/crc32.c \
  zlib/infback.c \
  zlib/inffast.c \
  zlib/inflate.c \
  zlib/inftrees.c \
  zlib/zutil.c \
  asm/nasm.c

NDISASM_SOURCE_FILES := \
  $(filter stdlib/% nasmlib/% common/%, $(NASM_SOURCE_FILES)) \
  $(filter x86/insnsa.c x86/insnsb.c x86/insnsn.c x86/regs.c \
         x86/regvals.c x86/regflags.c x86/iflag.c, $(NASM_SOURCE_FILES)) \
  x86/insnsd.c \
  x86/regdis.c \
  disasm/disasm.c \
  disasm/sync.c \
  disasm/prefix.c \
  disasm/diserror.c \
  disasm/ndisasm.c
