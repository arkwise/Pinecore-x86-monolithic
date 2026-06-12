# libpcnet — DJGPP user-side BSD-sockets shim for Pinecore's net-provider ABI.
#
# Builds a static archive (libpcnet.a) from pcnet.c, and installs the
# archive + header into the DJGPP cross-toolchain tree so any DJGPP app
# can `#include <pcnet.h>` + `-lpcnet`.
#
#   make            - build libpcnet.a in this directory
#   make install    - install libpcnet.a + pcnet.h into $(DJGPP_PATH)
#   make clean      - remove libpcnet.a + .o
#
# DJGPP_PATH may be overridden from the parent Makefile.

DJGPP_PATH ?= /opt/djgpp

CC     := $(DJGPP_PATH)/bin/i586-pc-msdosdjgpp-gcc
AR     := $(DJGPP_PATH)/bin/i586-pc-msdosdjgpp-ar
RANLIB := $(DJGPP_PATH)/bin/i586-pc-msdosdjgpp-ranlib

CFLAGS := -O2 -Wall -Wno-unused

LIB    := libpcnet.a
OBJS   := pcnet.o

INSTALL_INCDIR := $(DJGPP_PATH)/include
INSTALL_LIBDIR := $(DJGPP_PATH)/lib

.PHONY: all install clean

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $(OBJS)
	$(RANLIB) $@

pcnet.o: pcnet.c pcnet.h
	$(CC) $(CFLAGS) -c pcnet.c -o $@

install: $(INSTALL_INCDIR)/pcnet.h $(INSTALL_LIBDIR)/$(LIB)

$(INSTALL_INCDIR)/pcnet.h: pcnet.h
	@mkdir -p $(INSTALL_INCDIR)
	cp $< $@
	@echo "installed pcnet.h  -> $@"

$(INSTALL_LIBDIR)/$(LIB): $(LIB)
	@mkdir -p $(INSTALL_LIBDIR)
	cp $< $@
	@echo "installed $(LIB)   -> $@"

clean:
	rm -f $(LIB) $(OBJS)
