# Pinecone Desktop Environment - Phase 1 build
#
# Cross-compiles DESKTOP.EXE from macOS using DJGPP + Allegro 4.4.
# Produces a COFF go32 DOS binary that needs a DPMI host at runtime
# (CWSDPMI.EXE on FreeDOS, or Pinecore's native DPMI host).

DJGPP_PATH ?= /opt/djgpp
export DJGPP := $(DJGPP_PATH)/djgpp.env

CC      := $(DJGPP_PATH)/bin/i586-pc-msdosdjgpp-gcc
CWSDPMI := $(DJGPP_PATH)/bin/cwsdpmi.exe

# Mac-native DJGPP cross-gcc doesn't auto-honour djgpp.env's DOS-style %DJDIR%
# paths, so we add the Allegro include/lib directories explicitly.
DJGPP_INCLUDE := $(DJGPP_PATH)/include
DJGPP_LIB     := $(DJGPP_PATH)/lib

CFLAGS  := -O2 -Wall -Wno-unused -Isrc -I$(DJGPP_INCLUDE)
LDFLAGS := -L$(DJGPP_LIB)
# -lpcnet: Pinecore BSD-sockets shim. Installed by `make libpcnet`
# (auto-invoked below) into $(DJGPP_LIB)/libpcnet.a + $(DJGPP_INCLUDE)/pcnet.h.
# Source lives in src/lib/pcnet/.
LIBS    := -lalleg -lpcnet

TARGET    := DESKTOP.EXE
BUILD_DIR := build

# libpcnet sub-project — built + installed before main.o so <pcnet.h>
# resolves and -lpcnet finds the archive. Sub-make is incremental (no-op
# when sources unchanged).
PCNET_DIR := src/lib/pcnet

# Unity build: src/main.c #include's every other .c file. Two reasons:
#
# 1. We want modular source for navigation (core/, shell/, apps/, …) but
#    keep one TU so the binary layout matches the known-good ~1.08 MB.
#    Smaller binaries trip the s38 _stubinfo size-shift bug (#GP at
#    EIP=0x1C76 in DJGPP CRT init). Until that's fixed in the kernel,
#    single-TU is the safe path.
#
# 2. Avoids GCC C99 `extern inline` semantics emitting duplicate copies
#    of Allegro's INLINE helpers (draw_character, gui_textout, etc.) in
#    every translation unit.
SRCS := src/main.c
OBJS := $(BUILD_DIR)/main.o
# All other .c files are dependencies of main.o, declared explicitly
# below so make rebuilds when any of them changes.
DEPS := $(shell find src -name '*.c' -not -name 'main.c') \
        $(shell find src -name '*.h')

# Pinecore parent project paths (for disk image staging)
PARENT       := ..
PINECORE_IMG := $(PARENT)/src/pinecore-pure-hdd.img
FREEDOS_HDD  := $(PARENT)/src/pinecore-hdd.img
FREEDOS_FLOP := $(PARENT)/src/pinecore-dist.img
DOOM_IMG     := $(PARENT)/src/pinecore-doom.img

.PHONY: all clean check pack-pinecore pack-freedos pack-doom \
        run-pinecore run-freedos run-doom run-dosbox help libpcnet

all: $(TARGET)

libpcnet:
	$(MAKE) -C $(PCNET_DIR) DJGPP_PATH=$(DJGPP_PATH) install

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@ls -l $@

$(BUILD_DIR)/main.o: src/main.c $(DEPS) libpcnet
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# Sanity: verify cross-compiler is reachable and we know the Allegro lib path
check:
	@echo "CC      = $(CC)"
	@echo "DJGPP   = $(DJGPP)"
	@$(CC) --version | head -1
	@test -f $(DJGPP_PATH)/lib/liballeg.a && echo "liballeg.a: found" || echo "liballeg.a: MISSING"
	@test -f $(CWSDPMI) && echo "CWSDPMI.EXE: found" || echo "CWSDPMI.EXE: MISSING"

# Copy DESKTOP.EXE onto Pinecore's HDD image (no CWSDPMI - Pinecore is the host).
# Uses mtools (mcopy). Will overwrite an existing DESKTOP.EXE.
pack-pinecore: $(TARGET)
	@test -f $(PINECORE_IMG) || (echo "Missing $(PINECORE_IMG)"; exit 1)
	mcopy -i $(PINECORE_IMG) -o $(TARGET) ::/DESKTOP.EXE
	@echo "DESKTOP.EXE copied to $(PINECORE_IMG)"

# Stage DESKTOP.EXE + CWSDPMI.EXE on the parent project's FreeDOS HDD image,
# which is the disk that pinecore-dist.img (FreeDOS floppy) mounts as C:.
pack-freedos: $(TARGET)
	@test -f $(FREEDOS_HDD) || (echo "Missing $(FREEDOS_HDD)"; exit 1)
	mcopy -i $(FREEDOS_HDD) -o $(TARGET) ::/DESKTOP.EXE
	mcopy -i $(FREEDOS_HDD) -o $(CWSDPMI) ::/CWSDPMI.EXE
	@echo "Staged DESKTOP.EXE + CWSDPMI.EXE on $(FREEDOS_HDD)"

# Boot FreeDOS (from floppy) with the HDD as C:. After boot:
#     C:
#     DESKTOP
# CWSDPMI.EXE is auto-discovered from the current directory.
run-freedos: pack-freedos
	qemu-system-i386 \
	    -drive file=$(FREEDOS_FLOP),format=raw,if=floppy,index=0 \
	    -drive file=$(FREEDOS_HDD),format=raw,if=ide,index=0 \
	    -serial file:/tmp/pinecone-serial.log -display cocoa -m 32 -boot a

# Boot Pinecore with DESKTOP.EXE on disk (Pinecore IS the DPMI host).
# At the Pinecore Commando:  exec desktop.exe
run-pinecore: pack-pinecore
	cd $(PARENT)/src && qemu-system-i386 -kernel kernel.pure.bin \
	    -drive file=pinecore-pure-hdd.img,format=raw,if=ide,index=0 \
	    -serial file:/tmp/pinecore-serial.log -display cocoa -m 32

# Stage DESKTOP.EXE + CWSDPMI.EXE onto the DOOM test image. This is the
# 64MB image that's been the active C: drive for DOOM testing — having
# DESKTOP.EXE alongside DOOM.EXE lets us A/B both clients against the
# same DPMI host in the same session.
pack-doom: $(TARGET)
	@test -f $(DOOM_IMG) || (echo "Missing $(DOOM_IMG)"; exit 1)
	mcopy -i $(DOOM_IMG) -o $(TARGET) ::/DESKTOP.EXE
	mcopy -i $(DOOM_IMG) -o $(CWSDPMI) ::/CWSDPMI.EXE
	@echo "Staged DESKTOP.EXE + CWSDPMI.EXE on $(DOOM_IMG)"

# Boot the DOOM image under Pinecore. At the shell:  exec DESKTOP.EXE
run-doom: pack-doom
	cd $(PARENT)/src && qemu-system-i386 -kernel kernel.pure.bin \
	    -drive file=pinecore-doom.img,format=raw,if=ide,index=0 \
	    -serial file:/tmp/pinecore-serial.log -display cocoa -m 32

# Launch DESKTOP.EXE in DOSBox using the bundled dosbox.conf. Requires
# DOSBox to be installed (brew install dosbox or dosbox-x).
run-dosbox: $(TARGET)
	@command -v dosbox >/dev/null 2>&1 || \
	    command -v dosbox-x >/dev/null 2>&1 || \
	    (echo "Install DOSBox first: brew install dosbox  (or dosbox-x)"; exit 1)
	dosbox -conf dosbox.conf 2>/dev/null || dosbox-x -conf dosbox.conf

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
	$(MAKE) -C $(PCNET_DIR) DJGPP_PATH=$(DJGPP_PATH) clean

help:
	@echo "Pinecone Desktop - Phase 1"
	@echo
	@echo "  make               - build DESKTOP.EXE"
	@echo "  make check         - verify toolchain paths"
	@echo "  make pack-freedos  - stage DESKTOP.EXE + CWSDPMI.EXE on FreeDOS HDD image"
	@echo "  make pack-pinecore - stage DESKTOP.EXE on Pinecore pure HDD image"
	@echo "  make pack-doom     - stage DESKTOP.EXE on the DOOM test image"
	@echo "  make run-freedos   - boot FreeDOS in QEMU (baseline CWSDPMI test)"
	@echo "  make run-pinecore  - boot Pinecore kernel in QEMU"
	@echo "  make run-doom      - boot Pinecore against the DOOM image in QEMU"
	@echo "  make run-dosbox    - launch DESKTOP.EXE in DOSBox (host-side test)"
	@echo "  make clean         - remove build artifacts"
