# PDTricker firmware (SDCC port)

Firmware for the [PDTricker](https://github.com/wuxx/PDTricker) USB-PD
trigger board (CH552T MCU), ported from Keil C51 to SDCC so it builds on
Linux. Based on the [ch554_sdcc](https://github.com/Blinkinlabs/ch554_sdcc)
template.

## Changes vs. upstream

Besides the SDCC port itself and the [extra features](#extra-features-beyond-the-original)
listed below, three hardware/firmware bugs were found and fixed on real
boards (all in `firmware/main.c`, see its comments for the full story):

- **Button pin (P1.0/KEY) never actually configured as a pulled-up
  input** — only `P1_DIR_PU` was set, not `P1_MOD_OC`, so the pin could
  boot as a push-pull output and the button would appear completely dead.
- **`FREQ_SYS=24000000` hangs the chip** during `CfgFsys()`'s clock-select
  sequence on boot. `16000000` is the value confirmed to run reliably.
- **Main-loop timing silently drifted from real time** — an unconditional
  debug `printf()` on every loop iteration blocked the CPU per UART
  character (polled, unbuffered `putchar()`), stretching every
  millisecond-counted button-hold threshold (5s menu, 10s bootloader) well
  past their real durations. Fixed by removing that printf from the hot
  path, explicitly pinning `UART0_BAUD` (it was silently defaulting to a
  value that overflows the baud-rate register at 16MHz), and replacing a
  32-bit modulo (no hardware divide on the 8051) with a plain counter.

All three button gestures (short click, 5s hold → mode menu, 10s hold →
ISP bootloader) are confirmed working on hardware after these fixes.

## Build

```
cd firmware
make FREQ_SYS=16000000 EXTRA_FLAGS='-DUART0_BAUD=57600'   # -> pdtricker.hex / pdtricker.bin
```

Both flags matter, not just style:

- **`FREQ_SYS=16000000`** — the default `24000000` hangs the chip during
  `CfgFsys()`'s clock-select sequence on boot (confirmed on hardware, not
  theoretical). 16MHz is the combination that's actually been run reliably.
- **`EXTRA_FLAGS='-DUART0_BAUD=57600'`** — `UART0_BAUD` is never set
  otherwise and silently falls back to `debug.h`'s `9600` default, which
  overflows the 8-bit `TH1` baud-rate reload register at 16MHz (the actual
  baud ends up unpredictable, not 9600). 57600 is what the UART command
  docs below assume.

A precompiled `pdtricker.bin`/`.hex` (built with these exact flags, from
the current `main.c`) is attached to the
[latest release](../../releases/latest) if you just want to flash it
without building.

## Flash over USB

The CH552T has a ROM USB bootloader — no programmer needed. Per the
[v1.1 schematic](https://github.com/wuxx/PDTricker/blob/master/hardware/PDTricker-v1.1.png),
the MCU's D+/D- (P3.6/P3.7) are wired straight to the board's Type-C port
(USB2_P/USB2_N on J1) — the same USB cable you'd use to power the board is
the programming connection, no extra wiring needed.

**Note: the "no button needed" ISP entry only applies to a factory-blank
chip** — a brand new, never-flashed CH552T boots straight into the ROM
bootloader on power-up. Once any application firmware has been flashed
(including the original wuxx factory firmware this board usually ships
with), the chip boots that firmware instead, and firmware without its own
USB stack (like this one — it only talks UART, see below) never re-enters
ISP mode on its own. Confirmed the hard way: 2 separate boards, 3 separate
host machines, multiple cables — all showed the exact same `lsusb`/dmesg
`error -71` symptom (kernel sees *something* respond electrically but
enumeration never completes), because the already-flashed firmware simply
never talks USB. `lsusb`/`dmesg -w` showing nothing (or a failed
enumeration) while the board's own LED lights up (so it's getting power) is
the signature of this — not a broken board or cable.

Two ways to get into ISP mode once firmware is already on the chip:

1. **This firmware's own button-hold-to-flash (once it's on the chip):**
   hold the button while plugging the board into J1 and keep holding for
   **10 seconds** — the 20V LED starts fast-blinking once you pass the 5s
   mark (that's the existing mode-menu's own threshold) and stays lit solid
   right before the jump. Release before 10s and you get the normal
   power-on gestures instead (safe-boot-override under 5s, mode menu
   5s-10s) — nothing new needed to remember there. This only works once
   this firmware (or anything that includes the same
   `jump_to_bootloader()`) is already flashed — doesn't help for the very
   first flash.
2. **Manual D+-to-3V3 bridge (needed for the first flash, or if the chip
   ever ends up running non-cooperative firmware again):** briefly bridge
   MCU pin 14 (P3.6/D+, the same net as `USB2_P` on J1) to a 3V3 point on
   the board (e.g. the `C4` decoupling cap pad) with tweezers/a jumper wire
   while plugging the USB cable into J1, then release after ~1s. No
   soldering — just a momentary touch during power-on. This is the
   ROM bootloader's actual hardware entry condition (D+ pulled to 3.3V at
   power-on); the button-hold above is just this firmware voluntarily
   jumping there in software once it's already running.

Once in ISP mode (either way):

1. On the machine the board is physically plugged into, install a flasher:
   - `pip install wchisp` (or `chprog`), or the Rust `wchisp` CLI
     (https://github.com/ch32-rs/wchisp) — either works with CH55x chips.
2. Flash it:
   ```
   wchisp flash pdtricker.bin
   ```
   or, if using the Makefile's `flash` target (expects `wchisptool` on PATH):
   ```
   make flash
   ```
3. If that doesn't detect the device: unplug/replug the board right as you
   run the flash command (some `wchisp`/`chprog` versions need the device to
   enumerate freshly to catch the brief ISP window at power-on).

## Flashing via SSH to the machine with the board attached

Since the board is on a different host than where this firmware is built,
copy the built `.bin` over and flash remotely, e.g.:

```
scp firmware/pdtricker.bin <host>:/tmp/
ssh <host> 'wchisp flash /tmp/pdtricker.bin'
```

Replace `<host>` with your SSH target once you've got it configured.

## Layout

- `firmware/main.c` — application, ported from the original `software/ch552t/main.c`
- `include/` — SDCC-compatible CH552 register headers + adc/debug helpers (from ch554_sdcc)

## Extra features (beyond the original)

- **UART commands** — send `5V`, `9V`, `12V`, `15V`, `20V` or `?` (status) as a
  newline-terminated line on UART0 (57600 baud, TX=P3.1, RX=P3.0) to switch
  voltage or query the current one from a script/computer instead of the button.
- **Single vs. double-click** — clicks within a 300ms window of each other
  count as one burst: 1 click steps forward (5→9→12→15→20→5V), 2 clicks
  steps backward. No long-press or triple-click shortcut to jump straight to
  20V — a 3rd click would be physically indistinguishable from just clicking
  forward 3 separate times (which lands on 15V, not 20V), so it would be
  confusing rather than useful. There's also no long-press-to-20V: an
  accidentally extended hold should never jump the voltage.
- **LED fault feedback** — after switching voltage, a fast blink (200ms) means
  PD is still negotiating; if the ADC reading still doesn't match the
  requested voltage after ~500ms, it switches to a slow blink (600ms) to
  signal a real fault instead of looking like it's still just negotiating.
- **Voltage is remembered across power cycles** (was already the case since
  v1.1 of the original firmware) — whatever you last selected (button or
  UART) is what it boots into next time.
- **Safe-boot override** — hold the button down while plugging the board in
  for less than 5s; it forces 5V (and saves that as the new remembered
  voltage), overriding whatever was remembered before. The 5V LED blinks
  twice to confirm the override triggered. This only runs after the chip has
  already decided to start the application — the CH552's USB-ISP bootloader
  entry is controlled by a separate config fuse set by the flashing tool,
  not by any GPIO this firmware reads, so this doesn't interfere with
  getting into flash mode.
- **3 selectable boot modes**, changed via the on-board menu or UART. The
  button's normal click cycling always works in every mode — the mode only
  decides what gets requested automatically at power-on:
  - `0` **always-5V** — always boot on 5V, ignore the remembered voltage.
    (There's no separate "request nothing" mode: the CH224K always reacts to
    whatever its CFG pins read, so the closest thing to "off" is forcing the
    safe 5V level, which is exactly this mode.)
  - `1` **safe-delay** — boot on 5V, wait 3s, then step up to the remembered
    voltage. Whatever gets plugged in has 3 full seconds on 5V to be
    unplugged in reaction before it steps up. The step-up reuses the normal
    fast-blink negotiate feedback so you can see which LED it's heading to.
  - `2` **remembered** — power up directly on the last-selected voltage.
  - Set via UART: `MODE0`..`MODE2` (saved to DataFlash immediately).
  - Set via the button: hold it for **~5 seconds** (this is the *only*
    long-press action left — a hold shorter than 5s does nothing but wait).
    The 20V LED fast-blinks while held, as a "menu incoming" warning, then
    once released one LED lights up per mode (5V=0, 9V=1, 12V=2).
    Short-click to step through modes; either hold ~800ms or leave it idle
    for 2s to save the shown mode and exit. Reachable both at power-on (hold
    the button before/while plugging in past 5s) and any time during normal
    operation.
- **Automatic fallback to 5V on a persistent fault** — if the ADC reading
  still doesn't match the requested voltage after the negotiate window
  (~500ms of slow-blink), the firmware doesn't just sit there blinking: it
  actively switches back down to 5V instead of leaving the output on an
  unverified/mismatched level.
- **`?stats` UART command** — reports switch count and time spent (ms) on
  each voltage since boot, e.g. for observing charging behavior of whatever
  you're testing with the board. `?reset` zeroes the counters without
  rebooting the board, for starting a fresh test run.

Note: the CH224K only supports the fixed 5/9/12/15/20V levels selectable via
CFG1-3 (see its datasheet) — there's no PPS/custom-voltage mode, and no
skip-list either (every level is always in the cycle) since that's simpler
and there was no real need to hide any of the 5 levels.
