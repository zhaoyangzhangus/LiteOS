# -*- makefile -*-
#
# Makefile for building NASM using OpenWatcom
# cross-compile on a DOS/Win32/OS2 platform host
#

top_srcdir  = .
srcdir      = .
VPATH       = $(srcdir)/asm;$(srcdir)/x86;$(srcdir)/macros;$(srcdir)/output;
VPATH       +=$(srcdir)/common;$(srcdir)/stdlib;$(srcdir)/nasmlib;
VPATH       +=$(srcdir)/disasm;$(srcdir)/zlib;asm;x86;macros
prefix      = C:\Program Files\NASM
exec_prefix = $(prefix)
bindir      = $(prefix)/bin
mandir      = $(prefix)/man

CC          = *wcc386
DEBUG       =
CFLAGS      = -zq -6 -ox -wx -wcd=124 -ze -fpi $(DEBUG)
BUILD_CFLAGS = $(CFLAGS) $(%TARGET_CFLAGS)
INTERNAL_CFLAGS = -I. -Iasm -Ix86 -Iinclude -I"$(srcdir)" -I"$(srcdir)/include" &
    -I"$(srcdir)/asm" -I"$(srcdir)/x86" -I"$(srcdir)/disasm" -I"$(srcdir)/output" &
    -I"$(srcdir)/zlib"
ALL_CFLAGS  = $(BUILD_CFLAGS) $(INTERNAL_CFLAGS)
LD          = *wlink
LDEBUG      =
LDFLAGS     = op q $(%TARGET_LFLAGS) $(LDEBUG)
LIBS        =
STRIP       = wstrip

PERL        = perl
PERLFLAGS   = -I$(srcdir)/perllib -I$(srcdir)
RUNPERL     = $(PERL) $(PERLFLAGS)

!ifdef __LOADDLL__
!loaddll wcc    wccd.dll
!loaddll wcc386 wccd386.dll
!loaddll wlib   wlibd.dll
!loaddll wlink  wlinkd.dll
!endif

.BEFORE
    @set INCLUDE=
    @set COPYCMD=/y
    @for %d in ($(SUBDIRS) $(XSUBDIRS)) do @if not exist %d mkdir %d

# rm is handled internally by WMAKE, so it does work even on non-Unix systems
RM_F        = -rm -f
LN_S        = copy
EMPTY       = %create
SIDE        = %null Created by side effect
PHONY       = .SYMBOLIC

MAKENSIS    = makensis

# Binary suffixes
O           = obj
A           = lib
X           = .exe

# WMAKE errors out if a suffix is declared more than once, including
# its own built-in declarations.  Thus, we need to explicitly clear the list
# first.  Also, WMAKE only allows implicit rules that point "to the left"
# in this list!
.SUFFIXES:
.SUFFIXES: .man .1 .obj .i .c .lib .exe

# Needed to find C files anywhere but in the current directory
.c : $(VPATH)

.c.obj: .AUTODEPEND
    $(CC) $(ALL_CFLAGS) -fo=$^@ $[@

MANIFEST    =

ZLIB        = $(ZLIBOBJ)

#-- Begin File Lists --#
# Edit in Makefile.in, not here!
NASM    = asm/nasm.obj
NDISASM = disasm/ndisasm.obj

PROGOBJ = $(NASM) $(NDISASM)
PROGS   = nasm$(X) ndisasm$(X)

# Objects for the local copy of zlib. The variable ZLIB is set to
# $(ZLIBOBJ) if the internal version of zlib should be used.
ZLIBOBJ = &
	zlib/adler32.obj &
	zlib/crc32.obj &
	zlib/infback.obj &
	zlib/inffast.obj &
	zlib/inflate.obj &
	zlib/inftrees.obj &
	zlib/zutil.obj

# Common library objects
LIBOBJ_COM = &
	stdlib/snprintf.obj stdlib/vsnprintf.obj stdlib/strlcpy.obj &
	stdlib/strnlen.obj &
	&
	nasmlib/ver.obj &
	nasmlib/alloc.obj nasmlib/asprintf.obj &
	nasmlib/crc32b.obj nasmlib/crc64.obj nasmlib/md5c.obj &
	nasmlib/string.obj nasmlib/nctype.obj &
	nasmlib/file.obj nasmlib/fileio.obj nasmlib/mmap.obj &
	nasmlib/realpath.obj nasmlib/path.obj &
	nasmlib/ilog2.obj nasmlib/numstr.obj &
	nasmlib/rlimit.obj &
	nasmlib/zerobuf.obj nasmlib/bsi.obj &
	nasmlib/rbtree.obj nasmlib/hashtbl.obj &
	nasmlib/raa.obj nasmlib/saa.obj &
	nasmlib/strlist.obj &
	nasmlib/perfhash.obj nasmlib/badenum.obj &
	nasmlib/readnum.obj &
	&
	common/common.obj common/errstubs.obj &
	&
	x86/insnsa.obj x86/insnsb.obj x86/insnsn.obj &
	x86/regs.obj x86/regvals.obj x86/regflags.obj &
	x86/iflag.obj &
	&
	$(ZLIB)

# Files dependent on warnings.dat
WARNOBJ   = asm/warnings.obj
WARNFILES = asm/warnings_c.h include/warnings.h doc/warnings.src

OUTPUTOBJ = &
	output/outform.obj output/outlib.obj &
	output/nulldbg.obj output/nullout.obj &
	output/outbin.obj output/outaout.obj output/outcoff.obj &
	output/outelf.obj &
	output/outobj.obj output/outas86.obj &
	output/outdbg.obj output/outieee.obj output/outmacho.obj &
	output/codeview.obj

# Assembler-only library objects
LIBOBJ_ASM = &
	asm/error.obj &
	asm/floats.obj &
	asm/directiv.obj &
	asm/pragma.obj &
	asm/assemble.obj asm/labels.obj asm/parser.obj &
	asm/preproc.obj asm/quote.obj &
	asm/listing.obj asm/eval.obj asm/exprlib.obj asm/exprdump.obj &
	asm/stdscan.obj &
	asm/getbool.obj &
	asm/strfunc.obj &
	asm/segalloc.obj &
	asm/rdstrnum.obj &
	asm/srcfile.obj &
	asm/directbl.obj &
	asm/pptok.obj &
	asm/tokhash.obj &
	asm/uncompress.obj &
	&
	macros/macros.obj &
	&
	$(WARNOBJ) &
	$(OUTPUTOBJ)

# Objects which are only used for the disassembler
LIBOBJ_DIS = &
	disasm/disasm.obj disasm/sync.obj disasm/prefix.obj &
	disasm/diserror.obj &
	&
	x86/insnsd.obj x86/regdis.obj

LIBOBJ    = $(LIBOBJ_COM) $(LIBOBJ_ASM) $(LIBOBJ_DIS)
ALLOBJ    = $(PROGOBJ) $(LIBOBJ)
SUBDIRS  = stdlib nasmlib include config output asm disasm x86 &
	   common zlib macros misc
XSUBDIRS = nsis win test doc editors
DEPDIRS  = . $(SUBDIRS)

EDITORS  = editors/nasmtok.el editors/nasmtok.json

#-- End File Lists --#

what:   $(PHONY)
    @echo Please build "dos", "win32", "os2" or "linux386"

dos:    $(PHONY)
    @set TARGET_CFLAGS=-bt=DOS -I"$(%WATCOM)/h"
    @set TARGET_LFLAGS=sys causeway
    @%make all

win32:  $(PHONY)
    @set TARGET_CFLAGS=-bt=NT -I"$(%WATCOM)/h" -I"$(%WATCOM)/h/nt"
    @set TARGET_LFLAGS=sys nt
    @%make all

os2:    $(PHONY)
    @set TARGET_CFLAGS=-bt=OS2 -I"$(%WATCOM)/h" -I"$(%WATCOM)/h/os2"
    @set TARGET_LFLAGS=sys os2v2
    @%make all

linux386:   $(PHONY)
    @set TARGET_CFLAGS=-bt=LINUX -I"$(%WATCOM)/lh"
    @set TARGET_LFLAGS=sys linux
    @%make all

all: perlreq nasm$(X) ndisasm$(X) $(PHONY)
#   cd rdoff && $(MAKE) all

NASMLIB = nasm.lib
ASMLIB  = asm.lib
DISLIB  = dis.lib

nasm$(X): $(NASM) $(ASMLIB) $(NASMLIB)
    $(LD) $(LDFLAGS) name nasm$(X) libr {$(ASMLIB) $(NASMLIB) $(LIBS)} file {$(NASM)}

ndisasm$(X): $(NDISASM) $(DISLIB) $(NASMLIB)
    $(LD) $(LDFLAGS) name ndisasm$(X) libr {$(DISLIB) $(NASMLIB) $(LIBS)} file {$(NDISASM)}

<<<<<<< HEAD
nasm.lib: $(LIBOBJ_COM)
    wlib -q -b -n $@ $(LIBOBJ_COM)

asm.lib: $(LIBOBJ_ASM)
    wlib -q -b -n $@ $(LIBOBJ_ASM)

dis.lib: $(LIBOBJ_DIS)
    wlib -q -b -n $@ $(LIBOBJ_DIS)

# These are specific to certain Makefile syntaxes (what are they
# actually supposed to look like for wmake?)
WARNTIMES = $(WARNFILES:=.time)
WARNSRCS  = $(LIBOBJ_NW:.obj=.c)
=======
nasm.lib: $(LIBOBJ)
    *wlib -q -b -n $@ $<

ndisasm.lib: $(LIBOBJ_DIS)
    *wlib -q -b -n $@ $<
>>>>>>> 61e7e23c94c8 (open-watcom: fix Open Watcom build make file)

#-- Begin Generated File Rules --#
# Edit in Makefile.in, not here!

# These source files are automagically generated from data files using
# Perl scripts. They're distributed, though, so it isn't necessary to
# have Perl just to recompile NASM from the distribution.

# Perl-generated source files
PERLREQ_CLEANABLE = &
	  x86/insnsb.c x86/insnsa.c x86/insnsd.c x86/insnsi.h x86/insnsn.c &
	  x86/regs.c x86/regs.h x86/regflags.c x86/regdis.c x86/regdis.h &
	  x86/regvals.c asm/tokhash.c asm/tokens.h asm/pptok.h asm/pptok.c &
	  x86/iflag.c x86/iflaggen.h &
	  macros/macros.c &
	  asm/pptok.ph asm/directbl.c asm/directiv.h &
	  $(WARNFILES) &
	  version.h version.mac version.mak nsis/version.nsh

PERLREQ = $(PERLREQ_CLEANABLE)

INSDEP = x86/insns.xda x86/insns.pl x86/insns-iflags.ph x86/iflags.ph

x86/insns.xda: x86/insns.dat x86/preinsns.pl $(DIRS)
	$(RUNPERL) $(srcdir)/x86/preinsns.pl $(srcdir)/x86/insns.dat $@

x86/iflag.c: $(INSDEP)
	$(RUNPERL) $(srcdir)/x86/insns.pl -fc &
		x86/insns.xda x86/iflag.c
x86/iflaggen.h: $(INSDEP)
	$(RUNPERL) $(srcdir)/x86/insns.pl -fh &
		x86/insns.xda x86/iflaggen.h
x86/insnsb.c: $(INSDEP)
	$(RUNPERL) $(srcdir)/x86/insns.pl -b &
		x86/insns.xda x86/insnsb.c
x86/insnsa.c: $(INSDEP)
	$(RUNPERL) $(srcdir)/x86/insns.pl -a &
		x86/insns.xda x86/insnsa.c
x86/insnsd.c: $(INSDEP)
	$(RUNPERL) $(srcdir)/x86/insns.pl -d &
		x86/insns.xda x86/insnsd.c
x86/insnsi.h: $(INSDEP)
	$(RUNPERL) $(srcdir)/x86/insns.pl -i &
		x86/insns.xda x86/insnsi.h
x86/insnsn.c: $(INSDEP)
	$(RUNPERL) $(srcdir)/x86/insns.pl -n &
		x86/insns.xda x86/insnsn.c

# These files contains all the standard macros that are derived from
# the version number.
version.h: version version.pl
	$(RUNPERL) $(srcdir)/version.pl h < $(srcdir)/version > version.h
version.mac: version version.pl
	$(RUNPERL) $(srcdir)/version.pl mac < $(srcdir)/version > version.mac
version.sed: version version.pl
	$(RUNPERL) $(srcdir)/version.pl sed < $(srcdir)/version > version.sed
version.mak: version version.pl
	$(RUNPERL) $(srcdir)/version.pl make < $(srcdir)/version > version.mak
nsis/version.nsh: version version.pl $(DIRS)
	$(RUNPERL) $(srcdir)/version.pl nsis < $(srcdir)/version > nsis/version.nsh

# This source file is generated from the standard macros file
# `standard.mac' by another Perl script. Again, it's part of the
# standard distribution.
macros/macros.c: macros/macros.pl asm/pptok.ph version.mac &
	$(srcdir)/macros/*.mac $(srcdir)/output/*.mac
	$(RUNPERL) $(srcdir)/macros/macros.pl version.mac &
		$(srcdir)/macros/*.mac $(srcdir)/output/*.mac

# These source files are generated from regs.dat by yet another
# perl script.
x86/regs.c: x86/regs.dat x86/regs.pl
	$(RUNPERL) $(srcdir)/x86/regs.pl c &
		$(srcdir)/x86/regs.dat > x86/regs.c
x86/regflags.c: x86/regs.dat x86/regs.pl
	$(RUNPERL) $(srcdir)/x86/regs.pl fc &
		$(srcdir)/x86/regs.dat > x86/regflags.c
x86/regdis.c: x86/regs.dat x86/regs.pl
	$(RUNPERL) $(srcdir)/x86/regs.pl dc &
		$(srcdir)/x86/regs.dat > x86/regdis.c
x86/regdis.h: x86/regs.dat x86/regs.pl
	$(RUNPERL) $(srcdir)/x86/regs.pl dh &
		$(srcdir)/x86/regs.dat > x86/regdis.h
x86/regvals.c: x86/regs.dat x86/regs.pl
	$(RUNPERL) $(srcdir)/x86/regs.pl vc &
		$(srcdir)/x86/regs.dat > x86/regvals.c
x86/regs.h: x86/regs.dat x86/regs.pl
	$(RUNPERL) $(srcdir)/x86/regs.pl h &
		$(srcdir)/x86/regs.dat > x86/regs.h


# Assembler token hash
asm/tokhash.c: x86/insns.xda x86/insnsn.c asm/tokens.dat asm/tokhash.pl &
	perllib/phash.ph
	$(RUNPERL) $(srcdir)/asm/tokhash.pl c &
		x86/insnsn.c $(srcdir)/x86/regs.dat &
		$(srcdir)/asm/tokens.dat > asm/tokhash.c

# Assembler token metadata
asm/tokens.h: x86/insns.xda x86/insnsn.c asm/tokens.dat asm/tokhash.pl &
	perllib/phash.ph
	$(RUNPERL) $(srcdir)/asm/tokhash.pl h &
		x86/insnsn.c $(srcdir)/x86/regs.dat &
		$(srcdir)/asm/tokens.dat > asm/tokens.h

# Preprocessor token hash
asm/pptok.h: asm/pptok.dat asm/pptok.pl perllib/phash.ph
	$(RUNPERL) $(srcdir)/asm/pptok.pl h &
		$(srcdir)/asm/pptok.dat asm/pptok.h
asm/pptok.c: asm/pptok.dat asm/pptok.pl perllib/phash.ph
	$(RUNPERL) $(srcdir)/asm/pptok.pl c &
		$(srcdir)/asm/pptok.dat asm/pptok.c
asm/pptok.ph: asm/pptok.dat asm/pptok.pl perllib/phash.ph
	$(RUNPERL) $(srcdir)/asm/pptok.pl ph &
		$(srcdir)/asm/pptok.dat asm/pptok.ph
doc/pptok.src: asm/pptok.dat asm/pptok.pl perllib/phash.ph
	$(RUNPERL) $(srcdir)/asm/pptok.pl src &
		$(srcdir)/asm/pptok.dat doc/pptok.src

# Directives hash
asm/directiv.h: asm/directiv.dat nasmlib/perfhash.pl perllib/phash.ph
	$(RUNPERL) $(srcdir)/nasmlib/perfhash.pl h &
		$(srcdir)/asm/directiv.dat asm/directiv.h
asm/directbl.c: asm/directiv.dat nasmlib/perfhash.pl perllib/phash.ph
	$(RUNPERL) $(srcdir)/nasmlib/perfhash.pl c &
		$(srcdir)/asm/directiv.dat asm/directbl.c

# Editor token files
editors/nasmtok.el: editors/nasmtok.pl asm/tokhash.c asm/pptok.c &
		 asm/directiv.dat macros/macros.c editors/builtin.mac &
		 version.mak
	$(RUNPERL) $(srcdir)/editors/nasmtok.pl -el $@ $(srcdir) $(objdir)

editors/nasmtok.json: editors/nasmtok.pl asm/tokhash.c asm/pptok.c &
		 asm/directiv.dat macros/macros.c editors/builtin.mac &
		 version.mak
	$(RUNPERL) $(srcdir)/editors/nasmtok.pl -json $@ $(srcdir) $(objdir)

editors: $(EDITORS) $(PHONY)

asm/warnings_c.h: asm/warnings.pl asm/warnings.dat
	$(RUNPERL) $(srcdir)/asm/warnings.pl c asm/warnings_c.h &
		$(srcdir)/asm/warnings.dat

include/warnings.h: asm/warnings.pl asm/warnings.dat
	$(RUNPERL) $(srcdir)/asm/warnings.pl h include/warnings.h &
		$(srcdir)/asm/warnings.dat

doc/warnings.src: asm/warnings.pl asm/warnings.dat
	$(RUNPERL) $(srcdir)/asm/warnings.pl doc doc/warnings.src &
		$(srcdir)/asm/warnings.dat

$(PERLREQ): $(DIRS)

perlreq: $(PERLREQ) $(PHONY)

warnings: $(WARNFILES) $(PHONY)

#-- End Generated File Rules --#

#-- Begin NSIS Rules --#
# Edit in Makefile.in, not here!

nsis/arch.nsh: nsis/getpearch.pl nasm$(X) $(DIRS)
	$(PERL) $(srcdir)/nsis/getpearch.pl nasm$(X) > nsis/arch.nsh

# Should only be done after "make everything".
# The use of redirection here keeps makensis from moving the cwd to the
# source directory.
nsis: nsis/nasm.nsi nsis/arch.nsh nsis/version.nsh
	$(MAKENSIS) -Dsrcdir=$(srcdir) -Dobjdir=$(objdir) - &
		< $(srcdir)/nsis/nasm.nsi

#-- End NSIS Rules --#

clean: $(PHONY)
    rm -f *.obj *.s *.i
    rm -f asm/*.obj asm/*.s asm/*.i
    rm -f x86/*.obj x86/*.s x86/*.i
    rm -f lib/*.obj lib/*.s lib/*.i
    rm -f macros/*.obj macros/*.s macros/*.i
    rm -f output/*.obj output/*.s output/*.i
    rm -f common/*.obj common/*.s common/*.i
    rm -f stdlib/*.obj stdlib/*.s stdlib/*.i
    rm -f nasmlib/*.obj nasmlib/*.s nasmlib/*.i
    rm -f disasm/*.obj disasm/*.s disasm/*.i
    rm -f zlib/*.obj zlib/*.s zlib/*.i
    rm -f config.h config.log config.status
    rm -f nasm$(X) ndisasm$(X) $(NASMLIB) $(NDISLIB)

distclean: clean $(PHONY)
    rm -f config.h config.log config.status
    rm -f Makefile *~ *.bak *.lst *.bin
    rm -f output/*~ output/*.bak
    rm -f test/*.lst test/*.bin test/*.obj test/*.bin

cleaner: clean $(PHONY)
    rm -f $(PERLREQ)
    rm -f *.man
    rm -f nasm.spec
#   cd doc && $(MAKE) clean

spotless: distclean cleaner $(PHONY)
    rm -f doc/Makefile doc/*~ doc/*.bak

strip: $(PHONY)
    $(STRIP) *.exe

doc:
#    cd doc && $(MAKE) all

everything: all doc

#
# This build dependencies in *ALL* makefiles.  Partially for that reason,
# it's expected to be invoked manually.
#
alldeps: perlreq $(PHONY)
    $(PERL) syncfiles.pl Makefile.in Mkfiles/openwcom.mak
    $(PERL) mkdep.pl -M Makefile.in Mkfiles/openwcom.mak -- . output lib

#-- Magic hints to mkdep.pl --#
# @object-ending: ".obj"
# @path-separator: "/"
# @exclude: "config/config.h"
# @continuation: "&"
#-- Everything below is generated by mkdep.pl - do not edit --#
asm/assemble.obj: asm/asmutil.h asm/assemble.c asm/assemble.h asm/directiv.h &
 asm/listing.h asm/pptok.h asm/preproc.h asm/srcfile.h asm/tokens.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 include/bytesex.h include/compiler.h include/dbginfo.h include/disp8.h &
 include/error.h include/hashtbl.h include/iflag.h include/ilog2.h &
 include/insns.h include/labels.h include/macros.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/rbtree.h include/strlist.h include/tables.h &
 include/warnings.h x86/iflaggen.h x86/insnsi.h x86/regs.h x86/x86const.h
asm/directbl.obj: asm/directbl.c asm/directiv.h config/msvc.h &
 config/unconfig.h config/unknown.h config/watcom.h include/bytesex.h &
 include/compiler.h include/nasmint.h include/nasmlib.h include/perfhash.h
asm/directiv.obj: asm/asmutil.h asm/assemble.h asm/directiv.c asm/directiv.h &
 asm/eval.h asm/floats.h asm/listing.h asm/pptok.h asm/preproc.h asm/quote.h &
 asm/srcfile.h asm/stdscan.h config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/error.h include/hashtbl.h include/iflag.h include/ilog2.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/strlist.h include/tables.h include/warnings.h output/outform.h &
 x86/iflaggen.h x86/insnsi.h x86/regs.h x86/x86const.h
asm/error.obj: asm/directiv.h asm/error.c asm/listing.h asm/pptok.h &
 asm/preproc.h asm/srcfile.h config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/error.h include/hashtbl.h include/labels.h include/macros.h &
 include/nasm.h include/nasmint.h include/nasmlib.h include/nctype.h &
 include/opflags.h include/perfhash.h include/strlist.h include/tables.h &
 include/warnings.h x86/insnsi.h x86/regs.h x86/x86const.h
asm/eval.obj: asm/asmutil.h asm/assemble.h asm/directiv.h asm/eval.c &
 asm/eval.h asm/floats.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 include/bytesex.h include/compiler.h include/error.h include/hashtbl.h &
 include/iflag.h include/ilog2.h include/labels.h include/macros.h &
 include/nasm.h include/nasmint.h include/nasmlib.h include/nctype.h &
 include/opflags.h include/perfhash.h include/strlist.h include/tables.h &
 include/warnings.h x86/iflaggen.h x86/insnsi.h x86/regs.h x86/x86const.h
asm/exprdump.obj: asm/directiv.h asm/exprdump.c asm/pptok.h asm/preproc.h &
 asm/srcfile.h config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/hashtbl.h include/labels.h include/macros.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/strlist.h include/tables.h include/warnings.h &
 x86/insnsi.h x86/regs.h x86/x86const.h
asm/exprlib.obj: asm/directiv.h asm/exprlib.c asm/pptok.h asm/preproc.h &
 asm/srcfile.h config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/hashtbl.h include/labels.h include/macros.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/strlist.h include/tables.h include/warnings.h &
 x86/insnsi.h x86/regs.h x86/x86const.h
asm/floats.obj: asm/directiv.h asm/floats.c asm/floats.h asm/pptok.h &
 asm/preproc.h asm/srcfile.h config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/error.h include/hashtbl.h include/labels.h include/macros.h &
 include/nasm.h include/nasmint.h include/nasmlib.h include/nctype.h &
 include/opflags.h include/perfhash.h include/strlist.h include/tables.h &
 include/warnings.h x86/insnsi.h x86/regs.h x86/x86const.h
asm/getbool.obj: asm/asmutil.h asm/directiv.h asm/eval.h asm/getbool.c &
 asm/pptok.h asm/preproc.h asm/srcfile.h asm/stdscan.h config/msvc.h &
 config/unconfig.h config/unknown.h config/watcom.h include/bytesex.h &
 include/compiler.h include/error.h include/hashtbl.h include/labels.h &
 include/macros.h include/nasm.h include/nasmint.h include/nasmlib.h &
 include/nctype.h include/opflags.h include/perfhash.h include/strlist.h &
 include/tables.h include/warnings.h x86/insnsi.h x86/regs.h x86/x86const.h
asm/labels.obj: asm/directiv.h asm/labels.c asm/pptok.h asm/preproc.h &
 asm/srcfile.h config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/hashtbl.h include/labels.h include/macros.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/strlist.h include/tables.h include/warnings.h &
 x86/insnsi.h x86/regs.h x86/x86const.h
asm/listing.obj: asm/directiv.h asm/listing.c asm/listing.h asm/pptok.h &
 asm/preproc.h asm/srcfile.h config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/error.h include/hashtbl.h include/labels.h include/macros.h &
 include/nasm.h include/nasmint.h include/nasmlib.h include/nctype.h &
 include/opflags.h include/perfhash.h include/strlist.h include/tables.h &
 include/warnings.h x86/insnsi.h x86/regs.h x86/x86const.h
asm/nasm.obj: asm/asmutil.h asm/assemble.h asm/directiv.h asm/eval.h &
 asm/floats.h asm/listing.h asm/nasm.c asm/parser.h asm/pptok.h &
 asm/preproc.h asm/quote.h asm/srcfile.h asm/stdscan.h asm/tokens.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 include/bytesex.h include/compiler.h include/error.h include/hashtbl.h &
 include/iflag.h include/ilog2.h include/insns.h include/labels.h &
 include/macros.h include/nasm.h include/nasmint.h include/nasmlib.h &
 include/nctype.h include/opflags.h include/perfhash.h include/raa.h &
 include/saa.h include/strlist.h include/tables.h include/ver.h &
 include/warnings.h output/outform.h x86/iflaggen.h x86/insnsi.h x86/regs.h &
 x86/x86const.h
asm/parser.obj: asm/asmutil.h asm/assemble.h asm/directiv.h asm/eval.h &
 asm/floats.h asm/parser.c asm/parser.h asm/pptok.h asm/preproc.h &
 asm/srcfile.h asm/stdscan.h asm/tokens.h config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/error.h include/hashtbl.h include/iflag.h include/ilog2.h &
 include/insns.h include/labels.h include/macros.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/strlist.h include/tables.h include/warnings.h &
 x86/iflaggen.h x86/insnsi.h x86/regs.h x86/x86const.h
asm/pptok.obj: asm/pptok.c asm/pptok.h asm/preproc.h config/msvc.h &
 config/unconfig.h config/unknown.h config/watcom.h include/bytesex.h &
 include/compiler.h include/hashtbl.h include/nasmint.h include/nasmlib.h &
 include/nctype.h
asm/pragma.obj: asm/asmutil.h asm/assemble.h asm/directiv.h asm/listing.h &
 asm/pptok.h asm/pragma.c asm/preproc.h asm/srcfile.h config/msvc.h &
 config/unconfig.h config/unknown.h config/watcom.h include/bytesex.h &
 include/compiler.h include/error.h include/hashtbl.h include/iflag.h &
 include/ilog2.h include/labels.h include/macros.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/strlist.h include/tables.h include/warnings.h &
 x86/iflaggen.h x86/insnsi.h x86/regs.h x86/x86const.h
asm/preproc.obj: asm/asmutil.h asm/assemble.h asm/directiv.h asm/eval.h &
 asm/listing.h asm/pptok.h asm/preproc.c asm/preproc.h asm/quote.h &
 asm/srcfile.h asm/stdscan.h asm/tokens.h config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/dbginfo.h include/error.h include/hashtbl.h include/iflag.h &
 include/ilog2.h include/labels.h include/macros.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/rbtree.h include/strlist.h include/tables.h &
 include/warnings.h x86/iflaggen.h x86/insnsi.h x86/regs.h x86/x86const.h
asm/quote.obj: asm/quote.c asm/quote.h config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/error.h include/nasmint.h include/nasmlib.h include/nctype.h &
 include/warnings.h
asm/rdstrnum.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/rdstrnum.c &
 asm/srcfile.h config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/hashtbl.h include/labels.h include/macros.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/strlist.h include/tables.h include/warnings.h &
 x86/insnsi.h x86/regs.h x86/x86const.h
asm/segalloc.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/segalloc.c &
 asm/srcfile.h asm/tokens.h config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/hashtbl.h include/iflag.h include/ilog2.h include/insns.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/strlist.h include/tables.h include/warnings.h x86/iflaggen.h &
 x86/insnsi.h x86/regs.h x86/x86const.h
asm/srcfile.obj: asm/srcfile.c asm/srcfile.h config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/hashtbl.h include/nasmint.h include/nasmlib.h
asm/stdscan.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/quote.h &
 asm/srcfile.h asm/stdscan.c asm/stdscan.h asm/tokens.h config/msvc.h &
 config/unconfig.h config/unknown.h config/watcom.h include/bytesex.h &
 include/compiler.h include/error.h include/hashtbl.h include/iflag.h &
 include/ilog2.h include/insns.h include/labels.h include/macros.h &
 include/nasm.h include/nasmint.h include/nasmlib.h include/nctype.h &
 include/opflags.h include/perfhash.h include/strlist.h include/tables.h &
 include/warnings.h x86/iflaggen.h x86/insnsi.h x86/regs.h x86/x86const.h
asm/strfunc.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 asm/strfunc.c config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/hashtbl.h include/labels.h include/macros.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/strlist.h include/tables.h include/warnings.h &
 x86/insnsi.h x86/regs.h x86/x86const.h
asm/tokhash.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 asm/stdscan.h asm/tokens.h asm/tokhash.c config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/error.h include/hashtbl.h include/iflag.h include/ilog2.h &
 include/insns.h include/labels.h include/macros.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/strlist.h include/tables.h include/warnings.h &
 x86/iflaggen.h x86/insnsi.h x86/regs.h x86/x86const.h
asm/uncompress.obj: asm/uncompress.c config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/error.h include/macros.h include/nasmint.h include/nasmlib.h &
 include/warnings.h zlib/zconf.h zlib/zlib.h
asm/warnings.obj: asm/warnings.c asm/warnings_c.h config/msvc.h &
 config/unconfig.h config/unknown.h config/watcom.h include/compiler.h &
 include/error.h include/nasmint.h include/warnings.h
common/common.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 asm/tokens.h common/common.c config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/error.h include/hashtbl.h include/iflag.h include/ilog2.h &
 include/insns.h include/labels.h include/macros.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/strlist.h include/tables.h include/warnings.h &
 x86/iflaggen.h x86/insnsi.h x86/regs.h x86/x86const.h
common/errstubs.obj: common/errstubs.c config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/error.h include/nasmint.h include/nasmlib.h include/warnings.h
disasm/disasm.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 asm/tokens.h config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h disasm/disasm.c disasm/disasm.h disasm/sync.h &
 include/bytesex.h include/compiler.h include/disp8.h include/error.h &
 include/hashtbl.h include/iflag.h include/ilog2.h include/insns.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/strlist.h include/tables.h include/warnings.h x86/iflaggen.h &
 x86/insnsi.h x86/regdis.h x86/regs.h x86/x86const.h
disasm/diserror.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 disasm/disasm.h disasm/diserror.c include/bytesex.h include/compiler.h &
 include/error.h include/hashtbl.h include/iflag.h include/ilog2.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/strlist.h include/tables.h include/warnings.h x86/iflaggen.h &
 x86/insnsi.h x86/regs.h x86/x86const.h
disasm/ndisasm.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 asm/tokens.h config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h disasm/disasm.h disasm/ndisasm.c disasm/sync.h &
 include/bytesex.h include/compiler.h include/error.h include/hashtbl.h &
 include/iflag.h include/ilog2.h include/insns.h include/labels.h &
 include/macros.h include/nasm.h include/nasmint.h include/nasmlib.h &
 include/nctype.h include/opflags.h include/perfhash.h include/strlist.h &
 include/tables.h include/ver.h include/warnings.h x86/iflaggen.h &
 x86/insnsi.h x86/regs.h x86/x86const.h
disasm/prefix.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 disasm/disasm.h disasm/prefix.c include/bytesex.h include/compiler.h &
 include/error.h include/hashtbl.h include/iflag.h include/ilog2.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/strlist.h include/tables.h include/warnings.h x86/iflaggen.h &
 x86/insnsi.h x86/regs.h x86/x86const.h
disasm/sync.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h disasm/sync.c disasm/sync.h include/bytesex.h &
 include/compiler.h include/nasmint.h include/nasmlib.h
macros/macros.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 include/bytesex.h include/compiler.h include/error.h include/hashtbl.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/strlist.h include/tables.h include/warnings.h macros/macros.c &
 output/outform.h x86/insnsi.h x86/regs.h x86/x86const.h
misc/crcgen.obj: misc/crcgen.c
misc/omfdump.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/nasmint.h &
 misc/omfdump.c
misc/xcrcgen.obj: misc/xcrcgen.c
nasmlib/alloc.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/nasmint.h include/nasmlib.h include/warnings.h nasmlib/alloc.c &
 nasmlib/alloc.h
nasmlib/asprintf.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/nasmint.h &
 include/nasmlib.h nasmlib/alloc.h nasmlib/asprintf.c
nasmlib/badenum.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/nasmint.h &
 include/nasmlib.h nasmlib/badenum.c
nasmlib/bsi.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/nasmint.h &
 include/nasmlib.h nasmlib/bsi.c
nasmlib/crc32b.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/hashtbl.h &
 include/nasmint.h include/nasmlib.h nasmlib/crc32b.c
nasmlib/crc64.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/hashtbl.h &
 include/nasmint.h include/nasmlib.h include/nctype.h nasmlib/crc64.c
nasmlib/file.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/nasmint.h include/nasmlib.h include/warnings.h nasmlib/file.c
nasmlib/fileio.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/nasmint.h include/nasmlib.h include/warnings.h nasmlib/fileio.c
nasmlib/hashtbl.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 include/bytesex.h include/compiler.h include/error.h include/hashtbl.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/strlist.h include/tables.h include/warnings.h nasmlib/hashtbl.c &
 x86/insnsi.h x86/regs.h x86/x86const.h
nasmlib/ilog2.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/ilog2.h include/nasmint.h &
 nasmlib/ilog2.c
nasmlib/md5c.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/md5.h include/nasmint.h &
 nasmlib/md5c.c
nasmlib/mmap.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/nasmint.h &
 include/nasmlib.h nasmlib/mmap.c
nasmlib/nctype.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/nasmint.h include/nctype.h &
 nasmlib/nctype.c
nasmlib/numstr.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/nasmint.h &
 include/nasmlib.h nasmlib/numstr.c
nasmlib/path.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/nasmint.h include/nasmlib.h include/warnings.h nasmlib/path.c
nasmlib/perfhash.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/hashtbl.h &
 include/nasmint.h include/nasmlib.h include/perfhash.h nasmlib/perfhash.c
nasmlib/raa.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/ilog2.h &
 include/nasmint.h include/nasmlib.h include/raa.h nasmlib/raa.c
nasmlib/rbtree.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/nasmint.h &
 include/nasmlib.h include/rbtree.h nasmlib/rbtree.c
nasmlib/readnum.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 include/bytesex.h include/compiler.h include/error.h include/hashtbl.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/strlist.h include/tables.h include/warnings.h nasmlib/readnum.c &
 x86/insnsi.h x86/regs.h x86/x86const.h
nasmlib/realpath.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/nasmint.h &
 include/nasmlib.h nasmlib/realpath.c
nasmlib/rlimit.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/nasmint.h &
 include/nasmlib.h nasmlib/rlimit.c
nasmlib/saa.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/ilog2.h &
 include/nasmint.h include/nasmlib.h include/saa.h nasmlib/saa.c
nasmlib/string.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/nasmint.h &
 include/nasmlib.h include/nctype.h nasmlib/string.c
nasmlib/strlist.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/hashtbl.h &
 include/nasmint.h include/nasmlib.h include/strlist.h nasmlib/strlist.c
nasmlib/ver.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/nasmint.h include/ver.h &
 nasmlib/ver.c version.h
nasmlib/zerobuf.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/nasmint.h &
 include/nasmlib.h nasmlib/zerobuf.c
output/codeview.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 include/bytesex.h include/compiler.h include/error.h include/hashtbl.h &
 include/labels.h include/macros.h include/md5.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/rbtree.h include/saa.h include/strlist.h &
 include/tables.h include/warnings.h output/codeview.c output/outlib.h &
 output/pecoff.h version.h x86/insnsi.h x86/regs.h x86/x86const.h
output/nulldbg.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 include/bytesex.h include/compiler.h include/error.h include/hashtbl.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/rbtree.h include/saa.h include/strlist.h include/tables.h &
 include/warnings.h output/nulldbg.c output/outlib.h x86/insnsi.h x86/regs.h &
 x86/x86const.h
output/nullout.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 include/bytesex.h include/compiler.h include/error.h include/hashtbl.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/rbtree.h include/saa.h include/strlist.h include/tables.h &
 include/warnings.h output/nullout.c output/outlib.h x86/insnsi.h x86/regs.h &
 x86/x86const.h
output/outaout.obj: asm/directiv.h asm/eval.h asm/pptok.h asm/preproc.h &
 asm/srcfile.h asm/stdscan.h config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/error.h include/hashtbl.h include/labels.h include/macros.h &
 include/nasm.h include/nasmint.h include/nasmlib.h include/nctype.h &
 include/opflags.h include/perfhash.h include/raa.h include/rbtree.h &
 include/saa.h include/strlist.h include/tables.h include/warnings.h &
 output/outaout.c output/outform.h output/outlib.h x86/insnsi.h x86/regs.h &
 x86/x86const.h
output/outas86.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 include/bytesex.h include/compiler.h include/error.h include/hashtbl.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/raa.h include/rbtree.h include/saa.h include/strlist.h &
 include/tables.h include/warnings.h output/outas86.c output/outform.h &
 output/outlib.h x86/insnsi.h x86/regs.h x86/x86const.h
output/outbin.obj: asm/directiv.h asm/eval.h asm/pptok.h asm/preproc.h &
 asm/srcfile.h asm/stdscan.h config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/error.h include/hashtbl.h include/labels.h include/macros.h &
 include/nasm.h include/nasmint.h include/nasmlib.h include/nctype.h &
 include/opflags.h include/perfhash.h include/rbtree.h include/saa.h &
 include/strlist.h include/tables.h include/warnings.h output/outbin.c &
 output/outform.h output/outlib.h x86/insnsi.h x86/regs.h x86/x86const.h
output/outcoff.obj: asm/directiv.h asm/eval.h asm/pptok.h asm/preproc.h &
 asm/srcfile.h config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/hashtbl.h include/ilog2.h include/labels.h include/macros.h &
 include/nasm.h include/nasmint.h include/nasmlib.h include/nctype.h &
 include/opflags.h include/perfhash.h include/raa.h include/rbtree.h &
 include/saa.h include/strlist.h include/tables.h include/ver.h &
 include/warnings.h output/outcoff.c output/outform.h output/outlib.h &
 output/pecoff.h x86/insnsi.h x86/regs.h x86/x86const.h
output/outdbg.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 asm/tokens.h config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/dbginfo.h &
 include/error.h include/hashtbl.h include/iflag.h include/ilog2.h &
 include/insns.h include/labels.h include/macros.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/rbtree.h include/saa.h include/strlist.h &
 include/tables.h include/warnings.h output/outdbg.c output/outform.h &
 output/outlib.h x86/iflaggen.h x86/insnsi.h x86/regs.h x86/x86const.h
output/outelf.obj: asm/directiv.h asm/eval.h asm/pptok.h asm/preproc.h &
 asm/srcfile.h asm/stdscan.h config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/error.h include/hashtbl.h include/labels.h include/macros.h &
 include/nasm.h include/nasmint.h include/nasmlib.h include/nctype.h &
 include/opflags.h include/perfhash.h include/raa.h include/rbtree.h &
 include/saa.h include/strlist.h include/tables.h include/ver.h &
 include/warnings.h output/dwarf.h output/elf.h output/outelf.c &
 output/outelf.h output/outform.h output/outlib.h output/stabs.h &
 x86/insnsi.h x86/regs.h x86/x86const.h
output/outform.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 include/bytesex.h include/compiler.h include/error.h include/hashtbl.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/rbtree.h include/saa.h include/strlist.h include/tables.h &
 include/warnings.h output/outform.c output/outform.h output/outlib.h &
 x86/insnsi.h x86/regs.h x86/x86const.h
output/outieee.obj: asm/asmutil.h asm/directiv.h asm/pptok.h asm/preproc.h &
 asm/srcfile.h config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/hashtbl.h include/labels.h include/macros.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/rbtree.h include/saa.h include/strlist.h &
 include/tables.h include/ver.h include/warnings.h output/outform.h &
 output/outieee.c output/outlib.h x86/insnsi.h x86/regs.h x86/x86const.h
output/outlib.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 include/bytesex.h include/compiler.h include/error.h include/hashtbl.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/raa.h include/rbtree.h include/saa.h include/strlist.h &
 include/tables.h include/warnings.h output/outlib.c output/outlib.h &
 x86/insnsi.h x86/regs.h x86/x86const.h
output/outmacho.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 include/bytesex.h include/compiler.h include/error.h include/hashtbl.h &
 include/ilog2.h include/labels.h include/macros.h include/nasm.h &
 include/nasmint.h include/nasmlib.h include/nctype.h include/opflags.h &
 include/perfhash.h include/raa.h include/rbtree.h include/saa.h &
 include/strlist.h include/tables.h include/ver.h include/warnings.h &
 output/dwarf.h output/macho.h output/outform.h output/outlib.h &
 output/outmacho.c x86/insnsi.h x86/regs.h x86/x86const.h
output/outobj.obj: asm/asmutil.h asm/directiv.h asm/eval.h asm/pptok.h &
 asm/preproc.h asm/srcfile.h asm/stdscan.h config/msvc.h config/unconfig.h &
 config/unknown.h config/watcom.h include/bytesex.h include/compiler.h &
 include/error.h include/hashtbl.h include/labels.h include/macros.h &
 include/nasm.h include/nasmint.h include/nasmlib.h include/nctype.h &
 include/opflags.h include/perfhash.h include/rbtree.h include/saa.h &
 include/strlist.h include/tables.h include/ver.h include/warnings.h &
 output/outform.h output/outlib.h output/outobj.c x86/insnsi.h x86/regs.h &
 x86/x86const.h
stdlib/snprintf.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/nasmint.h &
 include/nasmlib.h stdlib/snprintf.c
stdlib/strlcpy.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/nasmint.h stdlib/strlcpy.c
stdlib/strnlen.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/nasmint.h stdlib/strnlen.c
stdlib/vsnprintf.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/nasmint.h include/nasmlib.h include/warnings.h stdlib/vsnprintf.c
x86/iflag.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/iflag.h include/ilog2.h &
 include/nasmint.h x86/iflag.c x86/iflaggen.h
x86/insnsa.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 asm/tokens.h config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/hashtbl.h include/iflag.h include/ilog2.h include/insns.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/strlist.h include/tables.h include/warnings.h x86/iflaggen.h &
 x86/insnsa.c x86/insnsi.h x86/regs.h x86/x86const.h
x86/insnsb.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 asm/tokens.h config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/hashtbl.h include/iflag.h include/ilog2.h include/insns.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/strlist.h include/tables.h include/warnings.h x86/iflaggen.h &
 x86/insnsb.c x86/insnsi.h x86/regs.h x86/x86const.h
x86/insnsd.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 asm/tokens.h config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/bytesex.h include/compiler.h include/error.h &
 include/hashtbl.h include/iflag.h include/ilog2.h include/insns.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/strlist.h include/tables.h include/warnings.h x86/iflaggen.h &
 x86/insnsd.c x86/insnsi.h x86/regs.h x86/x86const.h
x86/insnsn.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/nasmint.h include/tables.h &
 x86/insnsi.h x86/insnsn.c
x86/regdis.obj: x86/regdis.c x86/regdis.h x86/regs.h
x86/regflags.obj: asm/directiv.h asm/pptok.h asm/preproc.h asm/srcfile.h &
 config/msvc.h config/unconfig.h config/unknown.h config/watcom.h &
 include/bytesex.h include/compiler.h include/error.h include/hashtbl.h &
 include/labels.h include/macros.h include/nasm.h include/nasmint.h &
 include/nasmlib.h include/nctype.h include/opflags.h include/perfhash.h &
 include/strlist.h include/tables.h include/warnings.h x86/insnsi.h &
 x86/regflags.c x86/regs.h x86/x86const.h
x86/regs.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/nasmint.h include/tables.h &
 x86/insnsi.h x86/regs.c
x86/regvals.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/nasmint.h include/tables.h &
 x86/insnsi.h x86/regvals.c
zlib/adler32.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/nasmint.h zlib/adler32.c &
 zlib/zconf.h zlib/zlib.h zlib/zutil.h
zlib/crc32.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/nasmint.h zlib/crc32.c &
 zlib/crc32.h zlib/zconf.h zlib/zlib.h zlib/zutil.h
zlib/infback.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/nasmint.h zlib/infback.c &
 zlib/inffast.h zlib/inffixed.h zlib/inflate.h zlib/inftrees.h zlib/zconf.h &
 zlib/zlib.h zlib/zutil.h
zlib/inffast.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/nasmint.h zlib/inffast.c &
 zlib/inffast.h zlib/inflate.h zlib/inftrees.h zlib/zconf.h zlib/zlib.h &
 zlib/zutil.h
zlib/inflate.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/nasmint.h zlib/inffast.h &
 zlib/inffixed.h zlib/inflate.c zlib/inflate.h zlib/inftrees.h zlib/zconf.h &
 zlib/zlib.h zlib/zutil.h
zlib/inftrees.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/nasmint.h zlib/inftrees.c &
 zlib/inftrees.h zlib/zconf.h zlib/zlib.h zlib/zutil.h
zlib/zutil.obj: config/msvc.h config/unconfig.h config/unknown.h &
 config/watcom.h include/compiler.h include/nasmint.h zlib/gzguts.h &
 zlib/zconf.h zlib/zlib.h zlib/zutil.c zlib/zutil.h
