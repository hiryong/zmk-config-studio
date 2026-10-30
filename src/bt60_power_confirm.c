/*
 * BT60 V2: physical key combo power switch.
 *
 * Fn = bottom-right key, Space = space bar (physical positions, not keymap
 * bindings, so ZMK Studio changes do not move them).
 *
 * - Off: hold Fn, then hold Space; once both have been held together for 3 s,
 *   the keyboard turns off when Fn is released (Fn is the wake key, so it must
 *   be up before the chip goes to sleep). While Fn is down, the Space press is
 *   held back: released before 3 s it is sent as a normal key press, so a
 *   quick Fn+Space still does what the keymap says.
 * - Wake: only Fn wakes the chip from System OFF (see bt60.overlay).
 * - On: after a combo power-off, the next wake asks for Fn+Space held together
 *   for 3 s (any order). Key input is not sent while waiting. If that does not
 *   happen within the timeout the keyboard turns off again; while the keys are
 *   held the timeout waits.
 * - USB power or the reset button start normally. 15 min idle sleep does not go
 *   through zmk_pm_soft_off(), so it still wakes on any key.
 * - zmk_pm_soft_off() is wrapped (linker --wrap) to leave a mark in GPREGRET2.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/pm.h>

LOG_MODULE_REGISTER(bt60_power_confirm, CONFIG_ZMK_LOG_LEVEL);

#define MARK_REG 1 /* GPREGRET2 (GPREGRET is used by the bootloader) */
#define MARK_VALUE 0xB6

#define POS_FN CONFIG_BT60_POWER_CONFIRM_FN_POSITION
#define POS_SPACE CONFIG_BT60_POWER_CONFIRM_SPACE_POSITION
#define MAX_POS 64
#define HOLD K_MSEC(CONFIG_BT60_POWER_CONFIRM_HOLD_MS)

/* Turning on */
static bool confirming;
static bool timed_out;
static uint64_t swallowed; /* keys pressed while confirming; their release is swallowed too */
static uint64_t held;      /* keys held while confirming */

/* Turning off */
static bool fn_down;
static bool space_captured; /* Space press held back while Fn is down */
static bool off_armed;      /* combo held 3 s; turn off when Fn is released */
static bool swallow_space_release;
static struct zmk_position_state_changed_event space_press;

static void on_hold_done(struct k_work *work);
static void on_timeout(struct k_work *work);
static void on_power_off(struct k_work *work);
static void off_hold_done(struct k_work *work);
static void off_now(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(on_hold_work, on_hold_done);
static K_WORK_DELAYABLE_DEFINE(timeout_work, on_timeout);
static K_WORK_DEFINE(on_off_work, on_power_off);
static K_WORK_DELAYABLE_DEFINE(off_hold_work, off_hold_done);
static K_WORK_DEFINE(off_work, off_now);

int __real_zmk_pm_soft_off(void);

int __wrap_zmk_pm_soft_off(void) {
    nrf_power_gpregret_set(NRF_POWER, MARK_REG, MARK_VALUE);

    int ret = __real_zmk_pm_soft_off();

    /* Only returns if soft-off failed: do not leave the keyboard locked. */
    LOG_ERR("soft off failed (%d), leaving confirm mode", ret);
    nrf_power_gpregret_set(NRF_POWER, MARK_REG, 0);
    confirming = false;
    k_work_cancel_delayable(&on_hold_work);
    k_work_cancel_delayable(&timeout_work);
    return ret;
}

/* ---------- turning on ---------- */

static bool on_combo_held(void) {
    return (held & BIT64(POS_FN)) && (held & BIT64(POS_SPACE));
}

static void on_power_off(struct k_work *work) {
    if (!confirming) {
        return;
    }
    LOG_INF("not confirmed, back to soft off");
    zmk_pm_soft_off();
}

static void on_hold_done(struct k_work *work) {
    if (confirming && on_combo_held()) {
        LOG_INF("Fn+Space held, starting normally");
        confirming = false;
        k_work_cancel_delayable(&timeout_work);
    }
}

static void on_timeout(struct k_work *work) {
    if (!confirming) {
        return;
    }
    if (on_combo_held()) {
        timed_out = true; /* wait for the hold to finish or be released */
        return;
    }
    k_work_submit(&on_off_work);
}

static int confirm_event(const struct zmk_position_state_changed *ev, uint64_t bit) {
    if (ev->state) {
        swallowed |= bit;
        held |= bit;
    } else {
        swallowed &= ~bit;
        held &= ~bit;
    }

    if (on_combo_held()) {
        if (!k_work_delayable_is_pending(&on_hold_work)) {
            k_work_schedule(&on_hold_work, HOLD);
        }
    } else {
        k_work_cancel_delayable(&on_hold_work);
        if (timed_out) {
            k_work_submit(&on_off_work);
        }
    }
    return ZMK_EV_EVENT_HANDLED;
}

/* ---------- turning off ---------- */

static void off_now(struct k_work *work) {
    LOG_INF("Fn+Space held 3 s, turning off");
    zmk_pm_soft_off();
}

static void off_hold_done(struct k_work *work) {
    if (space_captured && fn_down) {
        /* Space press is dropped: nothing reaches the computer. */
        space_captured = false;
        swallow_space_release = true;
        off_armed = true;
    }
}

/* Let the held-back Space press continue to the keymap. */
static void release_space(void) {
    if (space_captured) {
        space_captured = false;
        k_work_cancel_delayable(&off_hold_work);
        ZMK_EVENT_RELEASE(space_press);
    }
}

static int normal_event(const zmk_event_t *eh, const struct zmk_position_state_changed *ev,
                        uint64_t bit) {
    if (!ev->state && (swallowed & bit)) {
        /* Release of a key whose press was swallowed while turning on. */
        swallowed &= ~bit;
        return ZMK_EV_EVENT_HANDLED;
    }

    if (ev->position == POS_SPACE) {
        if (ev->state && fn_down && !off_armed) {
            space_press = copy_raised_zmk_position_state_changed(ev);
            space_captured = true;
            k_work_schedule(&off_hold_work, HOLD);
            return ZMK_EV_EVENT_CAPTURED;
        }
        if (!ev->state) {
            if (space_captured) {
                release_space(); /* short press: send it, then its release */
                return ZMK_EV_EVENT_BUBBLE;
            }
            if (swallow_space_release) {
                swallow_space_release = false;
                return ZMK_EV_EVENT_HANDLED;
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->position == POS_FN) {
        fn_down = ev->state;
        if (!ev->state) {
            release_space(); /* Fn let go before 3 s: Space was a normal press */
            if (off_armed) {
                off_armed = false;
                k_work_submit(&off_work);
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Any other key while Space is held back: send Space first to keep order. */
    if (space_captured && ev->state) {
        release_space();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int on_position(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL || ev->position >= MAX_POS) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    const uint64_t bit = BIT64(ev->position);

    if (confirming) {
        return confirm_event(ev, bit);
    }
    return normal_event(eh, ev, bit);
}

ZMK_LISTENER(bt60_power_confirm, on_position);
ZMK_SUBSCRIPTION(bt60_power_confirm, zmk_position_state_changed);

static int bt60_power_confirm_init(void) {
    const uint32_t mark = nrf_power_gpregret_get(NRF_POWER, MARK_REG);
    const uint32_t reason = nrf_power_resetreas_get(NRF_POWER);
    const bool usb = nrf_power_usbregstatus_vbusdet_get(NRF_POWER);

    /* Use the mark once; a new soft-off writes it again. */
    nrf_power_gpregret_set(NRF_POWER, MARK_REG, 0);
    nrf_power_resetreas_clear(NRF_POWER, reason);

    const bool reset_pin = reason & NRF_POWER_RESETREAS_RESETPIN_MASK;

    LOG_INF("mark 0x%02x reset 0x%08x usb %d", mark, reason, usb);

    if (mark == MARK_VALUE && !reset_pin && !usb) {
        confirming = true;
        k_work_schedule(&timeout_work, K_MSEC(CONFIG_BT60_POWER_CONFIRM_TIMEOUT_MS));
    }
    return 0;
}

SYS_INIT(bt60_power_confirm_init, APPLICATION, 0);
