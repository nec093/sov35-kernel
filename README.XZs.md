# Linux 5.4 for the Sony Xperia XZs (tone / keyaki, MSM8996)

Branch `port-5.4`: a port of the Sony Open Devices msm-4.9 kernel
(`aosp/LE.UM.2.3.2.r1.4`) to Linux **5.4.302**, used to boot an Android 12
Phh-Treble GSI on the Xperia XZs. Tested on an SOV35 with the stock vendor
partition.

> **Warning.** Only ever *boot* these images (`fastboot boot`). Never flash
> them to the boot partition: if something goes wrong the device simply
> falls back to its installed kernel on the next reboot. A bootloader-unlocked
> device is required.

## Status

| Area | State |
|------|-------|
| Boot to Android 12 GSI UI, touch (clearpad), display, GPU (Adreno 530) | working |
| CPU frequency scaling (schedutil) and thermal throttling | working |
| DDR bandwidth voting (cpubw/memlat devfreq) | working |
| Battery / charger / fuel gauge (PMI8994) | working |
| Wi-Fi (BCM4359 on PCIe, brcmfmac) | working |
| Internal storage (FUSE) and microSD (high-speed mode) | working |
| Camera, rear (IMX400) and front (IMX258) | working |
| USB adb | working (peripheral mode forced from the ramdisk) |
| Video codec (Venus/vidc), SDE rotator | working (H.264 hardware encode verified); the GSI's ueventd does not search `/vendor/firmware_mnt/image`, where the Venus firmware lives, so without that search path the framework falls back to software codecs |
| Audio, ADSP | not yet |
| Sensor hub (SLPI), modem | not yet (also offline on the stock 4.9 kernel here) |
| Fingerprint (FPC1145) | kernel driver probes, the HAL talks to the sensor; enrolment not reachable from the GSI settings |
| Bluetooth, NFC | not yet |
| cpuidle (PSCI: core power collapse, L2 retention and L2 power collapse) | working; the Kryo clusters run in BHS mode only (LDO mode disabled, see Notes) |
| LMH (limits management hardware) | working: sensors, profile, DPM voltage and ODCM are set up; throttling intensity readable from the `lmh-*` thermal zones |

## Build

Tested on Ubuntu 22.04 with its cross toolchain (GCC 11.4):

```sh
sudo apt install gcc-aarch64-linux-gnu make bc bison flex libssl-dev libelf-dev python3 cpio
scripts/xzs/build.sh            # -> out/arch/arm64/boot/Image.gz-dtb
```

`OUT`, `CROSS_COMPILE` and `JOBS` can be overridden from the environment.
The defconfig is `aosp_tone_keyaki_defconfig`; the keyaki DTBs are appended
to the kernel image.

## Boot image

The kernel is combined with the ramdisk of your own device's
**Magisk-patched** boot image (the GSI's first-stage init runs from it), plus a
small init fragment (`scripts/xzs/ramdisk/xz_rpmb.rc`) that works around two
things the 5.4 kernel does differently (RPMB device node, USB mode).

1. Get AOSP mkbootimg (`https://android.googlesource.com/platform/system/tools/mkbootimg`).
2. Extract the ramdisk of your Magisk-patched boot image:
   `unpack_bootimg.py --boot_img magisk_patched.img --out unpacked`
3. Add the fragment:
   `scripts/xzs/add-overlay.sh unpacked/ramdisk ramdisk-xzs.cpio.gz`
4. Build the image:
   `MKBOOTIMG=/path/to/mkbootimg.py scripts/xzs/mkbootimg.sh ramdisk-xzs.cpio.gz boot-xzs-5.4.img`
5. Boot it once (not flash): `fastboot boot boot-xzs-5.4.img`

`adb reboot` returns to the installed kernel. `adb reboot bootloader` from
this kernel does not reach fastboot yet; reboot to the installed kernel first.

## Notes

- Firmware for the modem/ADSP/SLPI lives on the modem partition
  (`/vendor/firmware_mnt/image`). Do not add
  `firmware_class.path=/vendor/firmware_mnt/image`: the modem MBA then fails
  authentication and the device resets.
- Kryo LDO mode is disabled in the DT (`qcom,ldo-disable`). With it enabled,
  cluster (L2) power collapse makes a later CPU voltage transition take a
  whole cluster down (in the TZ recalibration / APM clock-source calls),
  followed by SErrors and a watchdog reset in most boots.
- The commits on `port-5.4` explain each fix (probe-ordering changes for
  late msm_bus/clock providers, CAF API compatibility shims, etc.).

---

# 日本語

Xperia XZs(tone / keyaki、MSM8996)向けに、Sony Open Devices の msm-4.9
カーネルを Linux 5.4.302 へ移植したブランチです。Android 12 の Phh-Treble
GSI を stock の vendor パーティションのまま起動できます(SOV35 で確認)。

- **焼かないでください。** `fastboot boot` での一時起動専用です。問題が起きても
  再起動すれば元のカーネルに戻ります。ブートローダーのアンロックが必要です。
- ビルド: `scripts/xzs/build.sh`(Ubuntu 22.04 の `gcc-aarch64-linux-gnu` で確認)
- ブートイメージ: 各自の端末の **Magisk パッチ済み** boot.img から ramdisk を取り出し、
  `scripts/xzs/add-overlay.sh` で設定を追加してから、`scripts/xzs/mkbootimg.sh` で作成します。
- 動作状況は上の表のとおりです(カメラ・Wi-Fi・電池・CPU クロック制御・cpuidle・
  ストレージ・動画コーデックは動作、音声・センサーハブ・Bluetooth などは未対応、
  指紋はドライバのみ動作)。動画コーデックは ueventd のファームウェア検索パスに
  `/vendor/firmware_mnt/image` が必要です(vendor 側の設定)。
