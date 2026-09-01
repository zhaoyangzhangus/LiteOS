# -*- makefile -*-
# SPDX-License-Identifier: BSD-2-Clause
# Copyright 1996-2025 The NASM Authors - All Rights Reserved
#
# Makefile for building NASM using Microsoft Visual C++ and NMAKE.
# Last tested on Visual Studio 2022 Community Edition.
#
# Make sure to have the proper directories in your path.
# This is typically done by opening the Visual Studio Command Prompt.
#

top_srcdir	= .
srcdir		= .
objdir          = .
VPATH		= .
prefix		= "C:\Program Files\NASM"
exec_prefix	= $(prefix)
bindir		= $(prefix)/bin
mandir		= $(prefix)/man

MANIFEST_FLAGS  = /manifest:embed /manifestfile:$(MANIFEST)

!IF "$(DEBUG)" == "1"
OPTFLAGS	= /Od
LDFLAGS		= /debug
!ELSE
OPTFLAGS	= /O2
 # /OPT:REF and /OPT:ICF two undo /DEBUG harm
LDFLAGS		= /debug /opt:ref /opt:icf
!ENDIF

CC		= cl
AR		= lib
ARFLAGS		= /nologo

CFLAGS		= $(OPTFLAGS) /Zi /nologo /std:c11 /bigobj
BUILD_CFLAGS	= $(CFLAGS) /W2
INTERNAL_CFLAGS = /I$(srcdir) /I. \
		  /I$(srcdir)/include /I./include \
		  /I$(srcdir)/x86 /I./x86 \
		  /I$(srcdir)/asm /I./asm \
		  /I$(srcdir)/disasm /I./disasm \
		  /I$(srcdir)/output /I./output \
		  /I$(srcdir)/zlib
ALL_CFLAGS	= $(BUILD_CFLAGS) $(INTERNAL_CFLAGS)
MANIFEST_FLAGS  = /manifest:embed /manifestinput:$(MANIFEST)
ALL_LDFLAGS	= /link $(LDFLAGS) $(MANIFEST_FLAGS) /subsystem:console /release
LIBS		=

PERL		= perl
PERLFLAGS	= -I$(srcdir)/perllib -I$(srcdir)
!IF [$(PERL) $(PERLFLAGS) -e "exit 0;"] == 0
RUNPERL         = $(PERL) $(PERLFLAGS)
!ELSE
RUNPERL         = :
!ENDIF

MAKENSIS        = makensis

RM_F		= -del /s /f /q
LN_S		= copy /y
EMPTY		= copy /y nul:
SIDE		= @rem Created by side effect

# Binary suffixes
O               = obj
A		= lib
X               = .exe
.SUFFIXES:
.SUFFIXES: $(X) .$(A) .obj .c .i .s .1 .man

.c.obj:
	$(CC) /c $(ALL_CFLAGS) /Fo:$@ $<

MANIFEST = win/manifest.xml

DIRS =

ZLIB    = $(ZLIBOBJ)

#-- Begin File Lists --#
# Edit in Makefile.in, not here!
NASM    = asm\nasm.obj
NDISASM = disasm\ndisasm.obj

PROGOBJ = $(NASM) $(NDISASM)
PROGS   = nasm$(X) ndisasm$(X)

# Objects for the local copy of zlib. The variable ZLIB is set to
# $(ZLIBOBJ) if the internal version of zlib should be used.
ZLIBOBJ = \
	zlib\adler32.obj \
	zlib\crc32.obj \
	zlib\infback.obj \
	zlib\inffast.obj \
	zlib\inflate.obj \
	zlib\inftrees.obj \
	zlib\zutil.obj

# Common library objects
LIBOBJ_COM = \
	stdlib\snprintf.obj stdlib\vsnprintf.obj stdlib\strlcpy.obj \
	stdlib\strnlen.obj \
	\
	nasmlib\ver.obj \
	nasmlib\alloc.obj nasmlib\asprintf.obj \
	nasmlib\crc32b.obj nasmlib\crc64.obj nasmlib\md5c.obj \
	nasmlib\string.obj nasmlib\nctype.obj \
	nasmlib\file.obj nasmlib\fileio.obj nasmlib\mmap.obj \
	nasmlib\realpath.obj nasmlib\path.obj \
	nasmlib\ilog2.obj nasmlib\numstr.obj \
	nasmlib\rlimit.obj \
	nasmlib\zerobuf.obj nasmlib\bsi.obj \
	nasmlib\rbtree.obj nasmlib\hashtbl.obj \
	nasmlib\raa.obj nasmlib\saa.obj \
	nasmlib\strlist.obj \
	nasmlib\perfhash.obj nasmlib\badenum.obj \
	nasmlib\readnum.obj \
	\
	common\common.obj common\errstubs.obj \
	\
	x86\insnsa.obj x86\insnsb.obj x86\insnsn.obj \
	x86\regs.obj x86\regvals.obj x86\regflags.obj \
	x86\iflag.obj \
	\
	$(ZLIB)

# Files dependent on warnings.dat
WARNOBJ   = asm\warnings.obj
WARNFILES = asm\warnings_c.h include\warnings.h doc\warnings.src

OUTPUTOBJ = \
	output\outform.obj output\outlib.obj \
	output\nulldbg.obj output\nullout.obj \
	output\outbin.obj output\outaout.obj output\outcoff.obj \
	output\outelf.obj \
	output\outobj.obj output\outas86.obj \
	output\outdbg.obj output\outieee.obj output\outmacho.obj \
	output\codeview.obj

# Assembler-only library objects
LIBOBJ_ASM = \
	asm\error.obj \
	asm\floats.obj \
	asm\directiv.obj \
	asm\pragma.obj \
	asm\assemble.obj asm\labels.obj asm\parser.obj \
	asm\preproc.obj asm\quote.obj \
	asm\listing.obj asm\eval.obj asm\exprlib.obj asm\exprdump.obj \
	asm\stdscan.obj \
	asm\getbool.obj \
	asm\strfunc.obj \
	asm\segalloc.obj \
	asm\rdstrnum.obj \
	asm\srcfile.obj \
	asm\directbl.obj \
	asm\pptok.obj \
	asm\tokhash.obj \
	asm\uncompress.obj \
	\
	macros\macros.obj \
	\
	$(WARNOBJ) \
	$(OUTPUTOBJ)

# Objects which are only used for the disassembler
LIBOBJ_DIS = \
	disasm\disasm.obj disasm\sync.obj disasm\prefix.obj \
	disasm\diserror.obj \
	\
	x86\insnsd.obj x86\regdis.obj

LIBOBJ    = $(LIBOBJ_COM) $(LIBOBJ_ASM) $(LIBOBJ_DIS)
ALLOBJ    = $(PROGOBJ) $(LIBOBJ)
SUBDIRS  = stdlib nasmlib include config output asm disasm x86 \
	   common zlib macros misc
XSUBDIRS = nsis win test doc editors
DEPDIRS  = . $(SUBDIRS)

EDITORS  = editors\nasmtok.el editors\nasmtok.json

#-- End File Lists --#

NASMLIB = libnasm.$(A)
ASMLIB  = libasm.$(A)
DISLIB  = libdis.$(A)

all: nasm$(X) ndisasm$(X)

nasm$(X): $(NASM) $(MANIFEST) $(ASMLIB) $(NASMLIB)
	$(CC) /Fe:$@ $(ALL_CFLAGS) $(NASM) $(ASMLIB) $(NASMLIB) $(LIBS) \
		$(ALL_LDFLAGS)

ndisasm$(X): $(NDISASM) $(MANIFEST) $(DISLIB) $(NASMLIB)
	$(CC) /Fe:$@ $(ALL_CFLAGS) $(NDISASM) $(DISLIB) $(NASMLIB) $(LIBS) \
		$(ALL_LDFLAGS)

$(NASMLIB): $(LIBOBJ_COM)
	$(AR) $(ARFLAGS) /out:$@ $**

$(ASMLIB): $(LIBOBJ_ASM)
	$(AR) $(ARFLAGS) /out:$@ $**

$(DISLIB): $(LIBOBJ_DIS)
	$(AR) $(ARFLAGS) /out:$@ $**

# These are specific to certain Makefile syntaxes...
WARNSRCS  = $(LIBOBJ_NW:.c=.obj)

#-- Begin Generated File Rules --#
# Edit in Makefile.in, not here!

# These source files are automagically generated from data files using
# Perl scripts. They're distributed, though, so it isn't necessary to
# have Perl just to recompile NASM from the distribution.

# Perl-generated source files
PERLREQ_CLEANABLE = \
	  x86\insnsb.c x86\insnsa.c x86\insnsd.c x86\insnsi.h x86\insnsn.c \
	  x86\regs.c x86\regs.h x86\regflags.c x86\regdis.c x86\regdis.h \
	  x86\regvals.c asm\tokhash.c asm\tokens.h asm\pptok.h asm\pptok.c \
	  x86\iflag.c x86\iflaggen.h \
	  macros\macros.c \
	  asm\pptok.ph asm\directbl.c asm\directiv.h \
	  $(WARNFILES) \
	  version.h version.mac version.mak nsis\version.nsh

PERLREQ = $(PERLREQ_CLEANABLE)

INSDEP = x86\insns.xda x86\insns.pl x86\insns-iflags.ph x86\iflags.ph

x86\insns.xda: x86\insns.dat x86\preinsns.pl $(DIRS)
	$(RUNPERL) $(srcdir)\x86\preinsns.pl $(srcdir)\x86\insns.dat $@

x86\iflag.c: $(INSDEP)
	$(RUNPERL) $(srcdir)\x86\insns.pl -fc \
		x86\insns.xda x86\iflag.c
x86\iflaggen.h: $(INSDEP)
	$(RUNPERL) $(srcdir)\x86\insns.pl -fh \
		x86\insns.xda x86\iflaggen.h
x86\insnsb.c: $(INSDEP)
	$(RUNPERL) $(srcdir)\x86\insns.pl -b \
		x86\insns.xda x86\insnsb.c
x86\insnsa.c: $(INSDEP)
	$(RUNPERL) $(srcdir)\x86\insns.pl -a \
		x86\insns.xda x86\insnsa.c
x86\insnsd.c: $(INSDEP)
	$(RUNPERL) $(srcdir)\x86\insns.pl -d \
		x86\insns.xda x86\insnsd.c
x86\insnsi.h: $(INSDEP)
	$(RUNPERL) $(srcdir)\x86\insns.pl -i \
		x86\insns.xda x86\insnsi.h
x86\insnsn.c: $(INSDEP)
	$(RUNPERL) $(srcdir)\x86\insns.pl -n \
		x86\insns.xda x86\insnsn.c

# These files contains all the standard macros that are derived from
# the version number.
version.h: version version.pl
	$(RUNPERL) $(srcdir)\version.pl h < $(srcdir)\version > version.h
version.mac: version version.pl
	$(RUNPERL) $(srcdir)\version.pl mac < $(srcdir)\version > version.mac
version.sed: version version.pl
	$(RUNPERL) $(srcdir)\version.pl sed < $(srcdir)\version > version.sed
version.mak: version version.pl
	$(RUNPERL) $(srcdir)\version.pl make < $(srcdir)\version > version.mak
nsis\version.nsh: version version.pl $(DIRS)
	$(RUNPERL) $(srcdir)\version.pl nsis < $(srcdir)\version > nsis\version.nsh

# This source file is generated from the standard macros file
# `standard.mac' by another Perl script. Again, it's part of the
# standard distribution.
macros\macros.c: macros\macros.pl asm\pptok.ph version.mac \
	$(srcdir)\macros\*.mac $(srcdir)\output\*.mac
	$(RUNPERL) $(srcdir)\macros\macros.pl version.mac \
		$(srcdir)\macros\*.mac $(srcdir)\output\*.mac

# These source files are generated from regs.dat by yet another
# perl script.
x86\regs.c: x86\regs.dat x86\regs.pl
	$(RUNPERL) $(srcdir)\x86\regs.pl c \
		$(srcdir)\x86\regs.dat > x86\regs.c
x86\regflags.c: x86\regs.dat x86\regs.pl
	$(RUNPERL) $(srcdir)\x86\regs.pl fc \
		$(srcdir)\x86\regs.dat > x86\regflags.c
x86\regdis.c: x86\regs.dat x86\regs.pl
	$(RUNPERL) $(srcdir)\x86\regs.pl dc \
		$(srcdir)\x86\regs.dat > x86\regdis.c
x86\regdis.h: x86\regs.dat x86\regs.pl
	$(RUNPERL) $(srcdir)\x86\regs.pl dh \
		$(srcdir)\x86\regs.dat > x86\regdis.h
x86\regvals.c: x86\regs.dat x86\regs.pl
	$(RUNPERL) $(srcdir)\x86\regs.pl vc \
		$(srcdir)\x86\regs.dat > x86\regvals.c
x86\regs.h: x86\regs.dat x86\regs.pl
	$(RUNPERL) $(srcdir)\x86\regs.pl h \
		$(srcdir)\x86\regs.dat > x86\regs.h


# Assembler token hash
asm\tokhash.c: x86\insns.xda x86\insnsn.c asm\tokens.dat asm\tokhash.pl \
	perllib\phash.ph
	$(RUNPERL) $(srcdir)\asm\tokhash.pl c \
		x86\insnsn.c $(srcdir)\x86\regs.dat \
		$(srcdir)\asm\tokens.dat > asm\tokhash.c

# Assembler token metadata
asm\tokens.h: x86\insns.xda x86\insnsn.c asm\tokens.dat asm\tokhash.pl \
	perllib\phash.ph
	$(RUNPERL) $(srcdir)\asm\tokhash.pl h \
		x86\insnsn.c $(srcdir)\x86\regs.dat \
		$(srcdir)\asm\tokens.dat > asm\tokens.h

# Preprocessor token hash
asm\pptok.h: asm\pptok.dat asm\pptok.pl perllib\phash.ph
	$(RUNPERL) $(srcdir)\asm\pptok.pl h \
		$(srcdir)\asm\pptok.dat asm\pptok.h
asm\pptok.c: asm\pptok.dat asm\pptok.pl perllib\phash.ph
	$(RUNPERL) $(srcdir)\asm\pptok.pl c \
		$(srcdir)\asm\pptok.dat asm\pptok.c
asm\pptok.ph: asm\pptok.dat asm\pptok.pl perllib\phash.ph
	$(RUNPERL) $(srcdir)\asm\pptok.pl ph \
		$(srcdir)\asm\pptok.dat asm\pptok.ph
doc\pptok.src: asm\pptok.dat asm\pptok.pl perllib\phash.ph
	$(RUNPERL) $(srcdir)\asm\pptok.pl src \
		$(srcdir)\asm\pptok.dat doc\pptok.src

# Directives hash
asm\directiv.h: asm\directiv.dat nasmlib\perfhash.pl perllib\phash.ph
	$(RUNPERL) $(srcdir)\nasmlib\perfhash.pl h \
		$(srcdir)\asm\directiv.dat asm\directiv.h
asm\directbl.c: asm\directiv.dat nasmlib\perfhash.pl perllib\phash.ph
	$(RUNPERL) $(srcdir)\nasmlib\perfhash.pl c \
		$(srcdir)\asm\directiv.dat asm\directbl.c

# Editor token files
editors\nasmtok.el: editors\nasmtok.pl asm\tokhash.c asm\pptok.c \
		 asm\directiv.dat macros\macros.c editors\builtin.mac \
		 version.mak
	$(RUNPERL) $(srcdir)\editors\nasmtok.pl -el $@ $(srcdir) $(objdir)

editors\nasmtok.json: editors\nasmtok.pl asm\tokhash.c asm\pptok.c \
		 asm\directiv.dat macros\macros.c editors\builtin.mac \
		 version.mak
	$(RUNPERL) $(srcdir)\editors\nasmtok.pl -json $@ $(srcdir) $(objdir)

editors: $(EDITORS) $(PHONY)

asm\warnings_c.h: asm\warnings.pl asm\warnings.dat
	$(RUNPERL) $(srcdir)\asm\warnings.pl c asm\warnings_c.h \
		$(srcdir)\asm\warnings.dat

include\warnings.h: asm\warnings.pl asm\warnings.dat
	$(RUNPERL) $(srcdir)\asm\warnings.pl h include\warnings.h \
		$(srcdir)\asm\warnings.dat

doc\warnings.src: asm\warnings.pl asm\warnings.dat
	$(RUNPERL) $(srcdir)\asm\warnings.pl doc doc\warnings.src \
		$(srcdir)\asm\warnings.dat

$(PERLREQ): $(DIRS)

perlreq: $(PERLREQ) $(PHONY)

warnings: $(WARNFILES) $(PHONY)

#-- End Generated File Rules --#

perlreq: $(PERLREQ)

#-- Begin NSIS Rules --#
# Edit in Makefile.in, not here!

nsis\arch.nsh: nsis\getpearch.pl nasm$(X) $(DIRS)
	$(PERL) $(srcdir)\nsis\getpearch.pl nasm$(X) > nsis\arch.nsh

# Should only be done after "make everything".
# The use of redirection here keeps makensis from moving the cwd to the
# source directory.
nsis: nsis\nasm.nsi nsis\arch.nsh nsis\version.nsh
	$(MAKENSIS) -Dsrcdir=$(srcdir) -Dobjdir=$(objdir) - \
		< $(srcdir)\nsis\nasm.nsi

#-- End NSIS Rules --#

clean:
	-del /f /s *.obj
	-del /f /s *.pdb
	-del /f /s *.s
	-del /f /s *.i
	-del /f $(NASMLIB) $(RDFLIB)
	-del /f nasm$(X)
	-del /f ndisasm$(X)

distclean: clean
	-del /f config.h
	-del /f config.log
	-del /f config.status
	-del /f Makefile
	-del /f /s *~
	-del /f /s *.bak
	-del /f /s *.lst
	-del /f /s *.bin
	-del /f /s *.dep
	-del /f output\*~
	-del /f output\*.bak
	-del /f test\*.lst
	-del /f test\*.bin
	-del /f test\*.obj
	-del /f test\*.bin
	-del /f/s autom4te*.cache

cleaner: clean
	-del /f $(PERLREQ)
	-del /f *.man
	-del /f nasm.spec
	rem cd doc && $(MAKE) clean

spotless: distclean cleaner
	-del /f doc\Makefile
	-del doc\*~
	-del doc\*.bak

strip:

# Abuse doc/Makefile.in to build nasmdoc.pdf only
docs:
	cd doc && $(MAKE) /f Makefile.in srcdir=. top_srcdir=.. \
		PERL=$(PERL) PDFOPT= nasmdoc.pdf

everything: all docs nsis

#
# Does this version of this file have external dependencies?  This definition
# will be automatically updated by mkdep.pl as needed.
#
EXTERNAL_DEPENDENCIES = 0

#
# Generate dependency information for this Makefile only.
# If this Makefile has external dependency information, then
# the dependency information will remain external, so it doesn't
# pollute the git logs.
#
msvc.dep: $(PERLREQ) tools\mkdep.pl
	$(RUNPERL) tools\mkdep.pl -M Mkfiles\msvc.mak -- $(DEPDIRS)

dep: msvc.dep

# Include and/or generate msvc.dep as needed. This is too complex to
# use the include-command feature, but we can open-code it here.
MKDEP=0
!IF $(EXTERNAL_DEPENDENCIES) == 1 && $(MKDEP) == 0
!IF EXISTS(msvc.dep)
!INCLUDE msvc.dep
!ELSEIF [$(MAKE) /c MKDEP=1 /f Mkfiles\msvc.mak msvc.dep] == 0
!INCLUDE msvc.dep
!ELSE
!ERROR Unable to rebuild dependencies file msvc.dep
!ENDIF
!ENDIF

#-- Magic hints to mkdep.pl --#
# @object-ending: ".obj"
# @path-separator: "\"
# @exclude: "config/config.h"
# @external: "msvc.dep"
# @selfrule: "1"
#-- Everything below is generated by mkdep.pl - do not edit --#
asm\assemble.obj: asm\asmutil.h asm\assemble.c asm\assemble.h asm\directiv.h \
 asm\listing.h asm\pptok.h asm\preproc.h asm\srcfile.h asm\tokens.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 include\bytesex.h include\compiler.h include\dbginfo.h include\disp8.h \
 include\error.h include\hashtbl.h include\iflag.h include\ilog2.h \
 include\insns.h include\labels.h include\macros.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\rbtree.h include\strlist.h include\tables.h \
 include\warnings.h x86\iflaggen.h x86\insnsi.h x86\regs.h x86\x86const.h
asm\directbl.obj: asm\directbl.c asm\directiv.h config\msvc.h \
 config\unconfig.h config\unknown.h config\watcom.h include\bytesex.h \
 include\compiler.h include\nasmint.h include\nasmlib.h include\perfhash.h
asm\directiv.obj: asm\asmutil.h asm\assemble.h asm\directiv.c asm\directiv.h \
 asm\eval.h asm\floats.h asm\listing.h asm\pptok.h asm\preproc.h asm\quote.h \
 asm\srcfile.h asm\stdscan.h config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\error.h include\hashtbl.h include\iflag.h include\ilog2.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\strlist.h include\tables.h include\warnings.h output\outform.h \
 x86\iflaggen.h x86\insnsi.h x86\regs.h x86\x86const.h
asm\error.obj: asm\directiv.h asm\error.c asm\listing.h asm\pptok.h \
 asm\preproc.h asm\srcfile.h config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\error.h include\hashtbl.h include\labels.h include\macros.h \
 include\nasm.h include\nasmint.h include\nasmlib.h include\nctype.h \
 include\opflags.h include\perfhash.h include\strlist.h include\tables.h \
 include\warnings.h x86\insnsi.h x86\regs.h x86\x86const.h
asm\eval.obj: asm\asmutil.h asm\assemble.h asm\directiv.h asm\eval.c \
 asm\eval.h asm\floats.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 include\bytesex.h include\compiler.h include\error.h include\hashtbl.h \
 include\iflag.h include\ilog2.h include\labels.h include\macros.h \
 include\nasm.h include\nasmint.h include\nasmlib.h include\nctype.h \
 include\opflags.h include\perfhash.h include\strlist.h include\tables.h \
 include\warnings.h x86\iflaggen.h x86\insnsi.h x86\regs.h x86\x86const.h
asm\exprdump.obj: asm\directiv.h asm\exprdump.c asm\pptok.h asm\preproc.h \
 asm\srcfile.h config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\hashtbl.h include\labels.h include\macros.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\strlist.h include\tables.h include\warnings.h \
 x86\insnsi.h x86\regs.h x86\x86const.h
asm\exprlib.obj: asm\directiv.h asm\exprlib.c asm\pptok.h asm\preproc.h \
 asm\srcfile.h config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\hashtbl.h include\labels.h include\macros.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\strlist.h include\tables.h include\warnings.h \
 x86\insnsi.h x86\regs.h x86\x86const.h
asm\floats.obj: asm\directiv.h asm\floats.c asm\floats.h asm\pptok.h \
 asm\preproc.h asm\srcfile.h config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\error.h include\hashtbl.h include\labels.h include\macros.h \
 include\nasm.h include\nasmint.h include\nasmlib.h include\nctype.h \
 include\opflags.h include\perfhash.h include\strlist.h include\tables.h \
 include\warnings.h x86\insnsi.h x86\regs.h x86\x86const.h
asm\getbool.obj: asm\asmutil.h asm\directiv.h asm\eval.h asm\getbool.c \
 asm\pptok.h asm\preproc.h asm\srcfile.h asm\stdscan.h config\msvc.h \
 config\unconfig.h config\unknown.h config\watcom.h include\bytesex.h \
 include\compiler.h include\error.h include\hashtbl.h include\labels.h \
 include\macros.h include\nasm.h include\nasmint.h include\nasmlib.h \
 include\nctype.h include\opflags.h include\perfhash.h include\strlist.h \
 include\tables.h include\warnings.h x86\insnsi.h x86\regs.h x86\x86const.h
asm\labels.obj: asm\directiv.h asm\labels.c asm\pptok.h asm\preproc.h \
 asm\srcfile.h config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\hashtbl.h include\labels.h include\macros.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\strlist.h include\tables.h include\warnings.h \
 x86\insnsi.h x86\regs.h x86\x86const.h
asm\listing.obj: asm\directiv.h asm\listing.c asm\listing.h asm\pptok.h \
 asm\preproc.h asm\srcfile.h config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\error.h include\hashtbl.h include\labels.h include\macros.h \
 include\nasm.h include\nasmint.h include\nasmlib.h include\nctype.h \
 include\opflags.h include\perfhash.h include\strlist.h include\tables.h \
 include\warnings.h x86\insnsi.h x86\regs.h x86\x86const.h
asm\nasm.obj: asm\asmutil.h asm\assemble.h asm\directiv.h asm\eval.h \
 asm\floats.h asm\listing.h asm\nasm.c asm\parser.h asm\pptok.h \
 asm\preproc.h asm\quote.h asm\srcfile.h asm\stdscan.h asm\tokens.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 include\bytesex.h include\compiler.h include\error.h include\hashtbl.h \
 include\iflag.h include\ilog2.h include\insns.h include\labels.h \
 include\macros.h include\nasm.h include\nasmint.h include\nasmlib.h \
 include\nctype.h include\opflags.h include\perfhash.h include\raa.h \
 include\saa.h include\strlist.h include\tables.h include\ver.h \
 include\warnings.h output\outform.h x86\iflaggen.h x86\insnsi.h x86\regs.h \
 x86\x86const.h
asm\parser.obj: asm\asmutil.h asm\assemble.h asm\directiv.h asm\eval.h \
 asm\floats.h asm\parser.c asm\parser.h asm\pptok.h asm\preproc.h \
 asm\srcfile.h asm\stdscan.h asm\tokens.h config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\error.h include\hashtbl.h include\iflag.h include\ilog2.h \
 include\insns.h include\labels.h include\macros.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\strlist.h include\tables.h include\warnings.h \
 x86\iflaggen.h x86\insnsi.h x86\regs.h x86\x86const.h
asm\pptok.obj: asm\pptok.c asm\pptok.h asm\preproc.h config\msvc.h \
 config\unconfig.h config\unknown.h config\watcom.h include\bytesex.h \
 include\compiler.h include\hashtbl.h include\nasmint.h include\nasmlib.h \
 include\nctype.h
asm\pragma.obj: asm\asmutil.h asm\assemble.h asm\directiv.h asm\listing.h \
 asm\pptok.h asm\pragma.c asm\preproc.h asm\srcfile.h config\msvc.h \
 config\unconfig.h config\unknown.h config\watcom.h include\bytesex.h \
 include\compiler.h include\error.h include\hashtbl.h include\iflag.h \
 include\ilog2.h include\labels.h include\macros.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\strlist.h include\tables.h include\warnings.h \
 x86\iflaggen.h x86\insnsi.h x86\regs.h x86\x86const.h
asm\preproc.obj: asm\asmutil.h asm\assemble.h asm\directiv.h asm\eval.h \
 asm\listing.h asm\pptok.h asm\preproc.c asm\preproc.h asm\quote.h \
 asm\srcfile.h asm\stdscan.h asm\tokens.h config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\dbginfo.h include\error.h include\hashtbl.h include\iflag.h \
 include\ilog2.h include\labels.h include\macros.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\rbtree.h include\strlist.h include\tables.h \
 include\warnings.h x86\iflaggen.h x86\insnsi.h x86\regs.h x86\x86const.h
asm\quote.obj: asm\quote.c asm\quote.h config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\error.h include\nasmint.h include\nasmlib.h include\nctype.h \
 include\warnings.h
asm\rdstrnum.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\rdstrnum.c \
 asm\srcfile.h config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\hashtbl.h include\labels.h include\macros.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\strlist.h include\tables.h include\warnings.h \
 x86\insnsi.h x86\regs.h x86\x86const.h
asm\segalloc.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\segalloc.c \
 asm\srcfile.h asm\tokens.h config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\hashtbl.h include\iflag.h include\ilog2.h include\insns.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\strlist.h include\tables.h include\warnings.h x86\iflaggen.h \
 x86\insnsi.h x86\regs.h x86\x86const.h
asm\srcfile.obj: asm\srcfile.c asm\srcfile.h config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\hashtbl.h include\nasmint.h include\nasmlib.h
asm\stdscan.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\quote.h \
 asm\srcfile.h asm\stdscan.c asm\stdscan.h asm\tokens.h config\msvc.h \
 config\unconfig.h config\unknown.h config\watcom.h include\bytesex.h \
 include\compiler.h include\error.h include\hashtbl.h include\iflag.h \
 include\ilog2.h include\insns.h include\labels.h include\macros.h \
 include\nasm.h include\nasmint.h include\nasmlib.h include\nctype.h \
 include\opflags.h include\perfhash.h include\strlist.h include\tables.h \
 include\warnings.h x86\iflaggen.h x86\insnsi.h x86\regs.h x86\x86const.h
asm\strfunc.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 asm\strfunc.c config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\hashtbl.h include\labels.h include\macros.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\strlist.h include\tables.h include\warnings.h \
 x86\insnsi.h x86\regs.h x86\x86const.h
asm\tokhash.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 asm\stdscan.h asm\tokens.h asm\tokhash.c config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\error.h include\hashtbl.h include\iflag.h include\ilog2.h \
 include\insns.h include\labels.h include\macros.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\strlist.h include\tables.h include\warnings.h \
 x86\iflaggen.h x86\insnsi.h x86\regs.h x86\x86const.h
asm\uncompress.obj: asm\uncompress.c config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\error.h include\macros.h include\nasmint.h include\nasmlib.h \
 include\warnings.h zlib\zconf.h zlib\zlib.h
asm\warnings.obj: asm\warnings.c asm\warnings_c.h config\msvc.h \
 config\unconfig.h config\unknown.h config\watcom.h include\compiler.h \
 include\error.h include\nasmint.h include\warnings.h
common\common.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 asm\tokens.h common\common.c config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\error.h include\hashtbl.h include\iflag.h include\ilog2.h \
 include\insns.h include\labels.h include\macros.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\strlist.h include\tables.h include\warnings.h \
 x86\iflaggen.h x86\insnsi.h x86\regs.h x86\x86const.h
common\errstubs.obj: common\errstubs.c config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\error.h include\nasmint.h include\nasmlib.h include\warnings.h
disasm\disasm.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 asm\tokens.h config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h disasm\disasm.c disasm\disasm.h disasm\sync.h \
 include\bytesex.h include\compiler.h include\disp8.h include\error.h \
 include\hashtbl.h include\iflag.h include\ilog2.h include\insns.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\strlist.h include\tables.h include\warnings.h x86\iflaggen.h \
 x86\insnsi.h x86\regdis.h x86\regs.h x86\x86const.h
disasm\diserror.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 disasm\disasm.h disasm\diserror.c include\bytesex.h include\compiler.h \
 include\error.h include\hashtbl.h include\iflag.h include\ilog2.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\strlist.h include\tables.h include\warnings.h x86\iflaggen.h \
 x86\insnsi.h x86\regs.h x86\x86const.h
disasm\ndisasm.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 asm\tokens.h config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h disasm\disasm.h disasm\ndisasm.c disasm\sync.h \
 include\bytesex.h include\compiler.h include\error.h include\hashtbl.h \
 include\iflag.h include\ilog2.h include\insns.h include\labels.h \
 include\macros.h include\nasm.h include\nasmint.h include\nasmlib.h \
 include\nctype.h include\opflags.h include\perfhash.h include\strlist.h \
 include\tables.h include\ver.h include\warnings.h x86\iflaggen.h \
 x86\insnsi.h x86\regs.h x86\x86const.h
disasm\prefix.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 disasm\disasm.h disasm\prefix.c include\bytesex.h include\compiler.h \
 include\error.h include\hashtbl.h include\iflag.h include\ilog2.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\strlist.h include\tables.h include\warnings.h x86\iflaggen.h \
 x86\insnsi.h x86\regs.h x86\x86const.h
disasm\sync.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h disasm\sync.c disasm\sync.h include\bytesex.h \
 include\compiler.h include\nasmint.h include\nasmlib.h
macros\macros.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 include\bytesex.h include\compiler.h include\error.h include\hashtbl.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\strlist.h include\tables.h include\warnings.h macros\macros.c \
 output\outform.h x86\insnsi.h x86\regs.h x86\x86const.h
misc\crcgen.obj: misc\crcgen.c
misc\omfdump.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\nasmint.h \
 misc\omfdump.c
misc\xcrcgen.obj: misc\xcrcgen.c
nasmlib\alloc.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\nasmint.h include\nasmlib.h include\warnings.h nasmlib\alloc.c \
 nasmlib\alloc.h
nasmlib\asprintf.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\nasmint.h \
 include\nasmlib.h nasmlib\alloc.h nasmlib\asprintf.c
nasmlib\badenum.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\nasmint.h \
 include\nasmlib.h nasmlib\badenum.c
nasmlib\bsi.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\nasmint.h \
 include\nasmlib.h nasmlib\bsi.c
nasmlib\crc32b.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\hashtbl.h \
 include\nasmint.h include\nasmlib.h nasmlib\crc32b.c
nasmlib\crc64.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\hashtbl.h \
 include\nasmint.h include\nasmlib.h include\nctype.h nasmlib\crc64.c
nasmlib\file.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\nasmint.h include\nasmlib.h include\warnings.h nasmlib\file.c
nasmlib\fileio.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\nasmint.h include\nasmlib.h include\warnings.h nasmlib\fileio.c
nasmlib\hashtbl.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 include\bytesex.h include\compiler.h include\error.h include\hashtbl.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\strlist.h include\tables.h include\warnings.h nasmlib\hashtbl.c \
 x86\insnsi.h x86\regs.h x86\x86const.h
nasmlib\ilog2.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\ilog2.h include\nasmint.h \
 nasmlib\ilog2.c
nasmlib\md5c.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\md5.h include\nasmint.h \
 nasmlib\md5c.c
nasmlib\mmap.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\nasmint.h \
 include\nasmlib.h nasmlib\mmap.c
nasmlib\nctype.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\nasmint.h include\nctype.h \
 nasmlib\nctype.c
nasmlib\numstr.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\nasmint.h \
 include\nasmlib.h nasmlib\numstr.c
nasmlib\path.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\nasmint.h include\nasmlib.h include\warnings.h nasmlib\path.c
nasmlib\perfhash.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\hashtbl.h \
 include\nasmint.h include\nasmlib.h include\perfhash.h nasmlib\perfhash.c
nasmlib\raa.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\ilog2.h \
 include\nasmint.h include\nasmlib.h include\raa.h nasmlib\raa.c
nasmlib\rbtree.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\nasmint.h \
 include\nasmlib.h include\rbtree.h nasmlib\rbtree.c
nasmlib\readnum.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 include\bytesex.h include\compiler.h include\error.h include\hashtbl.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\strlist.h include\tables.h include\warnings.h nasmlib\readnum.c \
 x86\insnsi.h x86\regs.h x86\x86const.h
nasmlib\realpath.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\nasmint.h \
 include\nasmlib.h nasmlib\realpath.c
nasmlib\rlimit.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\nasmint.h \
 include\nasmlib.h nasmlib\rlimit.c
nasmlib\saa.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\ilog2.h \
 include\nasmint.h include\nasmlib.h include\saa.h nasmlib\saa.c
nasmlib\string.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\nasmint.h \
 include\nasmlib.h include\nctype.h nasmlib\string.c
nasmlib\strlist.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\hashtbl.h \
 include\nasmint.h include\nasmlib.h include\strlist.h nasmlib\strlist.c
nasmlib\ver.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\nasmint.h include\ver.h \
 nasmlib\ver.c version.h
nasmlib\zerobuf.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\nasmint.h \
 include\nasmlib.h nasmlib\zerobuf.c
output\codeview.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 include\bytesex.h include\compiler.h include\error.h include\hashtbl.h \
 include\labels.h include\macros.h include\md5.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\rbtree.h include\saa.h include\strlist.h \
 include\tables.h include\warnings.h output\codeview.c output\outlib.h \
 output\pecoff.h version.h x86\insnsi.h x86\regs.h x86\x86const.h
output\nulldbg.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 include\bytesex.h include\compiler.h include\error.h include\hashtbl.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\rbtree.h include\saa.h include\strlist.h include\tables.h \
 include\warnings.h output\nulldbg.c output\outlib.h x86\insnsi.h x86\regs.h \
 x86\x86const.h
output\nullout.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 include\bytesex.h include\compiler.h include\error.h include\hashtbl.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\rbtree.h include\saa.h include\strlist.h include\tables.h \
 include\warnings.h output\nullout.c output\outlib.h x86\insnsi.h x86\regs.h \
 x86\x86const.h
output\outaout.obj: asm\directiv.h asm\eval.h asm\pptok.h asm\preproc.h \
 asm\srcfile.h asm\stdscan.h config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\error.h include\hashtbl.h include\labels.h include\macros.h \
 include\nasm.h include\nasmint.h include\nasmlib.h include\nctype.h \
 include\opflags.h include\perfhash.h include\raa.h include\rbtree.h \
 include\saa.h include\strlist.h include\tables.h include\warnings.h \
 output\outaout.c output\outform.h output\outlib.h x86\insnsi.h x86\regs.h \
 x86\x86const.h
output\outas86.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 include\bytesex.h include\compiler.h include\error.h include\hashtbl.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\raa.h include\rbtree.h include\saa.h include\strlist.h \
 include\tables.h include\warnings.h output\outas86.c output\outform.h \
 output\outlib.h x86\insnsi.h x86\regs.h x86\x86const.h
output\outbin.obj: asm\directiv.h asm\eval.h asm\pptok.h asm\preproc.h \
 asm\srcfile.h asm\stdscan.h config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\error.h include\hashtbl.h include\labels.h include\macros.h \
 include\nasm.h include\nasmint.h include\nasmlib.h include\nctype.h \
 include\opflags.h include\perfhash.h include\rbtree.h include\saa.h \
 include\strlist.h include\tables.h include\warnings.h output\outbin.c \
 output\outform.h output\outlib.h x86\insnsi.h x86\regs.h x86\x86const.h
output\outcoff.obj: asm\directiv.h asm\eval.h asm\pptok.h asm\preproc.h \
 asm\srcfile.h config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\hashtbl.h include\ilog2.h include\labels.h include\macros.h \
 include\nasm.h include\nasmint.h include\nasmlib.h include\nctype.h \
 include\opflags.h include\perfhash.h include\raa.h include\rbtree.h \
 include\saa.h include\strlist.h include\tables.h include\ver.h \
 include\warnings.h output\outcoff.c output\outform.h output\outlib.h \
 output\pecoff.h x86\insnsi.h x86\regs.h x86\x86const.h
output\outdbg.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 asm\tokens.h config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\dbginfo.h \
 include\error.h include\hashtbl.h include\iflag.h include\ilog2.h \
 include\insns.h include\labels.h include\macros.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\rbtree.h include\saa.h include\strlist.h \
 include\tables.h include\warnings.h output\outdbg.c output\outform.h \
 output\outlib.h x86\iflaggen.h x86\insnsi.h x86\regs.h x86\x86const.h
output\outelf.obj: asm\directiv.h asm\eval.h asm\pptok.h asm\preproc.h \
 asm\srcfile.h asm\stdscan.h config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\error.h include\hashtbl.h include\labels.h include\macros.h \
 include\nasm.h include\nasmint.h include\nasmlib.h include\nctype.h \
 include\opflags.h include\perfhash.h include\raa.h include\rbtree.h \
 include\saa.h include\strlist.h include\tables.h include\ver.h \
 include\warnings.h output\dwarf.h output\elf.h output\outelf.c \
 output\outelf.h output\outform.h output\outlib.h output\stabs.h \
 x86\insnsi.h x86\regs.h x86\x86const.h
output\outform.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 include\bytesex.h include\compiler.h include\error.h include\hashtbl.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\rbtree.h include\saa.h include\strlist.h include\tables.h \
 include\warnings.h output\outform.c output\outform.h output\outlib.h \
 x86\insnsi.h x86\regs.h x86\x86const.h
output\outieee.obj: asm\asmutil.h asm\directiv.h asm\pptok.h asm\preproc.h \
 asm\srcfile.h config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\hashtbl.h include\labels.h include\macros.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\rbtree.h include\saa.h include\strlist.h \
 include\tables.h include\ver.h include\warnings.h output\outform.h \
 output\outieee.c output\outlib.h x86\insnsi.h x86\regs.h x86\x86const.h
output\outlib.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 include\bytesex.h include\compiler.h include\error.h include\hashtbl.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\raa.h include\rbtree.h include\saa.h include\strlist.h \
 include\tables.h include\warnings.h output\outlib.c output\outlib.h \
 x86\insnsi.h x86\regs.h x86\x86const.h
output\outmacho.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 include\bytesex.h include\compiler.h include\error.h include\hashtbl.h \
 include\ilog2.h include\labels.h include\macros.h include\nasm.h \
 include\nasmint.h include\nasmlib.h include\nctype.h include\opflags.h \
 include\perfhash.h include\raa.h include\rbtree.h include\saa.h \
 include\strlist.h include\tables.h include\ver.h include\warnings.h \
 output\dwarf.h output\macho.h output\outform.h output\outlib.h \
 output\outmacho.c x86\insnsi.h x86\regs.h x86\x86const.h
output\outobj.obj: asm\asmutil.h asm\directiv.h asm\eval.h asm\pptok.h \
 asm\preproc.h asm\srcfile.h asm\stdscan.h config\msvc.h config\unconfig.h \
 config\unknown.h config\watcom.h include\bytesex.h include\compiler.h \
 include\error.h include\hashtbl.h include\labels.h include\macros.h \
 include\nasm.h include\nasmint.h include\nasmlib.h include\nctype.h \
 include\opflags.h include\perfhash.h include\rbtree.h include\saa.h \
 include\strlist.h include\tables.h include\ver.h include\warnings.h \
 output\outform.h output\outlib.h output\outobj.c x86\insnsi.h x86\regs.h \
 x86\x86const.h
stdlib\snprintf.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\nasmint.h \
 include\nasmlib.h stdlib\snprintf.c
stdlib\strlcpy.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\nasmint.h stdlib\strlcpy.c
stdlib\strnlen.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\nasmint.h stdlib\strnlen.c
stdlib\vsnprintf.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\nasmint.h include\nasmlib.h include\warnings.h stdlib\vsnprintf.c
x86\iflag.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\iflag.h include\ilog2.h \
 include\nasmint.h x86\iflag.c x86\iflaggen.h
x86\insnsa.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 asm\tokens.h config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\hashtbl.h include\iflag.h include\ilog2.h include\insns.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\strlist.h include\tables.h include\warnings.h x86\iflaggen.h \
 x86\insnsa.c x86\insnsi.h x86\regs.h x86\x86const.h
x86\insnsb.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 asm\tokens.h config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\hashtbl.h include\iflag.h include\ilog2.h include\insns.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\strlist.h include\tables.h include\warnings.h x86\iflaggen.h \
 x86\insnsb.c x86\insnsi.h x86\regs.h x86\x86const.h
x86\insnsd.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 asm\tokens.h config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\bytesex.h include\compiler.h include\error.h \
 include\hashtbl.h include\iflag.h include\ilog2.h include\insns.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\strlist.h include\tables.h include\warnings.h x86\iflaggen.h \
 x86\insnsd.c x86\insnsi.h x86\regs.h x86\x86const.h
x86\insnsn.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\nasmint.h include\tables.h \
 x86\insnsi.h x86\insnsn.c
x86\regdis.obj: x86\regdis.c x86\regdis.h x86\regs.h
x86\regflags.obj: asm\directiv.h asm\pptok.h asm\preproc.h asm\srcfile.h \
 config\msvc.h config\unconfig.h config\unknown.h config\watcom.h \
 include\bytesex.h include\compiler.h include\error.h include\hashtbl.h \
 include\labels.h include\macros.h include\nasm.h include\nasmint.h \
 include\nasmlib.h include\nctype.h include\opflags.h include\perfhash.h \
 include\strlist.h include\tables.h include\warnings.h x86\insnsi.h \
 x86\regflags.c x86\regs.h x86\x86const.h
x86\regs.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\nasmint.h include\tables.h \
 x86\insnsi.h x86\regs.c
x86\regvals.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\nasmint.h include\tables.h \
 x86\insnsi.h x86\regvals.c
zlib\adler32.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\nasmint.h zlib\adler32.c \
 zlib\zconf.h zlib\zlib.h zlib\zutil.h
zlib\crc32.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\nasmint.h zlib\crc32.c \
 zlib\crc32.h zlib\zconf.h zlib\zlib.h zlib\zutil.h
zlib\infback.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\nasmint.h zlib\infback.c \
 zlib\inffast.h zlib\inffixed.h zlib\inflate.h zlib\inftrees.h zlib\zconf.h \
 zlib\zlib.h zlib\zutil.h
zlib\inffast.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\nasmint.h zlib\inffast.c \
 zlib\inffast.h zlib\inflate.h zlib\inftrees.h zlib\zconf.h zlib\zlib.h \
 zlib\zutil.h
zlib\inflate.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\nasmint.h zlib\inffast.h \
 zlib\inffixed.h zlib\inflate.c zlib\inflate.h zlib\inftrees.h zlib\zconf.h \
 zlib\zlib.h zlib\zutil.h
zlib\inftrees.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\nasmint.h zlib\inftrees.c \
 zlib\inftrees.h zlib\zconf.h zlib\zlib.h zlib\zutil.h
zlib\zutil.obj: config\msvc.h config\unconfig.h config\unknown.h \
 config\watcom.h include\compiler.h include\nasmint.h zlib\gzguts.h \
 zlib\zconf.h zlib\zlib.h zlib\zutil.c zlib\zutil.h
