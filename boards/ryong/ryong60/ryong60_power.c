/*
 * ryong60: RGB 언더글로우를 켜서 쓰게 될 때(나중에 CONFIG_ZMK_RGB_UNDERGLOW=y,
 * CONFIG_ZMK_EXT_POWER=y) 대비한 코드. 지금 설정(=n)에서는 아무 것도 컴파일되지 않음.
 *
 * ZMK 는 언더글로우를 '꺼진 상태로 시작'해도 RGB 전원(EXT_POWER)은 켜 두므로
 * 불은 안 들어와도 RGB LED 14개가 대기 전류(약 10mA)를 씀.
 * 부팅 2초 뒤(설정을 다 읽은 뒤) 언더글로우가 꺼져 있으면 RGB 전원도 끔.
 * RGB 켜기 키(&rgb_ug RGB_TOG / RGB_ON)를 누르면 ZMK 가 전원을 자동으로 다시 켬.
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW) && IS_ENABLED(CONFIG_ZMK_EXT_POWER)
#include <drivers/ext_power.h>
#include <zmk/rgb_underglow.h>

static void ryong60_ext_power_check(struct k_work *work) {
    bool on = false;
    if (zmk_rgb_underglow_get_state(&on) != 0 || on) {
        return;
    }
    const struct device *ext = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
    if (device_is_ready(ext) && ext_power_get(ext) > 0) {
        ext_power_disable(ext);
    }
}

static K_WORK_DELAYABLE_DEFINE(ryong60_ext_power_work, ryong60_ext_power_check);

static int ryong60_power_init(void) {
    k_work_schedule(&ryong60_ext_power_work, K_SECONDS(2));
    return 0;
}

SYS_INIT(ryong60_power_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif

/*
 * ryong60: 전원 끄기 / 켜기 (BT60 V2 Oreo·Cheeto 의 bt60_power_confirm.c 와 같은 방식)
 *
 *  끄기: ZMK 공식 Soft Off. 키맵에서는 Fn + 왼쪽 Ctrl 자리. ZMK Studio 로 아무 키에나 옮길 수 있음.
 *        누르고 있어야 하는 시간은 Soft Off 의 hold-time-ms (key.ryong.com 설정 탭).
 *        Fn 을 누른 채로 꺼지면 Fn 을 뗄 때까지 기다렸다가 끔 — Fn 이 깨우는 키라서 바로 다시 깨어나지 않게.
 *  깨우기: 끈 뒤에는 깨우기 키(기본 Fn) 하나만 칩을 깨움 (ryong-wake.dtsi, key.ryong.com 에서 바꿈).
 *  켜기: 깨어나면 지금 키맵(ZMK Studio 로 바꾼 것 포함)에서 Soft Off 가 있는 키를 찾아,
 *        Fn + 그 키를 함께 3초 누르면 켜짐 (누르는 순서 상관없음). 그동안 키 입력은 컴퓨터로 보내지 않음.
 *        8초 안에 안 되면 다시 꺼짐. 키가 눌려 있는 동안은 기다림. 3초가 되는 순간 백라이트가 한 번 깜빡임.
 *  예외: USB 를 꽂아서 깨거나 리셋 버튼이면 바로 켜짐. 15분 자동 절전은 zmk_pm_soft_off() 를 거치지 않으므로
 *        예전처럼 아무 키로나 깨어남.
 *  모든 soft off 는 링커 --wrap 으로 감싸서 GPREGRET2 에 표시를 남김 (CMakeLists.txt).
 */
#include <zephyr/logging/log.h>
#include <zephyr/drivers/led.h>
#include <hal/nrf_power.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/pm.h>
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT)
#include <zmk/backlight.h>
#endif

LOG_MODULE_REGISTER(ryong60_power, CONFIG_ZMK_LOG_LEVEL);

#define R60_POS_FN CONFIG_RYONG_WAKE_POSITION /* 깨우는 키 (기본 Fn, 맨 아래 맨 오른쪽) — key.ryong.com 에서 바꿈 */
#define R60_POS_FALLBACK 53 /* 왼쪽 Ctrl: 키맵에 Soft Off 가 하나도 없을 때 대신 쓰는 키 */
#define R60_HOLD_MS 3000
#define R60_TIMEOUT_MS 8000
#define R60_MARK_REG 1      /* GPREGRET2 (GPREGRET 은 부트로더가 씀) */
#define R60_MARK 0xA4
#define R60_MAX_POS 64

static bool confirming;
static bool timed_out;
static uint64_t swallowed; /* 확인 중에 눌린 키 — 뗄 때도 삼킴 */
static uint64_t held;      /* 확인 중에 눌려 있는 키 */
static bool fn_down;       /* 평소 Fn 상태 */
static bool off_pending;   /* Fn 을 누른 채 꺼달라고 한 경우 */

static void hold_done(struct k_work *work);
static void timeout_done(struct k_work *work);
static void power_off(struct k_work *work);
static void recheck(struct k_work *work);
static void blink_end(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(hold_work, hold_done);
static K_WORK_DELAYABLE_DEFINE(recheck_work, recheck);
static K_WORK_DELAYABLE_DEFINE(timeout_work, timeout_done);
static K_WORK_DELAYABLE_DEFINE(blink_work, blink_end);
static K_WORK_DEFINE(off_work, power_off);

static void blink(void) {
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT) && DT_HAS_CHOSEN(zmk_backlight)
    const struct device *bl = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight));
    if (device_is_ready(bl)) {
        led_set_brightness(bl, 0, zmk_backlight_is_on() ? 0 : 60);
        k_work_reschedule(&blink_work, K_MSEC(250));
    }
#endif
}

static void blink_end(struct k_work *work) {
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT) && DT_HAS_CHOSEN(zmk_backlight)
    const struct device *bl = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight));
    if (device_is_ready(bl)) {
        led_set_brightness(bl, 0, zmk_backlight_is_on() ? zmk_backlight_get_brt() : 0);
    }
#endif
}

int __real_zmk_pm_soft_off(void);

int __wrap_zmk_pm_soft_off(void) {
    if (!confirming && fn_down) {
        LOG_INF("soft off waits for Fn release");
        off_pending = true;
        return 0;
    }
    nrf_power_gpregret_set(NRF_POWER, R60_MARK_REG, R60_MARK);
    int ret = __real_zmk_pm_soft_off();
    /* 돌아왔다면 끄기에 실패한 것: 키보드가 막히지 않게 정상 동작으로 */
    LOG_ERR("soft off failed (%d)", ret);
    nrf_power_gpregret_set(NRF_POWER, R60_MARK_REG, 0);
    confirming = false;
    k_work_cancel_delayable(&hold_work);
    k_work_cancel_delayable(&timeout_work);
    return ret;
}

static void power_off(struct k_work *work) { zmk_pm_soft_off(); }

/* 지금 키맵의 모든 층에서 Soft Off 가 있는 키 위치 */
static uint64_t soft_off_keys(void) {
    uint64_t mask = 0;
#if DT_NODE_EXISTS(DT_NODELABEL(soft_off))
    const struct device *soft_off_dev = DEVICE_DT_GET(DT_NODELABEL(soft_off));
    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(l);
        if (id == ZMK_KEYMAP_LAYER_ID_INVAL) {
            continue;
        }
        for (int p = 0; p < ZMK_KEYMAP_LEN && p < R60_MAX_POS; p++) {
            const struct zmk_behavior_binding *b = zmk_keymap_get_layer_binding_at_idx(id, p);
            if (b != NULL && b->behavior_dev != NULL && zmk_behavior_get_binding(b->behavior_dev) == soft_off_dev) {
                mask |= BIT64(p);
            }
        }
    }
#endif
    mask &= ~BIT64(R60_POS_FN);
    return mask ? mask : BIT64(R60_POS_FALLBACK);
}

static bool combo_held(void) { return (held & BIT64(R60_POS_FN)) && (held & soft_off_keys()); }

/* 키를 누르고 뗄 때마다, 그리고 확인 중 200ms 마다 — Studio 로 저장한 키맵이 늦게 읽혀도 맞게 */
static void update_hold(void) {
    if (combo_held()) {
        if (!k_work_delayable_is_pending(&hold_work)) {
            k_work_schedule(&hold_work, K_MSEC(R60_HOLD_MS));
        }
    } else {
        k_work_cancel_delayable(&hold_work);
        if (timed_out) {
            k_work_submit(&off_work);
        }
    }
}

static void recheck(struct k_work *work) {
    if (!confirming) {
        return;
    }
    update_hold();
    k_work_schedule(&recheck_work, K_MSEC(200));
}

static void hold_done(struct k_work *work) {
    if (confirming && combo_held()) {
        LOG_INF("Fn + Soft Off key held, starting normally");
        confirming = false;
        k_work_cancel_delayable(&timeout_work);
        k_work_cancel_delayable(&recheck_work);
        blink();
    }
}

static void timeout_done(struct k_work *work) {
    if (!confirming) {
        return;
    }
    if (combo_held()) {
        timed_out = true; /* 누르고 있는 중이면 끝날 때까지 기다림 */
        update_hold();
        return;
    }
    LOG_INF("not confirmed, back to soft off");
    k_work_submit(&off_work);
}

/* CMakeLists 에서 app 대상에 직접 넣어 ZMK 소스보다 먼저 링크됨 → 키 이벤트를 키맵보다 먼저 받음 */
static int on_position(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL || ev->position >= R60_MAX_POS) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    const uint64_t bit = BIT64(ev->position);

    if (!confirming) {
        if (!ev->state && (swallowed & bit)) {
            swallowed &= ~bit; /* 켜는 동안 눌렀던 키를 뗀 것 */
            return ZMK_EV_EVENT_HANDLED;
        }
        if (ev->position == R60_POS_FN) {
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
    update_hold();
    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(a_ryong60_power, on_position);
ZMK_SUBSCRIPTION(a_ryong60_power, zmk_position_state_changed);

static int ryong60_power_confirm_init(void) {
    const uint32_t mark = nrf_power_gpregret_get(NRF_POWER, R60_MARK_REG);
    const uint32_t reason = nrf_power_resetreas_get(NRF_POWER);
    const bool usb = nrf_power_usbregstatus_vbusdet_get(NRF_POWER);

    nrf_power_gpregret_set(NRF_POWER, R60_MARK_REG, 0); /* 표시는 한 번만 씀 */
    nrf_power_resetreas_clear(NRF_POWER, reason);

    const bool manual = reason & (NRF_POWER_RESETREAS_RESETPIN_MASK | NRF_POWER_RESETREAS_VBUS_MASK);
    LOG_INF("mark 0x%02x reset 0x%08x usb %d", mark, reason, usb);

    if (mark == R60_MARK && !manual && !usb) {
        confirming = true;
        k_work_schedule(&timeout_work, K_MSEC(R60_TIMEOUT_MS));
        k_work_schedule(&recheck_work, K_MSEC(200));
    }
    return 0;
}

SYS_INIT(ryong60_power_confirm_init, APPLICATION, 0);
