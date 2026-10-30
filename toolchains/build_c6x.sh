#!/bin/sh
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
# Builds a C compiler for the audio processor (TI C6000 family, target
# tic6x-elf: GCC 14.2.0 and binutils 2.43, C only, bare metal) into
# toolchains/installed/c6x, which git ignores.
# The firmware build needs it for the release, whose FLAC decoding runs
# on the audio processor (tools/chip_flac_block.py); the build without it
# (--everyday) does not. The source archives are checked against the GNU signing
# keys. configure does not work in a path with spaces, so sources, build
# and install go to a temporary folder (XDJ_BUILD_TMP, or one made under
# TMPDIR); the finished tools are copied over and the folder is deleted.
# GCC finds its own parts relative to where it is run from, so the copy
# works. Takes about 20 minutes.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
P="${XDJ_TOOLCHAIN:-$ROOT/toolchains/installed}"
T="${XDJ_BUILD_TMP:-$(mktemp -d "${TMPDIR:-/tmp}/xdj-c6x-gcc.XXXXXX")}"
case "$T" in *" "*) echo "build folder must not contain spaces: $T" >&2; exit 2;; esac
J=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
I="$T/install"
mkdir -p "$T/source"
cd "$T/source"
for f in gcc/gcc-14.2.0/gcc-14.2.0.tar.xz binutils/binutils-2.43.tar.xz; do
    name=$(basename "$f")
    [ -f "$name" ] || curl -fsSLO "https://ftp.gnu.org/gnu/$f"
    [ -f "$name.sig" ] || curl -fsSLO "https://ftp.gnu.org/gnu/$f.sig"
done
[ -f gnu-keyring.gpg ] || curl -fsSLO https://ftp.gnu.org/gnu/gnu-keyring.gpg
G=$(mktemp -d)
gpg --homedir "$G" --import gnu-keyring.gpg >/dev/null 2>&1
gpg --homedir "$G" --verify gcc-14.2.0.tar.xz.sig gcc-14.2.0.tar.xz
gpg --homedir "$G" --verify binutils-2.43.tar.xz.sig binutils-2.43.tar.xz
rm -rf "$G"
[ -d binutils-2.43 ] || tar xf binutils-2.43.tar.xz
[ -d gcc-14.2.0 ] || tar xf gcc-14.2.0.tar.xz
(cd gcc-14.2.0 && [ -e gmp ] || ./contrib/download_prerequisites)

mkdir -p "$T/build-binutils" && cd "$T/build-binutils"
../source/binutils-2.43/configure --target=tic6x-elf --prefix="$I" \
    --disable-nls --disable-werror --with-system-zlib \
    --disable-gprof --disable-gdb --disable-sim
# MAKEINFO=true: skip the manuals (makeinfo is usually not installed).
make -j"$J" MAKEINFO=true
make install MAKEINFO=true

export PATH="$I/bin:$PATH"
mkdir -p "$T/build-gcc" && cd "$T/build-gcc"
../source/gcc-14.2.0/configure --target=tic6x-elf --prefix="$I" \
    --disable-nls --enable-languages=c --without-headers --with-newlib \
    --disable-libssp --disable-libgomp --disable-libquadmath \
    --disable-libatomic --disable-threads --disable-shared --with-system-zlib
make -j"$J" all-gcc all-target-libgcc MAKEINFO=true
make install-gcc install-target-libgcc MAKEINFO=true

mkdir -p "$P/c6x"
cp -R "$I/." "$P/c6x/"
cd "$ROOT"
rm -rf "$T"
echo "compiler ready: $P/c6x/bin/tic6x-elf-gcc"
