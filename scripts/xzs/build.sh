#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Build the Xperia XZs (tone/keyaki) Linux 5.4 kernel.
# Output: $OUT/arch/arm64/boot/Image.gz-dtb (kernel + appended keyaki DTBs)
#
# Environment:
#   OUT            build directory          (default: <kernel>/out)
#   CROSS_COMPILE  aarch64 GCC prefix       (default: aarch64-linux-gnu-)
#   JOBS           parallel jobs            (default: nproc)
set -euo pipefail

KDIR=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$KDIR/out}
JOBS=${JOBS:-$(nproc)}
export ARCH=arm64
export CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-}

make -C "$KDIR" O="$OUT" aosp_tone_keyaki_defconfig
make -C "$KDIR" O="$OUT" olddefconfig
make -C "$KDIR" O="$OUT" -j"$JOBS" Image.gz-dtb

echo "kernel: $OUT/arch/arm64/boot/Image.gz-dtb"
