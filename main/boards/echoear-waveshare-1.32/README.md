# EchoEar · Waveshare ESP32-S3-Touch-AMOLED-1.32

Target: Waveshare ESP32-S3-Touch-AMOLED-1.32 (not the 1.43C / 1.75 / ESP32-C6 variants).

## Hardware baseline

The pin definitions in `config.h` are copied from the official Xiaozhi board definition:

- ESP32-S3-PICO-1, 8 MB Flash, 8 MB PSRAM
- 466 × 466 AMOLED, SH8601 QSPI (RGB565)
- CST820 capacitive touch hardware (firmware input driver is not yet wired)
- ES8311 codec, single onboard microphone and speaker output
- BOOT: GPIO0; power key: GPIO17; power enable: GPIO18
- LCD: CS10, PCLK11, D0..D3 = 12..15, RESET8
- Audio I2S: MCLK38, WS41, BCLK39, DIN40, DOUT42
- Codec I2C: SDA47, SCL48; PA enable GPIO46

The 1.32 board does not expose the 1.75-B board's dual-microphone / ES7210 audio path, SD interface, or validated AEC reference channel. Those capabilities are explicitly disabled in the EchoEar target profile.

## Port status

The initial board driver now ports the official 1.32 SH8601 QSPI display, power-key sequencing, BOOT key and ES8311 audio initialization into an EchoEar-specific board identity. It is still a work-in-progress and is not yet a flash-ready release:
- CST820 touch initialization and coordinate transform are not implemented yet.
- Runtime brightness control is not yet connected to the EchoEar host backlight interface.
- The build, partition layout, audio capture/playback and wake-word path still need a complete compile and hardware verification.
- Screen rotation, display offsets, audio capture/playback, wake word, interruption, power key and sleep/wake need hardware acceptance.
- Do not flash a package until the board driver and partition/asset size checks are complete.

The shared EchoEar UI and applications are provided by the pinned `external/echoear-core` submodule.
