#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Pack Image.gz-dtb and a ramdisk into a (header v0) boot image for the
# Xperia XZs. Meant for `fastboot boot <img>` only -- do not flash it.
#
# usage: mkbootimg.sh <ramdisk> [out.img] [extra kernel cmdline...]
#
# Environment:
#   MKBOOTIMG  path to AOSP mkbootimg.py   (default: mkbootimg in $PATH)
#   KERNEL     kernel image                (default: <kernel>/out/arch/arm64/boot/Image.gz-dtb)
set -euo pipefail

if [ $# -lt 1 ]; then
	echo "usage: $0 <ramdisk> [out.img] [extra cmdline...]" >&2
	exit 1
fi

KDIR=$(cd "$(dirname "$0")/../.." && pwd)
RAMDISK=$1
OUTIMG=${2:-boot-xzs-5.4.img}
shift $(( $# >= 2 ? 2 : 1 ))
EXTRA="$*"
KERNEL=${KERNEL:-$KDIR/out/arch/arm64/boot/Image.gz-dtb}
MKBOOTIMG=${MKBOOTIMG:-mkbootimg}

# log_buf_len: early boot messages otherwise rotate out of dmesg within
# ~10 s. androidboot.selinux=permissive: the GSI/vendor policy still has
# denials against this kernel.
CMDLINE="androidboot.bootdevice=7464900.sdhci androidboot.selinux=permissive"
CMDLINE+=" msm_rtb.filter=0x3F ehci-hcd.park=3 coherent_pool=8M"
CMDLINE+=" sched_enable_power_aware=1 user_debug=31 cgroup.memory=nokmem"
CMDLINE+=" printk.devkmsg=on kpti=0 androidboot.hardware=keyaki"
CMDLINE+=" buildvariant=userdebug log_buf_len=4M"

"$MKBOOTIMG" --kernel "$KERNEL" --ramdisk "$RAMDISK" \
	--header_version 0 --base 0x80000000 --kernel_offset 0x8000 \
	--ramdisk_offset 0x2000000 --tags_offset 0x1e00000 --pagesize 4096 \
	--os_version 10.0.0 --os_patch_level 2020-03 \
	--cmdline "$CMDLINE $EXTRA" -o "$OUTIMG"

echo "boot image: $OUTIMG"
