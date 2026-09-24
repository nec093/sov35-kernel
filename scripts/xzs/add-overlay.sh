#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Add scripts/xzs/ramdisk/xz_rpmb.rc to a Magisk-patched boot ramdisk as
# overlay.d/xz_rpmb.rc (Magisk's init injects overlay.d/*.rc).
#
# usage: add-overlay.sh <ramdisk.cpio.gz from your Magisk-patched boot.img> <out>
#
# Extract the ramdisk from your own device's boot image first, e.g. with
# AOSP unpack_bootimg.py: unpack_bootimg.py --boot_img boot.img --out unpacked
set -euo pipefail

if [ $# -ne 2 ]; then
	echo "usage: $0 <ramdisk.cpio.gz> <out.cpio.gz>" >&2
	exit 1
fi

HERE=$(cd "$(dirname "$0")" && pwd)
IN=$(realpath "$1")
OUTRD=$(realpath -m "$2")
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"
zcat "$IN" | cpio -idm 2>/dev/null
if [ ! -d overlay.d ]; then
	echo "no overlay.d in the ramdisk: is it Magisk-patched?" >&2
	exit 1
fi
install -m 0644 "$HERE/ramdisk/xz_rpmb.rc" overlay.d/xz_rpmb.rc
find . | sort | cpio -o -H newc -R root:root 2>/dev/null | gzip -9 > "$OUTRD"
echo "ramdisk: $OUTRD"
