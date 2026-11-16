# EchoEar · Waveshare ESP32-S3-Touch-AMOLED-1.32

Target: Waveshare ESP32-S3-Touch-AMOLED-1.32 (not the 1.43C / 1.75 / ESP32-C6 variants).

## Hardware baseline

The pin definitions in `config.h` are copied from the official Xiaozhi board definition:

- ESP32-S3-PICO-1, 8 MB Flash, 8 MB PSRAM
- 466 × 466 AMOLED, CO5300 QSPI
- CST820 capacitive touch
- ES8311 codec, single onboard microphone and speaker output
- BOOT: GPIO0; power key: GPIO17; power enable: GPIO18
- LCD: CS10, PCLK11, D0..D3 = 12..15, RESET8
- Audio I2S: MCLK38, WS41, BCLK39, DIN40, DOUT42
- Codec I2C: SDA47, SCL48; PA enable GPIO46

The 1.32 board does not expose the 1.75-B board's dual-microphone / ES7210 audio path, SD interface, or validated AEC reference channel. Those capabilities are explicitly disabled in the EchoEar target profile.

## Port status

This directory is the independent board identity and configuration layer. It is not yet a flash-ready port:
- The board-specific Board / display / touch / audio initialization still needs to be ported against the official 1.32 implementation.
- Touch controller initialization and coordinate transform require device verification.
- Screen rotation, display offsets, audio capture/playback, wake word, interruption, power key and sleep/wake need hardware acceptance.
- Do not flash a package until the board driver and partition/asset size checks are complete.

The shared EchoEar UI and applications are provided by the pinned `external/echoear-core` submodule.
