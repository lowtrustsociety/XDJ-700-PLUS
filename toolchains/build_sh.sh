#!/bin/sh
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
# Builds the SH-4 cross toolchain used by the firmware tools (GCC 14.2.0,
# binutils 2.43, C only, newlib-style bare metal) into toolchains/installed,
# which git ignores. The source archives are
# checked against the GNU signing keys; the sources and build folders go to
# toolchains/build and are deleted at the end. Takes about 20 minutes.
# The firmware build is deterministic, so a fresh toolchain must reproduce
# the pinned build hash (docs/HOW_IT_WORKS.md).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
P="${XDJ_TOOLCHAIN:-$ROOT/toolchains/installed}"
T="$ROOT/toolchains/build"
J=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
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
../source/binutils-2.43/configure --target=sh-elf --prefix="$P" \
    --disable-nls --disable-werror --with-system-zlib
# MAKEINFO=true: skip the manuals (makeinfo is usually not installed).
make -j"$J" MAKEINFO=true
make install MAKEINFO=true

export PATH="$P/bin:$PATH"
mkdir -p "$T/build-gcc" && cd "$T/build-gcc"
../source/gcc-14.2.0/configure --target=sh-elf --prefix="$P" \
    --disable-nls --enable-languages=c --without-headers --with-newlib \
    --disable-libssp --disable-libgomp --disable-libquadmath \
    --disable-libatomic --disable-threads --disable-shared --with-system-zlib
make -j"$J" all-gcc all-target-libgcc MAKEINFO=true
make install-gcc install-target-libgcc MAKEINFO=true
rm -rf "$T"
echo "toolchain ready: $P/bin/sh-elf-"
