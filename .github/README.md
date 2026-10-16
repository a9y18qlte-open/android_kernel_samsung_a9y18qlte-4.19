# Linux 4.19 for the Samsung Galaxy A9 (2018)

Kernel for the Samsung Galaxy A9 (2018), `a9y18qlte` (SM-A920F, Snapdragon 660),
on top of LineageOS' [android_kernel_qcom_sdm660](https://github.com/LineageOS/android_kernel_qcom_sdm660)
(`lineage-20`, Linux 4.19.325). It replaces Samsung's 4.4 kernel for LineageOS 22.2
(Android 15) and runs the device's Android 10 era vendor blobs (A920FXXS7CVL2).

The Samsung parts come from Samsung's A920FXXS7CVL2 kernel source and are ported
to 4.19. The upstream kernel README is [`README`](../README).

## Status

Working on LineageOS 22.2:

- Display (S6E3FA7 AMOLED, mDNIe, AOD), touch (Zinitix ZT7548, double tap to wake)
- Audio: speaker (TFA9872), earpiece, microphones, headset jack, calls including
  speakerphone, VoIP calls (Meet, WhatsApp)
- Camera: all five cameras, flash and torch, photo and video
- Video decoding and encoding (Venus), including VP9 up to 1440p60
- Wi-Fi, Wi-Fi hotspot, Bluetooth, NFC (PN547)
- Mobile data (IPA), Wi-Fi Calling
- Fingerprint (ET5xx), grip, hall sensor, vibrator, sensors on the ADSP
- USB-C (S2MM005), USB tethering, charging and battery (SM5705), thermistors
- File-based encryption on the UFS inline crypto engine (ICE)
- Samsung `sec_debug`: restart reasons, `/proc/last_kmsg`, crash logs in ramoops

## Branches

- `lineage-22.2-4.19`: the kernel.
- `lineage-22.2-4.19-ksu`: the same kernel with [KernelSU-Next](https://github.com/KernelSU-Next/KernelSU-Next)
  (`v3.4.0-legacy`, the branch for non-GKI kernels) with manual hooks. KernelSU-Next
  is a submodule, so clone with `--recurse-submodules`. Its syscall table mode
  cannot start `ksud` on this kernel.

## Building

The A9 options are a fragment on top of LineageOS' SDM660 defconfig:

```
make ARCH=arm64 O=out vendor/sdm660-perf_defconfig vendor/a9y18qlte.config
make ARCH=arm64 O=out
```

In an Android device tree:

```
TARGET_KERNEL_CONFIG := vendor/sdm660-perf_defconfig
TARGET_KERNEL_ADDITIONAL_CONFIG := vendor/a9y18qlte.config
```

The output is `arch/arm64/boot/Image.gz-dtb`: the kernel with the A9 device tree
(`sdm660-sec-a9y18qlte-eur-r06`) appended, as Samsung's bootloader expects.

## ROM requirements

Compared with a ROM built for Samsung's 4.4 kernel:

- **No `swiotlb=1`** on the kernel command line. arm64 4.19 has no ZONE_DMA below
  4 GiB, so the IPA's 32-bit DMA buffers bounce through swiotlb. With `swiotlb=1`
  every mapping fails and the IPA hangs at boot. Leave swiotlb at its default size.
- **`inlinecrypt` on `/data`** in the fstab. The ROM's "ice" file-based
  encryption is done by the UFS inline crypto engine, which 4.19 only uses for
  file systems mounted with `inlinecrypt`. Without it, encrypted files read back
  as ciphertext. The 4.4 kernel rejects the option, so each kernel needs its own
  fstab.
- **4.19 sysfs paths in `genfs_contexts`.** 4.19 places platform devices under
  `/sys/devices/platform/...` instead of `/sys/devices/soc/...`, and the subsystem
  numbering differs (`subsys0` modem, `subsys1` venus, `subsys2` adsp, `subsys3`
  cdsp, `subsys4` kgsl). Rules written for 4.4 paths miss, and services such as
  the health HAL are then denied access (no battery percentage).

## What was ported from Samsung

From Samsung's A920FXXS7CVL2 kernel, adapted to 4.19:

- **Board:** the A9 device tree, board options, Samsung input key and switch codes
- **Debug:** `sec_debug` (restart reasons, upload cause, `sec_log` and
  `/proc/last_kmsg`, debug summary, power log, PWDT), `sec_bsp`, Samsung hooks in
  arm64, the scheduler, watchdog, msm-poweroff, PIL, clocks and the IOMMU
- **Display:** Samsung MDSS panel drivers (S6E3FA7), mDNIe, self display, AOD
- **Touch:** Samsung input core (`sec_cmd`, `sec_tclm`), Zinitix ZT75xx with
  built-in firmware, S2DOS03 display/touch PMIC
- **USB and power:** S2MM005 USB-C, SM5705 MUIC, charger and fuel gauge,
  `battery_v2`, `usb_notify`, KTS1620 GPIO expander, LPM charging mode
- **Audio:** NXP TFA9872 speaker amplifier, Samsung q6audio adaptation (TFA DSP
  transport), machine driver links, codec tuning, `sec_jack`
- **Camera:** Samsung `camera_v2` changes, sensor, EEPROM and actuator drivers,
  SM5705 flash LED
- **Connectivity:** Samsung qcacld-3.0 features (MAC loader, `/sys/wifi`, SAR
  backoff, `swlan0` hotspot interface), PN547 NFC
- **Sensors and others:** A96T3X6 grip sensor, hall sensor, `sec_key`,
  thermistors, ET5xx fingerprint (TrustZone-owned SPI bus), vibrator
  (`timed_output`), microSD tray and power control

## 4.19-specific problems and fixes

Problems that only appear on 4.19 with this device's firmware and vendor blobs:

- **Boot:** Samsung's bootloader loads the kernel 0x6c000 off a 2 MiB boundary.
  arm64 shifts the kernel's virtual address by the same amount, so `init_stack`
  is only 16 KiB aligned and the `VMAP_STACK` overflow check fires on the first
  exception. `VMAP_STACK` is off, as it always was on 4.4.
- **Boot:** two SPI buses hang the SoC when probed. `spi_7` is enabled by the
  qcom audio DT but is `i2c_7` (the TFA9872) on the A9, and `spi_3` belongs to
  TrustZone (fingerprint). Both are disabled in the board DT.
- **Restart:** CAF's UFS power down sequence asserts the device reset (RST_N)
  in the shutdown path and leaves it asserted. The TLMM keeps the pin across a
  warm reset, so after `reboot recovery` Samsung's bootloader found the UFS
  device still in reset, could not read it and fell into the Qualcomm crash
  dump mode (black screen, `QUSB__BULK` on the PC, only a battery disconnect
  helped). RST_N is now only asserted when powering off. Warm resets, which
  keep DRAM and the ramoops log, are used for panics, crash dump mode, `edl` and
  recovery. Every other reboot hard resets, as on the stock kernel. The restart
  reason is kept in a PMIC register that survives either reset. Reboots with
  other commands, such as `reboot system`, set the normal boot reason there, so
  a recovery reason left from an earlier reboot does not apply to them.
- **Encryption:** Samsung's TrustZone predates the "CE type" ICE key calls of
  4.19's `crypto-qti-tz` and rejects them. `QTI_CRYPTO_TZ_LEGACY_ICE_KEY` uses
  the older `TZ_ES_SET_ICE_KEY` and `TZ_ES_INVALIDATE_ICE_KEY` calls of the 4.4
  PFK. `ENABLE_LEGACY_PFK` keeps the 4.4 "ice" key derivation, so data written by
  either kernel is readable by the other.
- **Legacy ION:** the 4.4 era blobs (keymaster, gatekeeper, `qseecomd`, camera)
  use the legacy ION API. `ION_LEGACY` is on, plus the legacy msm cache
  maintenance ioctls and per-handle fds.
- **Camera:** `msm_camera_regulator_set_mode()` set the normal mode in both
  branches after CAF's 4.14 to 4.19 port, so the CPP power domain never went to
  hardware-trigger mode. The CPP core was then never reset between frames, its
  firmware lost track of the frame messages, and the camera froze after a few
  frames. Also: camera SMMU banks on the 4.4 IOVA window, `cam_smmu` mappings
  matched by dma-buf, 4.4 frame request and buffer done handling in the ISP, and
  4.4 media entity and sensor info ABIs for the stock camera blobs.
- **Video:** the decoder's load check counted the new session's size once per
  running session. Sessions over the hardware load limit are kept, as on 4.4,
  instead of failing with `-EBUSY`. Players start the new decoder while the old
  one still runs when they switch quality, and YouTube fell back to 480p when
  going to 1440p60 VP9.
- **Audio:**
  - The AFE topology deregister step failed on this ADSP, which cannot report
    its version, so the TFA9872 ports never got their NXP topologies.
  - Samsung's `TERT_MI2S_RX_VI_FB_MUX` control was missing, so the amplifier's
    feedback path was never configured. The ADSP then refused to start the
    feedback port, and the speaker was faint in calls.
  - The ADSP audio domain is tracked through the SSR service, and the SlimBus
    NGD comes up when its QMI service appears.
  - The VoIP front end only took exactly 10 periods. The stock `compress_voip`
    HAL asks for fewer, so its VoIP PCM failed to open and VoIP apps had no
    audio. It takes 2 to 10 periods again, as on 4.4.
- **Thermal:** the XO thermistor's zone keeps its 4.4 name `xo_therm`. The QTI
  thermal HAL takes `xo-therm-adc` as its skin sensor with a 40 °C limit and
  reported severe throttling in normal use.
- **Modem and ADSP:** on some boots the ADSP stops answering right after it
  starts, which leaves the modem without its EFS until it crashes. An ADSP that
  stays silent at probe is restarted once. Modem and ADSP crashes restart the
  subsystem alone instead of the whole SoC.
- **Bluetooth:** the Bluetooth HAL uses the 4.4 `TIOCPM*` ioctl numbers for the
  UART clock votes. `msm_serial_hs` accepts them.
- **Mobile data:** Samsung's RIL sets up `rmnet_data` links, so `RMNET_DATA` is
  used instead of the upstream rmnet driver.
- **FastRPC:** the userspace of this era opens the non-secure device node.
- **Partitions:** the odm fstab entry is disabled. The stock odm partition
  carries Samsung's carrier and IMS configurations.

## Known issues

- **Reboot SError:** some reboots end in an SError during shutdown. Two
  fingerprint HAL threads power the sensor and call TrustZone while init tears
  down. It only happens during shutdown and does not affect the next boot.
- **ADSP stall:** why the ADSP occasionally stops right after boot is not
  known. The restart at probe recovers it.
