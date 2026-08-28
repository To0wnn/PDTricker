// PDTricker firmware, ported from Keil C51 to SDCC (ch554_sdcc template)
// Original: https://github.com/wuxx/PDTricker software/ch552t/main.c

#include <stdio.h>
#include <string.h>
#include <ch554.h>
#include <debug.h>
#include <adc.h>

void led_all_off();
void led_off(uint8_t value);
void led_set(uint8_t value, uint8_t blink);
uint16_t adc_read();
int pd_verify(uint8_t value);
void jump_to_bootloader();

enum {
    PD_5V = 0,
    PD_9V,
    PD_12V,
    PD_15V,
    PD_20V,
    PD_MAX,
};

enum {
    LED_MODE_ON = 0,
    LED_MODE_BLINK_FAST,
    LED_MODE_BLINK_SLOW,
};

/* boot mode: how the board behaves at power-on, selectable via the menu
 * (hold button ~5s) or "MODE n" over UART. Order matches the menu's LED
 * indicators: 5V=BOOT_ALWAYS_5V, 9V=BOOT_SAFE_DELAY, 12V=BOOT_REMEMBERED.
 * Button cycling always works regardless of mode — mode only affects what
 * gets requested automatically at power-on. There's no separate "off"
 * mode: the CH224K always reacts to whatever its CFG pins read, so the
 * closest thing to "request nothing" is to force the safe 5V level, which
 * is exactly what BOOT_ALWAYS_5V already does. */
enum {
    BOOT_ALWAYS_5V = 0,    /* always boot on 5V */
    BOOT_SAFE_DELAY,       /* boot on 5V, then step up to remembered voltage after a delay */
    BOOT_REMEMBERED,       /* power up directly on the remembered voltage */
    BOOT_MODE_MAX,
};

SBIT(KEY, 0x90, 0);      // P1.0

SBIT(LED_5V,  0xB0, 2);  // P3.2
SBIT(LED_9V,  0x90, 4);  // P1.4
SBIT(LED_12V, 0x90, 5);  // P1.5
SBIT(LED_15V, 0x90, 6);  // P1.6
SBIT(LED_20V, 0x90, 7);  // P1.7

SBIT(CFG_1, 0xB0, 3);    // P3.3
SBIT(CFG_2, 0xB0, 4);    // P3.4
SBIT(CFG_3, 0xB0, 5);    // P3.5

/*******************************************************************************/
/*******************************************************************************/

/* Jump into the CH552's ROM ISP bootloader (at 0x3800) from running
 * application code, so holding the button at power-on can drop straight
 * into flash mode without needing the manual D+-to-3V3 hardware bridge.
 * There's no software-settable "force bootloader" SFR bit — bSW_RESET only
 * resets back into the application (that's how the bootloader itself
 * returns to user code after flashing), so a direct call into the
 * bootloader's entry point is the standard approach WCH's own tools rely
 * on. USB is reset first (USB_INT_EN=0, USB_CTRL=0x06) so the bootloader
 * doesn't inherit whatever USB register state the application left behind
 * — running with stale state is the known cause of this method being
 * flaky on some CH55x boards. Never returns. */
void jump_to_bootloader()
{
    EA = 0;             /* disable all interrupts */
    USB_INT_EN = 0;      /* USB interrupt disable */
    USB_CTRL = 0x06;     /* USB base control reset */
    mDelaymS(50);

    __asm
        lcall 0x3800
    __endasm;

    while (1);           /* never reached */
}

uint8_t WriteDataFlash(uint8_t Addr, uint8_t *buf, uint8_t len)
{
    uint8_t i;
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;                                                           // enter safe mode
    GLOBAL_CFG |= bDATA_WE;                                                    // enable Data-Flash write
    SAFE_MOD = 0;                                                              // exit safe mode
    ROM_ADDR_H = DATA_FLASH_ADDR >> 8;
    Addr <<= 1;
    for (i = 0; i < len; i++) {
        ROM_ADDR_L = Addr + i * 2;
        ROM_DATA_L = *(buf + i);
        if (ROM_STATUS & bROM_ADDR_OK) {                                       // address valid
            ROM_CTRL = ROM_CMD_WRITE;                                          // write
        }
        if ((ROM_STATUS ^ bROM_ADDR_OK) > 0) return i;                         // 0x00=success, 0x02=unknown command
    }
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    GLOBAL_CFG &= ~bDATA_WE;
    SAFE_MOD = 0;
    return i;
}

uint8_t ReadDataFlash(uint8_t Addr, uint8_t len, uint8_t *buf)
{
    uint8_t i;
    ROM_ADDR_H = DATA_FLASH_ADDR >> 8;
    Addr <<= 1;
    for (i = 0; i < len; i++) {
        ROM_ADDR_L = Addr + i * 2;                                             // Addr must be even
        ROM_CTRL = ROM_CMD_READ;
        *(buf + i) = ROM_DATA_L;
    }
    return i;
}

uint8_t pd_config_read()
{
    uint8_t pc;
    ReadDataFlash(0x0, 1, &pc);
    return pc;
}

void pd_config_update(uint8_t pc)
{
    WriteDataFlash(0x0, &pc, 1);
}

uint8_t boot_mode_read()
{
    uint8_t m;
    ReadDataFlash(0x1, 1, &m);
    return (m >= BOOT_MODE_MAX) ? BOOT_REMEMBERED : m;  /* 255 = unwritten flash -> default */
}

void boot_mode_update(uint8_t m)
{
    WriteDataFlash(0x1, &m, 1);
}

/*******************************************************************************/
/*******************************************************************************/

/* TX:  P3.1/PWM2/TXD */
/* RX:  P3.0/PWM1/RXD */
int pd_set(uint8_t value)
{
    switch (value) {
        case (PD_5V):
            CFG_1 = 1;
            CFG_2 = 0;
            CFG_3 = 0;
            break;
        case (PD_9V):
            CFG_1 = 0;
            CFG_2 = 0;
            CFG_3 = 0;
            break;
        case (PD_12V):
            CFG_1 = 0;
            CFG_2 = 0;
            CFG_3 = 1;
            break;
        case (PD_15V):
            CFG_1 = 0;
            CFG_2 = 1;
            CFG_3 = 1;
            break;
        case (PD_20V):
            CFG_1 = 0;
            CFG_2 = 1;
            CFG_3 = 0;
            break;
        default:
            break;
    }

    pd_verify(value);

    return 0;
}

/* NOT a clock problem: an earlier compensation attempt here (a blanket
 * MS_SCALE(ms) multiplier on every threshold) was based on the main loop's
 * press_ms counter falling behind real time by ~2.5x. That theory broke
 * once the SAME +=10-per-iteration pattern in run_mode_menu()'s loop was
 * observed staying accurate - both loops run on the identical mDelaymS(10),
 * so a real clock mismatch would have hit both equally. The actual cause:
 * pd_verify() below unconditionally printf()s an ADC_VALUE line every
 * single main-loop iteration, and putchar() (debug.c) blocks the CPU per
 * character (polled UART, no buffer/interrupt) - at the UART0_BAUD this
 * board actually builds with (9600, see debug.h; the "57600" in comments
 * here and in the README was never wired up via a build flag, so it was
 * always wrong), a ~25-char line costs ~25ms of real blocking time on top
 * of the intended 10ms mDelaymS(10). run_mode_menu()'s loop has no printf
 * in its body at all, so it was never affected - which is exactly the
 * asymmetry that gave this away. Fixed by only printing on change instead
 * of every iteration (see pd_verify()); no threshold-scaling needed once
 * the loop body actually takes ~10ms again. */

/* how long after a voltage switch we treat a mismatch as "still negotiating"
 * rather than a real fault (PD negotiation normally settles within ~300ms) */
#define PD_NEGOTIATE_MS 500
uint16_t negotiate_ms_left = 0;

/* set by pd_verify() once a mismatch has persisted past the negotiate window;
 * main() checks this and falls back once FAULT_HOLD_MS has also elapsed */
uint8_t pd_fault = 0;

/* extra grace period on top of PD_NEGOTIATE_MS before actually falling back
 * on a fault - gives a slow charger/cable a real chance to still pick it up,
 * and gives you time to manually switch away yourself (the slow-blink fault
 * indicator is already visible the whole time) instead of the board
 * snapping back the instant the 500ms negotiate window closes. */
#define FAULT_HOLD_MS 5000
uint16_t fault_ms = 0;

/* last voltage that actually verified OK - what a fault falls back to,
 * instead of always forcing 5V. If nothing has ever verified yet (e.g. the
 * very first switch after boot fails), PD_5V is the sane starting value
 * here since it's the one level every PD source is required to support. */
uint8_t last_good_pd = PD_5V;

/* compare with adc value */
int pd_verify(uint8_t value)
{
    uint8_t r = 0;

    /* 100K 510K */
    uint16_t adc_value;
    /* No printf here (there used to be an unconditional one, then a
     * printf-on-change one) - this runs every main-loop iteration, and
     * putchar() (debug.c) blocks the CPU per UART character (polled,
     * unbuffered). UART0_BAUD was also never actually pinned to a build
     * flag, so it silently sat on debug.h's 9600 default, which overflows
     * TH1 (8-bit reload register) at FREQ_SYS=16000000 - actual baud was
     * unpredictable, not the ~9600 earlier comments here assumed. Between
     * the blocking-per-character cost and the uncertain baud, any printf
     * in this function is unsafe to reason about for loop timing purposes,
     * so it's removed entirely rather than made conditional again. Use the
     * "?" UART command (drained in main()'s loop, outside this function)
     * to read the current ADC value on demand instead. */
    adc_value = adc_read();

    switch (value) {
        case (PD_5V):
            r = (adc_value >= 57 && adc_value <= 69) ? 0 : 1;
            break;
        case (PD_9V):
            r = (adc_value >= 108 && adc_value <= 120) ? 0 : 1;
            break;
        case (PD_12V):
            r = (adc_value >= 146 && adc_value <= 158) ? 0 : 1;
            break;
        case (PD_15V):
            r = (adc_value >= 184 && adc_value <= 197) ? 0 : 1;
            break;
        case (PD_20V):
            r = (adc_value >= 247 && adc_value <= 260) ? 0 : 1;
            break;
        default:
            break;
    }

    if (r == 0) {
        negotiate_ms_left = 0;
        pd_fault = 0;
        fault_ms = 0;
        last_good_pd = value;
        led_set(value, LED_MODE_ON);
    } else if (negotiate_ms_left > 0) {
        led_set(value, LED_MODE_BLINK_FAST);   /* still negotiating: fast blink */
    } else {
        led_set(value, LED_MODE_BLINK_SLOW);   /* stuck mismatched: slow blink = fault */
        if (value != PD_5V) {
            pd_fault = 1;
        }
    }

    return r;
}

uint16_t count = 0;
uint8_t blink = 0;              /* 0=off, 1=fast, 2=slow */
uint8_t blink_value = PD_5V;

void led_all_off()
{
    LED_5V  = 0;
    LED_9V  = 0;
    LED_12V = 0;
    LED_15V = 0;
    LED_20V = 0;
}

void led_off(uint8_t value)
{
    switch (value) {
        case (PD_5V):  LED_5V  = 0; break;
        case (PD_9V):  LED_9V  = 0; break;
        case (PD_12V): LED_12V = 0; break;
        case (PD_15V): LED_15V = 0; break;
        case (PD_20V): LED_20V = 0; break;
        default: break;
    }
}

void led_set(uint8_t value, uint8_t mode)
{
    if (mode == LED_MODE_ON) {
        blink = 0;

        switch (value) {
            case (PD_5V):  LED_5V  = 1; break;
            case (PD_9V):  LED_9V  = 1; break;
            case (PD_12V): LED_12V = 1; break;
            case (PD_15V): LED_15V = 1; break;
            case (PD_20V): LED_20V = 1; break;
            default: break;
        }
    } else {
        blink = mode;
        blink_value = value;
    }
}

uint16_t adc_read()
{
    uint16_t v;

    ADC_ChannelSelect(0);
    ADC_START = 1;
    while (ADC_START);
    v = (uint16_t)ADC_DATA;
    return v;
}

void blink_handler()
{
    switch (blink_value) {
        case (PD_5V):  LED_5V  = !LED_5V;  break;
        case (PD_9V):  LED_9V  = !LED_9V;  break;
        case (PD_12V): LED_12V = !LED_12V; break;
        case (PD_15V): LED_15V = !LED_15V; break;
        case (PD_20V): LED_20V = !LED_20V; break;
        default: break;
    }
}

uint16_t key_read()
{
    /* 1: release; 0: pressed */
    return (uint16_t)(KEY == 0 ? 0 : 1);
}

/* ---- since-boot stats: switch count + time spent per voltage ----
 * placed in xdata (xRAM) since internal RAM is tight */
__xdata unsigned int switch_count[PD_MAX];
__xdata unsigned long time_ms[PD_MAX];

void print_stats()
{
    printf("stats (since boot):\n");
    printf("  5V:  %u switches, %lu ms\n",  switch_count[PD_5V],  time_ms[PD_5V]);
    printf("  9V:  %u switches, %lu ms\n",  switch_count[PD_9V],  time_ms[PD_9V]);
    printf("  12V: %u switches, %lu ms\n",  switch_count[PD_12V], time_ms[PD_12V]);
    printf("  15V: %u switches, %lu ms\n",  switch_count[PD_15V], time_ms[PD_15V]);
    printf("  20V: %u switches, %lu ms\n",  switch_count[PD_20V], time_ms[PD_20V]);
}

void reset_stats()
{
    uint8_t i;
    for (i = 0; i < PD_MAX; i++) {
        switch_count[i] = 0;
        time_ms[i] = 0;
    }
    printf("stats reset\n");
}

/* switch to a given PD level: turn off old LED, apply + persist the new one */
void pd_apply(uint8_t curr_pd, uint8_t next_pd)
{
    led_off(curr_pd);
    printf("pd_set %d\n", next_pd);
    negotiate_ms_left = PD_NEGOTIATE_MS;
    fault_ms = 0;
    pd_fault = 0;
    pd_set(next_pd);
    pd_config_update(next_pd);
    switch_count[next_pd]++;
}

/* ---- UART command line: "5V"/"9V"/"12V"/"15V"/"20V" or "?" for status ---- */
#define CMD_BUF_LEN 8
char cmd_buf[CMD_BUF_LEN];
uint8_t cmd_len = 0;

/* returns 0..PD_MAX-1 on match, 0xFF if no match */
uint8_t cmd_parse_pd(char *s)
{
    if (!strcmp(s, "5V"))  return PD_5V;
    if (!strcmp(s, "9V"))  return PD_9V;
    if (!strcmp(s, "12V")) return PD_12V;
    if (!strcmp(s, "15V")) return PD_15V;
    if (!strcmp(s, "20V")) return PD_20V;
    return 0xFF;
}

/* menu: hold the button ~5s to enter. 20V LED blinks while held to signal
 * "menu incoming". Once in the menu, one LED is lit per boot mode (5V/9V/
 * 12V = mode 0..2); short click steps to the next mode, letting go of
 * the button for MENU_IDLE_MS commits the currently-shown mode and exits.
 * Reachable both at power-on (button held before boot) and any time
 * afterwards (hold ~5s) — button cycling is never disabled by boot mode,
 * so there's no lockout scenario to guard against here. */
#define MENU_ENTRY_HOLD_MS 5000
#define MENU_IDLE_MS       10000

void menu_show(uint8_t mode)
{
    led_all_off();
    switch (mode) {
        case BOOT_ALWAYS_5V:  LED_5V  = 1; break;
        case BOOT_SAFE_DELAY: LED_9V  = 1; break;
        case BOOT_REMEMBERED: LED_12V = 1; break;
        default: break;
    }
}

void run_mode_menu(uint8_t *boot_mode)
{
    uint8_t mode = *boot_mode;
    uint16_t idle_ms = 0;
    uint16_t hold_ms;
    uint16_t blink_ms = 0;   /* counts up to the 500ms LED_20V toggle interval - a
        plain counter+reset instead of `idle_ms % 500`, since the 8051 has no
        hardware divide and 500 isn't a power of 2, so that modulo was a real
        per-iteration cost (same class of bug as the loop%20/%60 one already
        fixed in main()'s loop, which is exactly why MENU_IDLE_MS kept firing
        too soon even after that fix - this loop had its own instance of it). */

    /* entry confirmation: fast-blink the 20V LED for the remainder of the hold */
    led_all_off();
    while (key_read() == 0) {
        LED_20V = !LED_20V;
        mDelaymS(100);
    }

    printf("mode menu: current=%d\n", mode);
    menu_show(mode);

    for (;;) {
        if (key_read() == 0) {
            /* debounce + wait for release, counting hold time for a long-press exit */
            hold_ms = 0;
            while (key_read() == 0) {
                mDelaymS(10);
                hold_ms += 10;
            }
            if (hold_ms >= 800) {
                break; /* long press: commit and exit immediately */
            }
            /* short click: advance to next mode */
            mode = (mode + 1) % BOOT_MODE_MAX;
            menu_show(mode);
            idle_ms = 0;
        }

        mDelaymS(10);
        idle_ms += 10;

        /* slow background blink on LED_20V for as long as we're in the
         * menu, so it reads as "still in the menu" rather than looking
         * like a normal voltage LED (5V/9V/12V here mean boot-mode, not
         * output voltage, and that's easy to mistake for the latter) */
        blink_ms += 10;
        if (blink_ms >= 500) {
            blink_ms = 0;
            LED_20V = !LED_20V;
        }

        if (idle_ms >= MENU_IDLE_MS) {
            break; /* no input for a while: commit and exit */
        }
    }
    LED_20V = 0;

    if (mode != *boot_mode) {
        boot_mode_update(mode);
    }
    *boot_mode = mode;
    printf("mode menu: saved=%d\n", mode);

    led_all_off();
    LED_5V = 1;
    mDelaymS(200);
    LED_5V = 0;
}

/* uart-0 baudrate: 57600 */
void main()
{
    uint16_t curr_key, prev_key;
    uint16_t curr_pd = pd_config_read();
    uint8_t boot_mode = boot_mode_read();
    /* uint8_t, not uint32_t: only ever compared via `% 20`/`% 60` below, and
     * the 8051 has no hardware divide - a 32-bit modulo on every iteration
     * was measurably slowing the loop down for no benefit, since the value
     * itself is never used for anything beyond that modulo. */
    uint8_t loop = 0;
    uint16_t press_ms = 0;
    uint8_t long_press_fired = 0;
    uint16_t click_wait_ms = 0;   /* counts down after each release, waiting for another click */
    uint8_t click_count = 0;      /* clicks seen in the current burst: 1=forward, 2=back */
    uint8_t press_active = 0;     /* debounced press state, survives brief bounce glitches */
    uint16_t release_ms = 0;      /* how long the key has read released, for release-debounce */

    /* CfgFsys() timing note: on this board, skipping CfgFsys() entirely
     * made the whole chip run visibly sluggish (not just KEY - everything),
     * so the power-on-default clock is NOT actually the documented 24MHz
     * Fosc default while running application code (possibly a lower
     * fallback rate before CLOCK_CFG is ever touched). FREQ_SYS=16000000
     * is the last combination confirmed to boot reliably without hanging
     * and without this sluggishness - keep that, even though its
     * long-press timing accuracy is still under investigation separately. */
    CfgFsys();
    mInitSTDIO();
    printf("\r\nPDTricker system buildtime [" __TIME__ " " __DATE__ "] rev sdcc-1.0\r\n");
    printf("curr_pd: %d, boot_mode: %d\n", curr_pd, boot_mode);
    printf("uart cmds: 5V 9V 12V 15V 20V ? ?stats ?reset MODE0-2\n");

    if (curr_pd == 255) {
        pd_config_update(PD_5V);
        curr_pd = pd_config_read();
        printf("factory init: curr_pd: %d\n", curr_pd);
    }

    // P1: KEY input w/ pullup (P1.0), LEDs push-pull output (P1.4-7)
    // quasi-bidirectional (mode 3) needs MOD_OC=1 *and* DIR_PU=1 on bit 0 -
    // MOD_OC bit 0 was never touched before (only DIR_PU), leaving it on
    // its power-on-default; if that default is 0 the pin is push-pull
    // output instead of pulled-up input and KEY always reads the same fixed
    // value, i.e. the button appears completely dead.
    P1_MOD_OC  =  (P1_MOD_OC & ~((1 << 4) | (1 << 5) | (1 << 6) | (1 << 7))) | (1 << 0);
    P1_DIR_PU  =  (P1_DIR_PU & ~(1 << 0)) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 0);
    // P3: LED_5V + CFG_1/2/3 push-pull output (P3.2-5)
    P3_MOD_OC  &= ~((1 << 2) | (1 << 3) | (1 << 4) | (1 << 5));
    P3_DIR_PU  |= (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5);

    ADCInit(0);

    /* Power-on button-hold gestures - a single timed hold, read once, with
     * 3 thresholds so none of them can be triggered by accident by a
     * shorter hold meant for another gesture:
     *   < MENU_ENTRY_HOLD_MS (5s)   -> safe-boot override (forces 5V)
     *   >= MENU_ENTRY_HOLD_MS (5s)  -> mode menu
     *   >= BOOTLOADER_HOLD_MS (10s) -> jump into the ISP bootloader, so
     *     flashing new firmware no longer needs the manual D+-to-3V3
     *     hardware bridge - just hold the button while plugging in.
     * Held for the full 10s without release -> straight to bootloader, no
     * need to release and re-press. Released between 5s-10s -> menu, same
     * as before this feature existed. Released under 5s -> safe-boot. */
    #define BOOTLOADER_HOLD_MS 10000
    if (key_read() == 0) {
        uint16_t held_ms = 0;
        while (key_read() == 0 && held_ms < BOOTLOADER_HOLD_MS) {
            mDelaymS(10);
            /* visible feedback while past the menu's own entry threshold,
             * so a hold heading toward the bootloader still looks alive
             * (run_mode_menu() isn't entered here, so its own fast-blink
             * feedback doesn't run for this range) */
            if (held_ms >= MENU_ENTRY_HOLD_MS) {
                LED_20V = !LED_20V;
            }
            held_ms += 10;
        }

        if (held_ms >= BOOTLOADER_HOLD_MS) {
            led_all_off();
            LED_20V = 1;   /* solid LED: confirms the hold was seen before we jump */
            jump_to_bootloader();
        } else if (held_ms >= MENU_ENTRY_HOLD_MS) {
            /* released between the menu and bootloader thresholds: hand
             * off straight to the menu, which itself waits for release
             * before showing anything */
            run_mode_menu(&boot_mode);
        } else {
            /* released early: safe-boot override, forces 5V */
            printf("safe-boot: button held at power-on, forcing 5V\n");
            curr_pd = PD_5V;
            pd_config_update(curr_pd);
            led_all_off();
            LED_5V = 1;
            mDelaymS(150);
            LED_5V = 0;
            mDelaymS(150);
            LED_5V = 1;
            mDelaymS(150);
            LED_5V = 0;
        }
    }

    /* boot sequence, per the selected boot mode. pd_apply()'s fast-blink
     * negotiate feedback shows which LED it's heading to during a switch. */
    prev_key = curr_key = 1;
    led_all_off();

    switch (boot_mode) {
        case BOOT_ALWAYS_5V:
            curr_pd = PD_5V;
            pd_set(PD_5V);
            break;
        case BOOT_SAFE_DELAY:
            pd_set(PD_5V);
            if (curr_pd != PD_5V) {
                /* fast-blink the remembered target LED for the whole hold,
                 * so it's visible up front where this is heading instead
                 * of only starting to blink once the delay is already
                 * over - same fast-blink cadence pd_apply()'s own
                 * negotiate feedback uses, just started early. */
                #define BOOT_SAFE_HOLD_MS 3000
                {
                    uint16_t held_ms = 0;
                    while (held_ms < BOOT_SAFE_HOLD_MS) {
                        /* keep re-checking 5V itself too, not just the
                         * target LED - a slow charger can still be
                         * settling onto 5V partway into this hold (seen on
                         * hardware: consistent right after a quick
                         * unplug/replug, inconsistent after a longer gap),
                         * and without this its LED state was frozen at
                         * whatever the single pd_set(PD_5V) above saw. */
                        pd_verify(PD_5V);
                        led_off(curr_pd);
                        mDelaymS(200);
                        led_set(curr_pd, LED_MODE_ON);
                        mDelaymS(200);
                        held_ms += 400;
                    }
                }
                pd_apply(PD_5V, curr_pd);
            }
            break;
        case BOOT_REMEMBERED:
        default:
            pd_set(curr_pd);
            break;
    }

    while (1) {
        prev_key = curr_key;
        curr_key = key_read();

        if (curr_key == 0) {
            /* held down (or a bounce glitch mid-hold - see below): single
             * long threshold (5s -> menu). No other long-press action, so
             * an extended hold can never do anything but open the menu or
             * nothing at all. */
            release_ms = 0; /* any low reading cancels a pending release-debounce */
            if (prev_key == 1 && !press_active) {
                /* real press start, not just a bounce glitch we already
                 * counted as part of an ongoing hold */
                press_ms = 0;
                long_press_fired = 0;
                press_active = 1;
            }
            press_ms += 10;
            if (press_ms >= MENU_ENTRY_HOLD_MS && !long_press_fired) {
                long_press_fired = 1;
                click_count = 0;
                click_wait_ms = 0;
                run_mode_menu(&boot_mode);
                prev_key = curr_key = 1; /* re-sync after the menu's own key handling */
                press_active = 0;
            }
        } else if (press_active) {
            /* reads high, but debounce before treating this as a real
             * release - a few ms of contact chatter mid-hold would
             * otherwise be seen as "released then pressed again", which
             * reset press_ms back to 0 on every glitch and made the 5s
             * threshold effectively unreachable while still holding it
             * down continuously. */
            release_ms += 10;
            if (release_ms >= 30) {
                press_active = 0;
                if (!long_press_fired) {
                    if (click_count < 2) {
                        click_count++;
                    }
                    click_wait_ms = 300; /* (re)start the wait for another click */
                }
            }
        }

        if (click_count > 0 && curr_key == 1) {
            if (click_wait_ms > 10) {
                click_wait_ms -= 10;
            } else {
                /* window expired: 1 click -> step forward, 2 clicks -> step back.
                 * No 3rd-click shortcut to 20V — that would be indistinguishable
                 * from just clicking forward 3 times normally, which already
                 * gets you to 15V, not 20V, so it'd be confusing rather than useful. */
                if (click_count == 1) {
                    uint16_t next_pd = curr_pd + 1;
                    if (next_pd == PD_MAX) {
                        next_pd = PD_5V;
                    }
                    pd_apply(curr_pd, next_pd);
                    curr_pd = next_pd;
                } else {
                    uint16_t prev_pd = (curr_pd == PD_5V) ? (PD_MAX - 1) : (curr_pd - 1);
                    pd_apply(curr_pd, prev_pd);
                    curr_pd = prev_pd;
                }
                click_count = 0;
            }
        }

        /* drain any pending UART command (non-blocking) */
        while (RI) {
            char c = CH554UART0RcvByte();
            if (c == '\r' || c == '\n') {
                if (cmd_len > 0) {
                    cmd_buf[cmd_len] = '\0';
                    if (!strcmp(cmd_buf, "?")) {
                        printf("pd=%dV, mode=%d\n", curr_pd == PD_5V ? 5 : curr_pd == PD_9V ? 9 :
                               curr_pd == PD_12V ? 12 : curr_pd == PD_15V ? 15 : 20, boot_mode);
                    } else if (!strcmp(cmd_buf, "?stats")) {
                        print_stats();
                    } else if (!strcmp(cmd_buf, "?reset")) {
                        reset_stats();
                    } else if (cmd_len == 5 && !strncmp(cmd_buf, "MODE", 4) &&
                               cmd_buf[4] >= '0' && cmd_buf[4] < '0' + BOOT_MODE_MAX) {
                        boot_mode = cmd_buf[4] - '0';
                        boot_mode_update(boot_mode);
                        printf("boot_mode=%d\n", boot_mode);
                    } else {
                        uint8_t target = cmd_parse_pd(cmd_buf);
                        if (target != 0xFF && target != curr_pd) {
                            pd_apply(curr_pd, target);
                            curr_pd = target;
                        } else if (target == 0xFF) {
                            printf("err: unknown cmd '%s'\n", cmd_buf);
                        }
                    }
                    cmd_len = 0;
                }
            } else if (cmd_len < CMD_BUF_LEN - 1) {
                cmd_buf[cmd_len++] = c;
            } else {
                cmd_len = 0; /* overflow: drop line */
            }
        }

        if (negotiate_ms_left > 0) {
            negotiate_ms_left = (negotiate_ms_left > 10) ? negotiate_ms_left - 10 : 0;
        }

        pd_verify(curr_pd);

        if (pd_fault) {
            /* Give it FAULT_HOLD_MS on top of the PD_NEGOTIATE_MS window
             * already spent before actually falling back - a slow
             * charger/cable might still pick it up, and the slow-blink LED
             * already shows something's wrong so there's time to manually
             * switch away yourself if you don't want to wait. */
            fault_ms += 10;
            if (fault_ms >= FAULT_HOLD_MS) {
                /* still mismatched after the extra grace period: bail out
                 * to the last voltage that actually verified OK (not
                 * always 5V - if e.g. 12V isn't supported by whatever's
                 * plugged in but 9V is, this lands back on 9V instead of
                 * unnecessarily dropping all the way to 5V) instead of
                 * leaving the output sitting on a bad/unverified level. */
                printf("fault: %d mismatched, falling back to %d\n", curr_pd, last_good_pd);
                pd_fault = 0;
                pd_apply(curr_pd, last_good_pd);
                curr_pd = last_good_pd;
            }
        }

        /* fast blink every 200ms, slow blink every 600ms */
        if ((blink == LED_MODE_BLINK_FAST && loop % 20 == 0) ||
            (blink == LED_MODE_BLINK_SLOW && loop % 60 == 0)) {
            blink_handler();
        }

        time_ms[curr_pd] += 10;
        mDelaymS(10);
        loop++;
    }
}
