/*
 * BT60 V2: turn back on with the same key combo that turned it off.
 *
 * - Off: ZMK's own Soft Off behavior, placed on any key with ZMK Studio (for
 *   example on the Fn layer). bt60.keymap sets its official hold-time-ms to 3 s.
 *   If Soft Off fires while Fn is still held, sleep waits until Fn is released,
 *   because Fn is the wake key and would wake the chip again at once.
 * - Wake: only Fn (bottom-right key) wakes the chip from System OFF
 *   (see bt60.overlay). This is wiring, so Fn must stay in that spot.
 * - On: after a Soft Off, the next wake looks up the current keymap (ZMK Studio
 *   changes included) for the key(s) holding Soft Off, and waits for Fn plus
 *   that key held together for 3 s (any order). Key input is not sent while
 *   waiting. If that does not happen within the timeout the keyboard turns off
 *   again; while the keys are held the timeout waits.
 * - USB power or the reset button start normally. 15 min idle sleep does not go
 *   through zmk_pm_soft_off(), so it still wakes on any key.
 * - zmk_pm_soft_off() is wrapped (linker --wrap) to leave a mark in GPREGRET2.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/pm.h>

LOG_MODULE_REGISTER(bt60_power_confirm, CONFIG_ZMK_LOG_LEVEL);

#define MARK_REG 1 /* GPREGRET2 (GPREGRET is used by the bootloader) */
#define MARK_VALUE 0xB6

#define POS_FN CONFIG_BT60_POWER_CONFIRM_FN_POSITION
#define POS_FALLBACK CONFIG_BT60_POWER_CONFIRM_SPACE_POSITION
#define MAX_POS 64

static bool confirming;
static bool timed_out;
static uint64_t swallowed; /* keys pressed while confirming; their release is swallowed too */
static uint64_t held;      /* keys held while confirming */

static bool fn_down;     /* Fn state outside confirm mode */
static bool off_pending; /* Soft Off requested while Fn was held */

static void hold_done(struct k_work *work);
static void timeout_done(struct k_work *work);
static void power_off(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(hold_work, hold_done);
static K_WORK_DELAYABLE_DEFINE(timeout_work, timeout_done);
static K_WORK_DEFINE(off_work, power_off);

int __real_zmk_pm_soft_off(void);

int __wrap_zmk_pm_soft_off(void) {
    if (!confirming && fn_down) {
        LOG_INF("soft off waits for Fn release");
        off_pending = true;
        return 0;
    }

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

static void power_off(struct k_work *work) { zmk_pm_soft_off(); }

/* Key positions holding ZMK's Soft Off on any layer of the current keymap. */
static uint64_t soft_off_keys(void) {
    uint64_t mask = 0;
#if DT_NODE_EXISTS(DT_NODELABEL(soft_off))
    const struct device *soft_off_dev = DEVICE_DT_GET(DT_NODELABEL(soft_off));
    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(l);
        if (id == ZMK_KEYMAP_LAYER_ID_INVAL) {
            continue;
        }
        for (int p = 0; p < ZMK_KEYMAP_LEN && p < MAX_POS; p++) {
            const struct zmk_behavior_binding *b = zmk_keymap_get_layer_binding_at_idx(id, p);
            if (b != NULL && b->behavior_dev != NULL &&
                zmk_behavior_get_binding(b->behavior_dev) == soft_off_dev) {
                mask |= BIT64(p);
            }
        }
    }
#endif
    mask &= ~BIT64(POS_FN);
    if (mask == 0) {
        mask = BIT64(POS_FALLBACK);
    }
    return mask;
}

static bool combo_held(void) { return (held & BIT64(POS_FN)) && (held & soft_off_keys()); }

static void hold_done(struct k_work *work) {
    if (confirming && combo_held()) {
        LOG_INF("Fn + Soft Off key held, starting normally");
        confirming = false;
        k_work_cancel_delayable(&timeout_work);
    }
}

static void timeout_done(struct k_work *work) {
    if (!confirming) {
        return;
    }
    if (combo_held()) {
        timed_out = true; /* wait for the hold to finish or be released */
        return;
    }
    LOG_INF("not confirmed, back to soft off");
    k_work_submit(&off_work);
}

static int on_position(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL || ev->position >= MAX_POS) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    const uint64_t bit = BIT64(ev->position);

    if (!confirming) {
        if (!ev->state && (swallowed & bit)) {
            /* Release of a key whose press was swallowed while turning on. */
            swallowed &= ~bit;
            return ZMK_EV_EVENT_HANDLED;
        }
        if (ev->position == POS_FN) {
            fn_down = ev->state;
            if (!ev->state && off_pending) {
                off_pending = false;
                k_work_submit(&off_work);
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state) {
        swallowed |= bit;
        held |= bit;
    } else {
        swallowed &= ~bit;
        held &= ~bit;
    }

    if (combo_held()) {
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
