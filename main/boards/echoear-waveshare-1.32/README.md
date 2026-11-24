# EchoEar · Waveshare ESP32-S3-Touch-AMOLED-1.32

Target: Waveshare ESP32-S3-Touch-AMOLED-1.32 (not the 1.43C / 1.75 / ESP32-C6 variants).

## Hardware baseline

The pin definitions in `config.h` are copied from the official Xiaozhi board definition:

- ESP32-S3-PICO-1, 8 MB Flash, 8 MB PSRAM
- 466 × 466 AMOLED, CO5300 QSPI (RGB565)
- CST820 capacitive touch, I2C 0x15
- ES8311 codec, single onboard microphone and speaker output
- BOOT: GPIO0; power key: GPIO17; power enable: GPIO18
- LCD: CS10, PCLK11, D0..D3 = 12..15, RESET8
- Audio I2S: MCLK38, WS41, BCLK39, DIN40, DOUT42
- Codec / touch I2C: SDA47, SCL48; CST820 address 0x15, reset GPIO7, interrupt GPIO6
- PA enable GPIO46; 8 MB Flash uses the repository's `partitions/v2/8m.csv` layout (2 MB assets partition)

The 1.32 board does not expose the 1.75-B board's dual-microphone / ES7210 audio path, SD interface, or validated AEC reference channel. Those capabilities are explicitly disabled in the EchoEar target profile.

## Build (Windows PowerShell / ESP-IDF 6.1)

Open an ESP-IDF 6.1 terminal at the repository root, then initialize the pinned core:

```powershell
git submodule update --init --recursive
python -m unittest discover -s scripts/tests -v
python scripts/build.py echoear-waveshare-1.32 --name echoear-waveshare-1.32 --language zh-CN
```

The board variant selects the ESP32-S3 target, 8 MB flash size, the 8 MB partition table, and the shared ESP32-S3 OPI-PSRAM defaults. Do not copy the 1.75-B board's audio/AEC or SD-card settings into this target.

The build script manages the target and sdkconfig configuration. After building, inspect `sdkconfig` and `build/flasher_args.json` before flashing. Confirm the effective flash size is 8 MB, the partition table is `partitions/v2/8m.csv`, and the assets image fits the 2 MB assets partition.

## Port status

The initial board driver ports the official 1.32 CO5300 QSPI panel initialization, CST820 I2C touch, power-key sequencing, BOOT key, CO5300 DCS brightness command and ES8311 audio initialization into an EchoEar-specific board identity. It is still a work-in-progress and is not yet a flash-ready release:
- Touch coordinate orientation and gesture behavior need physical verification.
- The 8 MB partition and bundled EchoEar assets need a size/build check.
- Screen rotation, display offsets, touch mapping, audio capture/playback, wake word, interruption, power key and sleep/wake need hardware acceptance.
- Do not flash a package until the board driver and partition/asset size checks are complete.

The shared EchoEar UI and applications are provided by the pinned `external/echoear-core` submodule.
