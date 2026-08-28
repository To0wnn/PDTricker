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

## LEDs

Normal use: one LED (5V/9V/12V/15V/20V) = current output voltage.

| Pattern | Meaning |
|---|---|
| Solid | Requested voltage confirmed |
| Fast blink | Still negotiating |
| Slow blink | Fault — never confirmed, falls back to last-good voltage after ~5.5s |
| 20V fast blink, alone | Menu entry hold (5-10s) / bootloader hold (10s+) |

In the mode menu (after a 5s hold): 5V/9V/12V = boot mode, not voltage —
see below. 20V blinks slowly the whole time as a "still in menu" indicator.

## Boot modes

What voltage the board requests automatically when you plug it in — button
cycling (click to change voltage) always works the same in every mode, this
only affects the very first moment at power-on.

| Menu LED | Mode | What happens at power-on |
|---|---|---|
| 5V | always-5V | Always boots on 5V, ignores whatever was remembered. |
| 9V | safe-delay | Boots on 5V first, then after 3s steps up to the remembered voltage — gives whatever's plugged in 3 safe seconds before the real voltage hits. The target LED fast-blinks during that 3s so you can see where it's heading. |
| 12V | remembered | Boots straight on the last voltage you had selected. |

Set it: hold the button ~5s to open the menu, click to step through the 3
options (shown one LED at a time), then either hold ~800ms or wait ~2s idle
to save and exit. Or over UART: `MODE0`/`MODE1`/`MODE2`.

## Extra features (beyond the original)

- UART commands: `5V`/`9V`/`12V`/`15V`/`20V`/`?`/`?stats`/`?reset`/`MODE0-2`
  (57600 baud, TX=P3.1, RX=P3.0).
- Click cycles 5V→9V→12V→15V→20V; double-click steps back.
- LED fault feedback: fast blink = negotiating, slow blink = fault. If a
  requested voltage never verifies (charger doesn't support it), waits
  ~5.5s total then falls back to the last voltage that *did* verify — not
  always 5V, so a gap in what's supported (e.g. 9V/15V present but not 12V)
  doesn't unnecessarily drop you all the way down.
- Voltage remembered across power cycles.
- Safe-boot override: hold button <5s at power-on → forces 5V.
