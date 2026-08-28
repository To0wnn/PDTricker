# PDTricker firmware (SDCC port)

SDCC port of [PDTricker](https://github.com/wuxx/PDTricker) (CH552T USB-PD
trigger board), so it builds on Linux. Based on [ch554_sdcc](https://github.com/Blinkinlabs/ch554_sdcc).

Board: [AliExpress](https://nl.aliexpress.com/item/1005011792383263.html)

## Changes vs. upstream

- Fixed: button pin (P1.0) wasn't configured as pulled-up input → dead button.
- Fixed: `FREQ_SYS=24000000` hangs the chip on boot → use `16000000`.
- Fixed: unconditional debug `printf()` in the main loop blocked the CPU per
  UART char, throwing off all button-hold timing (5s menu, 10s bootloader).

All button gestures (click, 5s hold → menu, 10s hold → bootloader) confirmed
working on hardware.

## Build

```
cd firmware
make FREQ_SYS=16000000 EXTRA_FLAGS='-DUART0_BAUD=57600'
```

Precompiled `.bin`/`.hex` on the [latest release](../../releases/latest).

## Flash

Bridge pin 4 to pin 3 (D+ to 3.3V), then plug in the USB-C cable. Release
after ~1s. Board shows up as `4348:55e0`.

```
wchisp flash pdtricker.bin
```

Once this firmware is already flashed: hold the button, plug in the USB-C
cable, keep holding for 10s — jumps into flash mode too, no bridge needed.

## Layout

- `firmware/main.c` — application
- `include/` — SDCC-compatible CH552 headers + adc/debug helpers

## Extra features (beyond the original)

- UART commands: `5V`/`9V`/`12V`/`15V`/`20V`/`?`/`?stats`/`?reset`/`MODE0-2`
  (57600 baud, TX=P3.1, RX=P3.0).
- Click cycles 5V→9V→12V→15V→20V; double-click steps back.
- LED fault feedback: fast blink = negotiating, slow blink = fault, falls
  back to 5V if a requested voltage never verifies.
- Voltage remembered across power cycles.
- Safe-boot override: hold button <5s at power-on → forces 5V.
- 3 boot modes (always-5V / safe-delay / remembered), set via ~5s hold menu
  or `MODE0-2` UART command.
