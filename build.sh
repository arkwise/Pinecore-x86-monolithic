#!/usr/bin/env bash
#
# build.sh — portable build & run for pinecore-x86 (Linux + macOS).
#
# The committed Makefiles are tuned for the original macOS dev box (hardcoded
# DJGPP path, `-display cocoa`, and `flat`/`pure-usb` targets that mcopy into
# local-only DOS disk images). This script wraps them portably: it detects the
# host OS, auto-installs every toolchain it needs, builds the kernel + modules
# + boot loader + the Pinecone DESKTOP.EXE client, assembles a bootable image,
# and can boot the result in QEMU.
#
# Toolchains handled:
#   * i686-elf binutils + gcc   (freestanding kernel cross-compiler)
#   * DJGPP (i586-pc-msdosdjgpp) + Allegro 4.2   (DESKTOP.EXE)
#   * nasm, mtools, qemu, python3
#
# Usage:
#   ./build.sh                 install missing tools, then build everything
#   ./build.sh --run           build, then boot in QEMU (auto-launches desktop)
#   ./build.sh --check         only report toolchain status; install/build nothing
#   ./build.sh --no-install    build only; fail (don't install) if a tool is missing
#   ./build.sh --no-desktop    skip DESKTOP.EXE (no DJGPP/Allegro needed)
#   ./build.sh --clean         remove build artifacts (keeps the committed image)
#
# Env overrides:
#   CROSS_PREFIX_DIR   where to install the i686-elf cross toolchain (default /opt/cross)
#   DJGPP_PATH         where DJGPP lives / gets installed         (default /opt/djgpp)
#   GCC_VER, BINUTILS_VER, DJGPP_GCC_VER, ALLEGRO_VER             pin versions
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
PINECONE_DIR="$SCRIPT_DIR/pinecone"
TOOLS_DIR="$SCRIPT_DIR/tools"

CROSS_TARGET="i686-elf"
CROSS_PREFIX_DIR="${CROSS_PREFIX_DIR:-/opt/cross}"
DJGPP_PATH="${DJGPP_PATH:-/opt/djgpp}"

BINUTILS_VER="${BINUTILS_VER:-2.42}"
GCC_VER="${GCC_VER:-13.2.0}"
DJGPP_GCC_VER="${DJGPP_GCC_VER:-12.2.0}"
ALLEGRO_VER="${ALLEGRO_VER:-4.2.3.1}"

# Flags
DO_INSTALL=1; DO_BUILD=1; DO_RUN=0; DO_DESKTOP=1; DO_CLEAN=0; CHECK_ONLY=0

# ---------------------------------------------------------------------------
# Pretty logging
# ---------------------------------------------------------------------------
if [ -t 1 ]; then C_B=$'\033[1m'; C_G=$'\033[32m'; C_Y=$'\033[33m'; C_R=$'\033[31m'; C_0=$'\033[0m'; else C_B=; C_G=; C_Y=; C_R=; C_0=; fi
log()  { printf '%s==>%s %s\n' "$C_B" "$C_0" "$*"; }
ok()   { printf '%s ok %s %s\n' "$C_G" "$C_0" "$*"; }
warn() { printf '%swarn%s %s\n' "$C_Y" "$C_0" "$*" >&2; }
die()  { printf '%sERR %s %s\n' "$C_R" "$C_0" "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# OS / package-manager detection
# ---------------------------------------------------------------------------
OS=""; PKG=""; SUDO=""
detect_os() {
  case "$(uname -s)" in
    Linux)  OS=linux ;;
    Darwin) OS=macos ;;
    *) die "Unsupported OS: $(uname -s) (Linux and macOS only)" ;;
  esac
  if [ "$OS" = macos ]; then
    command -v brew >/dev/null 2>&1 || die "Homebrew required on macOS — install from https://brew.sh"
    PKG=brew
  else
    for m in apt-get dnf pacman zypper; do command -v "$m" >/dev/null 2>&1 && { PKG=$m; break; }; done
    [ -n "$PKG" ] || warn "No known package manager found; base-tool auto-install may not work."
  fi
  # sudo only when we are not root and a target dir is unwritable
  [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1 && SUDO=sudo || SUDO=""
  ok "Host: $OS  (pkg: ${PKG:-none}, arch: $(uname -m))"
}

pkg_install() { # pkg_install <brew-names> <apt-names> [dnf] [pacman]
  local brewp="$1" aptp="$2" dnfp="${3:-$2}" pacp="${4:-$2}"
  case "$PKG" in
    brew)    brew install $brewp ;;
    apt-get) $SUDO apt-get update -y && $SUDO apt-get install -y $aptp ;;
    dnf)     $SUDO dnf install -y $dnfp ;;
    pacman)  $SUDO pacman -Sy --noconfirm $pacp ;;
    zypper)  $SUDO zypper install -y $aptp ;;
    *)       warn "Install these manually: $aptp" ;;
  esac
}

writable_or_sudo() { # echo "sudo" if path (or its first existing parent) is not writable
  local p="$1"; while [ ! -e "$p" ] && [ "$p" != "/" ]; do p="$(dirname "$p")"; done
  [ -w "$p" ] && echo "" || echo "$SUDO"
}

# ---------------------------------------------------------------------------
# Base tools: nasm, mtools, qemu, python3, git, wget + cross build deps
# ---------------------------------------------------------------------------
QEMU=""
find_qemu() { for q in qemu-system-i386 qemu-system-x86_64; do command -v "$q" >/dev/null 2>&1 && { QEMU="$q"; return; }; done; }

ensure_base_tools() {
  log "Checking base tools (nasm, mtools, qemu, python3, git)…"
  local need_nasm=0 need_mtools=0 need_qemu=0 need_py=0 need_git=0
  command -v nasm   >/dev/null 2>&1 || need_nasm=1
  command -v mcopy  >/dev/null 2>&1 || need_mtools=1
  find_qemu; [ -z "$QEMU" ] && need_qemu=1
  command -v python3>/dev/null 2>&1 || need_py=1
  command -v git    >/dev/null 2>&1 || need_git=1
  if [ $((need_nasm+need_mtools+need_qemu+need_py+need_git)) -eq 0 ]; then ok "base tools present"; return; fi
  [ "$DO_INSTALL" = 1 ] || die "Missing base tools and --no-install set. Need: nasm mtools qemu python3 git"
  [ $need_nasm   = 1 ] && { log "installing nasm";   pkg_install nasm nasm; }
  [ $need_mtools = 1 ] && { log "installing mtools"; pkg_install mtools mtools; }
  [ $need_qemu   = 1 ] && { log "installing qemu";   pkg_install qemu "qemu-system-x86" "qemu-system-x86" "qemu"; }
  [ $need_py     = 1 ] && { log "installing python3";pkg_install python3 python3; }
  [ $need_git    = 1 ] && { log "installing git";    pkg_install git git; }
  # cross-compiler build prerequisites (Linux source build only)
  if [ "$OS" = linux ]; then
    pkg_install "" "build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev wget" \
                   "gcc gcc-c++ make bison flex texinfo gmp-devel mpfr-devel libmpc-devel wget" \
                   "base-devel bison flex texinfo gmp mpfr libmpc wget" || true
  fi
  find_qemu
  ok "base tools ready"
}

# ---------------------------------------------------------------------------
# i686-elf cross toolchain
# ---------------------------------------------------------------------------
CROSS_BIN=""
find_cross() {
  if command -v ${CROSS_TARGET}-gcc >/dev/null 2>&1; then CROSS_BIN="$(dirname "$(command -v ${CROSS_TARGET}-gcc)")"; return 0; fi
  [ -x "$CROSS_PREFIX_DIR/bin/${CROSS_TARGET}-gcc" ] && { CROSS_BIN="$CROSS_PREFIX_DIR/bin"; return 0; }
  return 1
}

ensure_cross() {
  log "Checking $CROSS_TARGET cross toolchain…"
  if find_cross; then ok "cross-gcc: $CROSS_BIN/${CROSS_TARGET}-gcc"; return; fi
  [ "$DO_INSTALL" = 1 ] || die "Missing $CROSS_TARGET-gcc and --no-install set."
  if [ "$OS" = macos ]; then
    log "installing $CROSS_TARGET via Homebrew (nativeos tap)…"
    brew install i686-elf-binutils i686-elf-gcc || die "brew install i686-elf-* failed"
    find_cross || die "cross toolchain still not found after brew install"
    ok "cross-gcc: $CROSS_BIN/${CROSS_TARGET}-gcc"; return
  fi
  # ---- Linux: build binutils + gcc from source ----
  local S="$SUDO"; S="$(writable_or_sudo "$CROSS_PREFIX_DIR")"
  warn "Building $CROSS_TARGET binutils $BINUTILS_VER + gcc $GCC_VER from source — this takes a while (20–60 min)."
  local work; work="$(mktemp -d)"; trap 'rm -rf "$work"' RETURN
  $S mkdir -p "$CROSS_PREFIX_DIR"
  ( cd "$work"
    log "downloading binutils + gcc sources…"
    wget -q "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
    wget -q "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"
    tar xf "binutils-${BINUTILS_VER}.tar.xz"; tar xf "gcc-${GCC_VER}.tar.xz"
    ( cd "gcc-${GCC_VER}" && ./contrib/download_prerequisites >/dev/null 2>&1 || true )
    local J; J="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
    log "building binutils…"
    mkdir build-binutils && cd build-binutils
    ../"binutils-${BINUTILS_VER}"/configure --target="$CROSS_TARGET" --prefix="$CROSS_PREFIX_DIR" \
        --with-sysroot --disable-nls --disable-werror >/dev/null
    make -j"$J" >/dev/null && $S make install >/dev/null
    cd ..
    log "building gcc (c, freestanding)…"
    mkdir build-gcc && cd build-gcc
    PATH="$CROSS_PREFIX_DIR/bin:$PATH" ../"gcc-${GCC_VER}"/configure --target="$CROSS_TARGET" \
        --prefix="$CROSS_PREFIX_DIR" --disable-nls --enable-languages=c --without-headers >/dev/null
    PATH="$CROSS_PREFIX_DIR/bin:$PATH" make -j"$J" all-gcc all-target-libgcc >/dev/null
    $S env PATH="$CROSS_PREFIX_DIR/bin:$PATH" make install-gcc install-target-libgcc >/dev/null
  )
  find_cross || die "cross toolchain build finished but ${CROSS_TARGET}-gcc not found in $CROSS_PREFIX_DIR/bin"
  ok "cross-gcc built: $CROSS_BIN/${CROSS_TARGET}-gcc"
}

# ---------------------------------------------------------------------------
# DJGPP + Allegro 4.2 (for DESKTOP.EXE)
# ---------------------------------------------------------------------------
DJGPP_BIN=""
find_djgpp() {
  if command -v i586-pc-msdosdjgpp-gcc >/dev/null 2>&1; then DJGPP_BIN="$(dirname "$(command -v i586-pc-msdosdjgpp-gcc)")"; DJGPP_PATH="$(dirname "$DJGPP_BIN")"; return 0; fi
  [ -x "$DJGPP_PATH/bin/i586-pc-msdosdjgpp-gcc" ] && { DJGPP_BIN="$DJGPP_PATH/bin"; return 0; }
  return 1
}
allegro_present() { [ -f "$DJGPP_PATH/i586-pc-msdosdjgpp/lib/liballeg.a" ] || [ -f "$DJGPP_PATH/lib/liballeg.a" ]; }

ensure_djgpp() {
  log "Checking DJGPP toolchain…"
  if find_djgpp; then ok "djgpp: $DJGPP_BIN/i586-pc-msdosdjgpp-gcc"; else
    [ "$DO_INSTALL" = 1 ] || die "Missing DJGPP and --no-install set."
    warn "Building DJGPP gcc $DJGPP_GCC_VER from source via andrewwutw/build-djgpp — this takes a while."
    local S; S="$(writable_or_sudo "$(dirname "$DJGPP_PATH")")"
    local work; work="$(mktemp -d)"
    git clone --depth 1 https://github.com/andrewwutw/build-djgpp.git "$work/build-djgpp"
    $S mkdir -p "$DJGPP_PATH"
    ( cd "$work/build-djgpp" && $S env DJGPP_PREFIX="$DJGPP_PATH" ./build-djgpp.sh "$DJGPP_GCC_VER" )
    rm -rf "$work"
    find_djgpp || die "DJGPP build finished but i586-pc-msdosdjgpp-gcc not found in $DJGPP_PATH/bin"
    ok "djgpp built: $DJGPP_BIN/i586-pc-msdosdjgpp-gcc"
  fi
  # Allegro 4.2 into the DJGPP tree
  if allegro_present; then ok "Allegro present in DJGPP tree"; return; fi
  [ "$DO_INSTALL" = 1 ] || die "Missing Allegro 4.2 in $DJGPP_PATH and --no-install set."
  log "Building Allegro $ALLEGRO_VER for DJGPP from source…"
  local work; work="$(mktemp -d)"
  ( cd "$work"
    wget -q "https://github.com/liballeg/allegro5/releases/download/4.2.3.1/allegro-${ALLEGRO_VER}.tar.gz" \
      || wget -q "https://downloads.sourceforge.net/project/alleg/allegro/4.2.3.1/allegro-${ALLEGRO_VER}.tar.gz" \
      || die "could not download Allegro $ALLEGRO_VER — install Allegro 4.2 for DJGPP manually, then re-run with --no-install"
    tar xf "allegro-${ALLEGRO_VER}.tar.gz"; cd "allegro-${ALLEGRO_VER}"
    export DJGPP="$DJGPP_PATH/djgpp.env" DJDIR="$DJGPP_PATH"
    export PATH="$DJGPP_BIN:$PATH"
    ./fix.sh djgpp
    # build just the library; install into the DJGPP tree
    make lib   >/dev/null 2>&1 || make >/dev/null 2>&1 || die "Allegro library build failed"
    "$(writable_or_sudo "$DJGPP_PATH")" make install >/dev/null 2>&1 || make install >/dev/null 2>&1 || true
  )
  rm -rf "$work"
  allegro_present || die "Allegro build finished but liballeg.a not found in the DJGPP tree"
  ok "Allegro installed into DJGPP tree"
}

# ---------------------------------------------------------------------------
# Build steps
# ---------------------------------------------------------------------------
build_kernel() {
  log "Building kernels + modules + boot loader…"
  export PATH="$CROSS_BIN:$PATH"   # the Makefile hardcodes i686-elf-objcopy on PATH
  ( cd "$SRC_DIR"
    make all                                   # kernel.dos.bin + kernel.pure.bin
    make modules                               # *.kmd
    make pcboot-bins                           # mbr.bin + vbr.bin + pcboot.bin
    # flat binaries directly (skip the Makefile's `flat` target — it mcopies into
    # local-only DOS images that don't ship with the repo)
    ${CROSS_TARGET}-objcopy -O binary kernel.pure.bin kernel.pure.flat
    ${CROSS_TARGET}-objcopy -O binary kernel.dos.bin  kernel.dos.flat
  )
  ok "kernel.pure.bin / kernel.dos.bin / *.kmd / boot bins built"
}

build_desktop() {
  log "Building Pinecone DESKTOP.EXE (DJGPP + Allegro)…"
  ( cd "$PINECONE_DIR"
    # -DALLEGRO_HAVE_STDINT_H: Allegro 4.2's astdint.h otherwise macro-defines
    # int8_t/etc. which collide with modern DJGPP's real <stdint.h>.
    make DJGPP_PATH="$DJGPP_PATH" \
         CFLAGS="-O2 -Wall -Wno-unused -Isrc -I$DJGPP_PATH/include -DALLEGRO_HAVE_STDINT_H"
  )
  ok "DESKTOP.EXE built ($(du -h "$PINECONE_DIR/DESKTOP.EXE" | cut -f1))"
}

# Derive a bare FAT16 C: image from the committed partitioned USB image, then
# rebuild the bootable USB image with the freshly built kernel + modules + desktop.
HDD_IMG="$SRC_DIR/pinecore-pure-hdd.img"
USB_IMG="$SRC_DIR/pinecore-pure-usb.img"
ensure_hdd_image() {
  if [ -f "$HDD_IMG" ]; then ok "base disk image present: $(basename "$HDD_IMG")"; return 0; fi
  if [ -f "$USB_IMG" ]; then
    log "deriving $(basename "$HDD_IMG") from committed $(basename "$USB_IMG")…"
    # FAT16 partition starts at LBA 63 (offset 32256) in the USB image
    dd if="$USB_IMG" of="$HDD_IMG" bs=512 skip=63 status=none
    ok "base disk image derived"
    return 0
  fi
  warn "No base DOS disk image (pinecore-pure-hdd.img / -usb.img). The bootable image"
  warn "needs the non-redistributable DOS files (COMMAND.COM etc.). Skipping image assembly."
  return 1
}

assemble_image() {
  ensure_hdd_image || return 0
  if [ "$DO_DESKTOP" = 1 ] && [ -f "$PINECONE_DIR/DESKTOP.EXE" ]; then
    log "staging DESKTOP.EXE onto the disk image…"
    mcopy -i "$HDD_IMG" -o "$PINECONE_DIR/DESKTOP.EXE" ::/DESKTOP.EXE
  fi
  log "assembling bootable $(basename "$USB_IMG")…"
  ( cd "$SRC_DIR"
    "$TOOLS_DIR/build-pure-usb.py" \
      --mbr boot/pcboot/mbr.bin --vbr boot/pcboot/vbr.bin --pcboot boot/pcboot/pcboot.bin \
      --kernel kernel.pure.flat --from-image "$(basename "$HDD_IMG")" \
      --modules-dir modules --out "$(basename "$USB_IMG")" )
  ok "bootable image: $USB_IMG"
}

run_qemu() {
  find_qemu; [ -n "$QEMU" ] || die "qemu not found"
  ensure_hdd_image || die "cannot run without a disk image"
  [ "$DO_DESKTOP" = 1 ] && [ -f "$PINECONE_DIR/DESKTOP.EXE" ] && \
    mcopy -i "$HDD_IMG" -o "$PINECONE_DIR/DESKTOP.EXE" ::/DESKTOP.EXE 2>/dev/null || true
  local disp; [ "$OS" = macos ] && disp=cocoa || disp=gtk
  log "Booting in QEMU ($disp). At the 'pine:C:/\$' prompt, type:  desktop"
  log "Serial trace → /tmp/pinecore-serial.log"
  ( cd "$SRC_DIR"
    exec "$QEMU" -kernel kernel.pure.bin \
      -drive file="$(basename "$HDD_IMG")",format=raw,if=ide,index=0 \
      -serial file:/tmp/pinecore-serial.log -display "$disp" -m 32 )
}

clean() {
  log "Cleaning build artifacts (committed disk image preserved)…"
  ( cd "$SRC_DIR"
    rm -f kernel.dos.bin kernel.pure.bin kernel.dos.flat kernel.pure.flat kernel.flat
    rm -f boot/pcboot/*.bin boot/pine.com modules/*.kmd pinecore-pure-hdd.img
    rm -rf build/ )
  rm -f "$PINECONE_DIR/DESKTOP.EXE"; rm -rf "$PINECONE_DIR/build"
  ok "cleaned"
}

# ---------------------------------------------------------------------------
# Toolchain status report
# ---------------------------------------------------------------------------
report() {
  echo
  printf '%sToolchain status%s\n' "$C_B" "$C_0"
  find_qemu
  local rows=(
    "nasm|$(command -v nasm || echo MISSING)"
    "mtools (mcopy)|$(command -v mcopy || echo MISSING)"
    "qemu|${QEMU:-MISSING}"
    "python3|$(command -v python3 || echo MISSING)"
    "${CROSS_TARGET}-gcc|$(find_cross && echo "$CROSS_BIN/${CROSS_TARGET}-gcc" || echo MISSING)"
    "djgpp gcc|$(find_djgpp && echo "$DJGPP_BIN/i586-pc-msdosdjgpp-gcc" || echo MISSING)"
    "Allegro 4.2|$(allegro_present && echo "present in $DJGPP_PATH" || echo MISSING)"
  )
  for r in "${rows[@]}"; do printf '  %-18s %s\n' "${r%%|*}" "${r##*|}"; done
  echo
}

# ---------------------------------------------------------------------------
# Args
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --run)        DO_RUN=1 ;;
    --check)      CHECK_ONLY=1 ;;
    --no-install) DO_INSTALL=0 ;;
    --no-desktop) DO_DESKTOP=0 ;;
    --clean)      DO_CLEAN=1; DO_BUILD=0 ;;
    -h|--help)    sed -n '2,40p' "$0"; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac; shift
done

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
detect_os
[ "$DO_CLEAN" = 1 ] && { clean; exit 0; }
if [ "$CHECK_ONLY" = 1 ]; then report; exit 0; fi

ensure_base_tools
ensure_cross
[ "$DO_DESKTOP" = 1 ] && ensure_djgpp || true
report

build_kernel
[ "$DO_DESKTOP" = 1 ] && build_desktop || warn "skipping DESKTOP.EXE (--no-desktop)"
assemble_image

echo
ok "Build complete."
[ -f "$USB_IMG" ]            && echo "  bootable image : $USB_IMG"
echo "  kernel         : $SRC_DIR/kernel.pure.bin"
[ "$DO_DESKTOP" = 1 ] && [ -f "$PINECONE_DIR/DESKTOP.EXE" ] && echo "  desktop client : $PINECONE_DIR/DESKTOP.EXE"
echo
if [ "$DO_RUN" = 1 ]; then run_qemu; else echo "Run it with:  ./build.sh --run"; fi
