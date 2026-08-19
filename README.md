# verpixeld-panel

Firmware for an **ICND1065L HUB320** LED panel driven by a **W6300-EVB-Pico2** (RP2350). Pairs with the [verpixeld](https://github.com/jan1503) host.

This is a product firmware repo, not a general HUB75 library. Driver work that belongs in [DMD_STM32](https://github.com/board707/DMD_STM32) still lives in the [dmd_stm32 fork](https://github.com/jan1503/dmd_stm32).

The GitHub repository name can be changed later (Settings → General → Rename); old URLs redirect.

## Hardware

- Panel: indoor 256×128 ICND1065L (HUB320, 64-scan, 74HC595 row mux)
- Controller: WIZnet W6300-EVB-Pico2 (RP2350 + W6300 Ethernet)
- Host stream: UDP port 7777 (verpixeld / [PixPlane](https://github.com/Jan1503/pixplane))
- Config UI: `http://<panel-ip>:5000` (OTA on the same page)

## Firmware 1.3

- Quad QSPI PIO transport for the W6300
- PIO-driven 595 mux (pio2), scan ISR stays on PIO0
- SRAM 5×7 font boot splash (no Adafruit GFX fonts in the upload path)
- 14-bit greyscale / double-buffer, optional 8-bit / triple-buffer (save + reboot)
- OTA: LittleFS staging, FQBN must include an FS partition

The steady brightness offset of the seam columns (63/64, 127/128, 191/192) is a chip/panel artifact. It is not fixed in software.

## Build

Arduino CLI, Pico 2 core, **4 MB flash with 1 MB FS** (OTA will not work without the FS slice):

```bash
arduino-cli compile --fqbn rp2040:rp2040:rpipico2:flash=4194304_1048576 \
  --library third_party/DMD_STM32 \
  --output-dir _build \
  firmware
```

Flash by dropping `_build/dmd_icnd1065l_w6300_pioq.ino.bin` on the panel web UI. Do not use `--upload` unless you intend to replace the running image over USB.

## Layout

```
firmware/                 Arduino sketch (single translation unit)
third_party/DMD_STM32/    Vendored RP/SPWM library snapshot (GPL-3.0)
```

## License

GPL-3.0. The panel driver is derived from [DMD_STM32](https://github.com/board707/DMD_STM32) by Dmitry Dmitriev (board707). See `LICENSE`.
