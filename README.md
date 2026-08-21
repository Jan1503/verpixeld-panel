# verpixeld-panel

Firmware for an **ICND1065L HUB320** LED panel driven by a **W6300-EVB-Pico2** (RP2350). Pairs with the [verpixeld](https://github.com/Jan1503/verpixeld) host.

This is a product firmware repo, not a general HUB75 library. Driver work that belongs in [DMD_STM32](https://github.com/board707/DMD_STM32) still lives in the [dmd_stm32 fork](https://github.com/jan1503/dmd_stm32).

The GitHub repository name can be changed later (Settings → General → Rename); old URLs redirect.

## Hardware

- Panel: indoor 256×128 ICND1065L (HUB320, 64-scan, 74HC595 row mux)
- Controller: WIZnet W6300-EVB-Pico2 (RP2350 + W6300 Ethernet)
- Host stream: UDP port 7777 (verpixeld / [PixPlane](https://github.com/Jan1503/pixplane))
- Config UI: `http://<panel-ip>:5000` (OTA on the same page)

## Firmware 1.7

- `livemode 8|14`: live colour/buffer realloc (no save, no reboot). Host uses this when visible canvases change depth.
- UDP v2: fragment deltas + 1 s keyframes (keyframes skip the 224 KB fill-copy; no RX-discard during stall)
- Config UI: live/idle badge, drop-rate tiles, OTA progress; colour/network apply save+reboot in one step
- HTTP GET replies paced (~1 KB per poll) so a page load cannot stall the UDP drain
- Quad QSPI PIO transport for the W6300
- PIO-driven 595 mux (pio2), scan ISR stays on PIO0
- SRAM 5×7 font boot splash (no Adafruit GFX fonts in the upload path)
- 14-bit greyscale / double-buffer, optional 8-bit / triple-buffer (`applymode` = persist + reboot; `livemode` = live)
- OTA: LittleFS staging, FQBN must include an FS partition

## Live 8/14-bit colour depth

The RP2350 has **520 KB SRAM**. Two 14-bit planes are ~448 KB; three 8-bit planes are ~384 KB. The chip cannot hold both layouts, so a depth change must free one buffer set and allocate the other.

| Command | Effect | Persisted? |
|---------|--------|------------|
| `livemode 8\|14` | Reallocate framebuffers, keep streaming after a brief hitch | No (RAM only) |
| `mode 8\|14` then `save` then `reboot` | Same switch as the boot default | Yes |

Firmware 1.7 is what makes `livemode` possible. Older builds only had the reboot path. The host **must stop UDP**, wait until `/status` reports the new `bits`, then reopen the streamer with matching `ColorBits`. Mixing 8-bit and 14-bit pixels in one packed frame is not supported.

verpixeld picks the depth from visible canvases (see that repo). A clock overlay can run 8-bit for fps; a video canvas forces 14-bit for the whole wall. HDMI / GPIO / SPI outputs are unrelated — this is the network panel only.

Serial / web: `livemode 8` or `livemode 14`. The config UI colour control still uses save+reboot so a power cycle comes back in the chosen mode.

## Scan-home seam (hardware)

Four logical columns — **63, 127, 191, 255** — are brighter in the highlights and darker in the shadows than their neighbours.

That is **not four panel joints**. Landscape mapping (`cfg_landscape + cfg_rot_cw + cfg_flip_x + cfg_flip_y`) puts `rowaddr 0` of each 1/64 scan group on those X coordinates. The first line of a 64-scan ICND group has a nonlinear DC / PWM offset. It is the same physical line, shown four times across 256 px.

This firmware does **not** correct it. Chip registers (REG03 and similar) do not invert the S-curve. Extra 595 slots, OE blanks or mux-phase tweaks desynchronise the 595 from the driver and create ghosts. The line cannot be made physically linear in software on the MCU.

The **workaround is on the host**: [PixPlane](https://github.com/Jan1503/pixplane) remaps 8-bit source on those four columns (9-point curve → 256-entry LUT) before packing. verpixeld and DeskCast expose that curve in their UIs. Calibrate with a host grey fill — firmware patterns `t f` / `t g` / `t v` bypass the host packer, so they show the raw artifact (useful for seeing it, useless for dialling the LUT).

<!-- PHOTO seam-hardware: close-up of the four scan-home columns on a grey field -->

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
