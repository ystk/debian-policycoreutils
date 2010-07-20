############################ -*- Mode: Makefile -*- ###########################
## local-vars.mk --- 
## Author           : Manoj Srivastava ( srivasta@glaurung.green-gryphon.com ) 
## Created On       : Sat Nov 15 10:43:00 2003
## Created On Node  : glaurung.green-gryphon.com
## Last Modified By : Manoj Srivastava
## Last Modified On : Thu Feb 12 22:50:48 2009
## Last Machine Used: anzu.internal.golden-gryphon.com
## Update Count     : 15
## Status           : Unknown, Use with caution!
## HISTORY          : 
## Description      : 
## 
## arch-tag: 1a76a87e-7af5-424a-a30d-61660c8f243e
## 
###############################################################################

FILES_TO_CLEAN  = debian/substvars debian/files
STAMPS_TO_CLEAN = 
DIRS_TO_CLEAN   = debian/stamp

# Location of the source dir
SRCTOP    := $(shell if [ "$$PWD" != "" ]; then echo $$PWD; else pwd; fi)
TMPTOP     = $(SRCTOP)/debian/$(package)
LINTIANDIR = $(TMPTOP)/usr/share/lintian/overrides
DOCBASEDIR = $(TMPTOP)/usr/share/doc-base

PREFIX=/usr

SBINDIR = $(TMPTOP)$(PREFIX)/sbin
BINDIR  = $(TMPTOP)$(PREFIX)/bin
LIBDIR  = $(TMPTOP)$(PREFIX)/lib
# Man Pages
MANDIR    = $(TMPTOP)/usr/share/man
MAN1DIR   = $(MANDIR)/man1
MAN3DIR   = $(MANDIR)/man3
MAN5DIR   = $(MANDIR)/man5
MAN7DIR   = $(MANDIR)/man7
MAN8DIR   = $(MANDIR)/man8

INFODIR = $(TMPTOP)/usr/share/info
DOCTOP  = $(TMPTOP)/usr/share/doc
DOCDIR  = $(DOCTOP)/$(package)
MENUDIR   = $(TMPTOP)/usr/lib/menu/

SELINUXDIR = $(TMPTOP)/selinux
PAMDIR     = $(TMPTOP)/etc/pam.d
PYDEFAULT  =$(strip $(shell pyversions -vd))
ifneq ($(firstword $(sort $(PYDEFAULT) 2.5)),2.5)
    PYVERSION := 2.5
else
    PYVERSION := $(PYDEFAULT)
endif
MODULES_DIR=$(TMPTOP)/usr/share/python-support/$(package)

define checkdir
	@test -f debian/rules -a -f setfiles/setfiles.c || \
          (echo Not in correct source directory; exit 1)
endef

define checkroot
	@test $$(id -u) = 0 || (echo need root priviledges; exit 1)
endef
