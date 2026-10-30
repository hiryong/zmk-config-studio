/*
 * BT60 V2: turn off with Fn+Space held 3 s, turn on only with Fn+Space held 3 s.
 *
 * - Off: the keymap's hold-tap calls &soft_off. Every call of zmk_pm_soft_off()
 *   is wrapped (linker --wrap) so a mark is written to GPREGRET2 first.
 * - Wake: only the Space key can wake the chip from System OFF (see bt60.overlay).
 * - Confirm: if the mark is set, the reset was not the reset pin and USB is not
 *   connected, key events are swallowed until Fn+Space is held for 3 s. If that
 *   does not happen within the timeout, the keyboard goes back to soft-off.
 *   While Fn+Space is being held, the timeout waits for it.
 * - 15 min idle sleep does not call zmk_pm_soft_off(), so it still wakes on any key.
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

static bool confirming;
static bool timed_out;
static bool fn_down;
static bool space_down;
static uint64_t swallowed; /* positions pressed while confirming, release still owed */

static void hold_done(struct k_work *work);
static void timeout_done(struct k_work *work);
static void power_off_now(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(hold_work, hold_done);
static K_WORK_DELAYABLE_DEFINE(timeout_work, timeout_done);
static K_WORK_DEFINE(off_work, power_off_now);

int __real_zmk_pm_soft_off(void);

int __wrap_zmk_pm_soft_off(void) {
    nrf_power_gpregret_set(NRF_POWER, MARK_REG, MARK_VALUE);

    int ret = __real_zmk_pm_soft_off();

    /* Only returns if soft-off failed: do not leave the keyboard locked. */
    LOG_ERR("soft off failed (%d), leaving confirm mode", ret);
    nrf_power_gpregret_set(NRF_POWER, MARK_REG, 0);
    confirming = false;
    k_work_cancel_delayable(&hold_work);
    k_work_cancel_delayable(&timeout_work);
    return ret;
}

static void power_off_now(struct k_work *work) {
    if (!confirming) {
        return;
    }
    LOG_INF("not confirmed, back to soft off");
    zmk_pm_soft_off();
}

static void hold_done(struct k_work *work) {
    if (confirming && fn_down && space_down) {
        LOG_INF("Fn+Space held, starting normally");
        confirming = false;
        k_work_cancel_delayable(&timeout_work);
    }
}

static void timeout_done(struct k_work *work) {
    if (!confirming) {
        return;
    }
    if (fn_down && space_down) {
        /* Combo is being held: wait for it to finish or be released. */
        timed_out = true;
        return;
    }
    k_work_submit(&off_work);
}

static int on_position(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL || ev->position >= MAX_POS) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const uint64_t bit = BIT64(ev->position);

    if (!confirming) {
        /* Swallow releases of keys whose press was swallowed while confirming. */
        if (!ev->state && (swallowed & bit)) {
            swallowed &= ~bit;
            return ZMK_EV_EVENT_HANDLED;
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state) {
        swallowed |= bit;
    } else {
        swallowed &= ~bit;
    }

    if (ev->position == POS_FN) {
        fn_down = ev->state;
    } else if (ev->position == POS_SPACE) {
        space_down = ev->state;
    }

    if (fn_down && space_down) {
        if (!k_work_delayable_is_pending(&hold_work)) {
            k_work_schedule(&hold_work, K_MSEC(CONFIG_BT60_POWER_CONFIRM_HOLD_MS));
        }
    } else {
        k_work_cancel_delayable(&hold_work);
        if (timed_out) {
            k_work_submit(&off_work);
        }
    }

    return ZMK_EV_EVENT_HANDLED;
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
