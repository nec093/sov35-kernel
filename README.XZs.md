# sov35-kernel — Linux 5.4 for the Sony Xperia XZs (SOV35, tone / keyaki, MSM8996)

A port of the Sony Open Devices msm-4.9 kernel (`aosp/LE.UM.2.3.2.r1.4`) to
Linux **5.4.302**, plus the CAF audio-kernel (`LA.UM.9.14.1`) for the ADSP /
WCD9335 / WSA881x audio stack. It is the kernel half of a setup that runs
Android 12+ GSIs on the Xperia XZs:

- kernel: this repository (`main`)
- vendor: [sov35-vendor](https://github.com/nec093/sov35-vendor), a
  LineageOS 18.1 Treble vendor flashed to the `oem` partition. It carries
  prebuilts of this kernel and its modules.
- GSI-specific fixes: [sov35-magisk-modules](https://github.com/nec093/sov35-magisk-modules)

Tested on an SOV35 (au) with phh AOSP 12.1.

## Status (2026-10-01, with sov35-vendor)

| Area | State |
|------|-------|
| Display, touch (clearpad), GPU (Adreno 530) | working |
| CPU frequency (schedutil), cpuidle (core PC, L2 retention, L2 power collapse), LMH, thermal | working (Kryo clusters in BHS mode only, see Notes) |
| DDR bandwidth voting (cpubw / memlat / CBF devfreq) | working |
| Battery, charger, fuel gauge (PMI8994) | working |
| Wi-Fi (BCM4359 on PCIe, brcmfmac) | working |
| Internal storage, microSD, USB adb | working |
| Audio: speakers (stereo WSA881x over SoundWire), headphones (up to 192 kHz / 24-bit on `SLIMBUS_6_RX`), microphones | working |
| Video codec (Venus), SDE rotator | working |
| Sensor hub (SLPI) | working |
| Camera | kernel side probes (IMX400 / IMX258); the camera HAL blobs in the vendor do not match yet |
| Modem | not working (TrustZone rejects the modem image) |
| Bluetooth | power-up not verified |
| NFC (CXD224X) | driver and HAL start; tag reading not verified |
| Fingerprint (FPC1145) | driver probes; enrolment not reachable from the GSI |

## Build

Tested on Ubuntu 22.04 with its cross toolchain (GCC 11.4):

```sh
sudo apt install gcc-aarch64-linux-gnu make bc bison flex libssl-dev libelf-dev python3 cpio
scripts/xzs/build.sh     # -> out/arch/arm64/boot/Image.gz-dtb + out/techpack/**/*_dlkm.ko
```

`OUT`, `CROSS_COMPILE` and `JOBS` can be overridden from the environment.
The defconfig is `aosp_tone_keyaki_defconfig`; the keyaki DTBs are appended
to the kernel image. The audio stack is built as modules
(`techpack/audio`, `*_dlkm.ko`) and installed in the vendor image. Copy a
build into sov35-vendor with its `scripts/update-kernel.sh`.

## Boot image

The kernel is combined with the ramdisk of your own device's
**Magisk-patched** boot image (the GSI's first-stage init runs from it), plus a
small init fragment (`scripts/xzs/ramdisk/xz_rpmb.rc`) for two things the 5.4
kernel does differently (the RPMB device node, and the USB mode).

1. Get AOSP mkbootimg (`https://android.googlesource.com/platform/system/tools/mkbootimg`).
2. Extract the ramdisk of your Magisk-patched boot image:
   `unpack_bootimg.py --boot_img magisk_patched.img --out unpacked`
3. Add the fragment:
   `scripts/xzs/add-overlay.sh unpacked/ramdisk ramdisk-xzs.cpio.gz`
4. Build the image:
   `MKBOOTIMG=/path/to/mkbootimg.py scripts/xzs/mkbootimg.sh ramdisk-xzs.cpio.gz boot-xzs-5.4.img`
5. With sov35-vendor installed on `oem`: `fastboot flash boot boot-xzs-5.4.img`.
   To try the kernel without flashing it, use `fastboot boot boot-xzs-5.4.img`
   instead.

## Notes

- Firmware for the modem/ADSP/SLPI lives on the modem partition
  (`/vendor/firmware_mnt/image`). Do not add
  `firmware_class.path=/vendor/firmware_mnt/image`: the modem MBA then fails
  authentication and the device resets.
- Kryo LDO mode is disabled in the DT (`qcom,ldo-disable`). With it enabled,
  cluster (L2) power collapse makes a later CPU voltage transition take a
  whole cluster down (in the TZ recalibration / APM clock-source calls),
  followed by SErrors and a watchdog reset in most boots.
- History: the root commit is the v5.4.302 tree, squashed (upstream history
  is at git.kernel.org). Every Sony/CAF/Android adaptation and fix follows as
  its own commit, with the reasoning in the commit message.

---

# 日本語

Xperia XZs（SOV35、tone / keyaki、MSM8996）向けに、Sony Open Devices の msm-4.9 カーネルを Linux 5.4.302 へ移植したカーネルです。
[sov35-vendor](https://github.com/nec093/sov35-vendor)（LineageOS 18.1 の Treble vendor、`oem` パーティションに書き込み）と組み合わせて、Android 12 以降の GSI を動かします。
GSI ごとの修正は [sov35-magisk-modules](https://github.com/nec093/sov35-magisk-modules) にあります。

- **ビルド**：`scripts/xzs/build.sh` を実行します（Ubuntu 22.04 の `gcc-aarch64-linux-gnu` で確認）。
  カーネル本体と、音声のモジュール（`*_dlkm.ko`）ができます。
  モジュールは vendor に入れるものなので、sov35-vendor の `scripts/update-kernel.sh` でコピーします。
- **ブートイメージ**：各自の端末の **Magisk パッチ済み** boot.img から ramdisk を取り出します。
  `scripts/xzs/add-overlay.sh` で設定を追加し、`scripts/xzs/mkbootimg.sh` で作成します。
  sov35-vendor を入れた状態であれば `fastboot flash boot` で書き込めます（試すだけなら `fastboot boot`）。
- **動作状況**は上の表のとおりです。カメラ（HAL のブロブが未対応）、モデム、Bluetooth の電源投入、NFC のタグ読み取りは、未対応または未確認です。
- Kryo の LDO モードは DT で無効にしています（`qcom,ldo-disable`）。
  有効のままだと、L2 の電源断を使ったあとの CPU 電圧遷移でクラスタごと停止し、多くの起動で SError と watchdog リセットに至ります。
