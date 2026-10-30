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
 * ryong60: 전원 끄기 / 켜기 = Fn + Esc 3초
 *
 *  끄기: 키맵의 gr_off(Fn 층 Esc 자리)가 3초 넘게 눌린 뒤 떼면 ZMK soft off.
 *        이 파일은 그 순간 '키로 끈 것' 표시(GPREGRET2)를 남김 — 15분 자동 절전과 구분하려고.
 *  켜기: 끈 뒤에는 Esc 하나만 칩을 깨움(dts). 깨어났을 때 표시가 있으면
 *        Fn+Esc 가 3초 동안 눌려 있어야 켜진 채로 둠. 아니면(가방 속에서 Esc 가 눌린 경우 등) 8초 안에 다시 끔.
 *        확인하는 동안 키 입력은 컴퓨터로 보내지 않음.
 *  예외: USB 를 꽂아서 깨거나, 리셋 버튼, 슬라이드 스위치로 껐다 켜면 바로 켜짐.
 *  3초가 되는 순간 백라이트가 한 번 깜빡임 → 손을 떼면 됨.
 */
#include <zephyr/drivers/led.h>
#include <hal/nrf_power.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/pm.h>
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT)
#include <zmk/backlight.h>
#endif

#define R60_POS_ESC 0       /* 키 위치 번호: Esc (맨 위 맨 왼쪽) */
#define R60_POS_FN 60       /* 키 위치 번호: Fn (맨 아래 맨 오른쪽) */
#define R60_HOLD_MS 3000
#define R60_GUARD_MS 8000
#define R60_FLAG 0xA4       /* GPREGRET2 표시값 (0번 비트는 비움: 부트로더 CRC 건너뛰기 비트와 겹치지 않게) */

static bool r60_fn, r60_esc;          /* 지금 눌려 있는지 */
static int64_t r60_esc_t = -1;        /* Fn 을 누른 상태에서 Esc 를 누른 시각 */
static bool r60_guard, r60_ok;
static int r60_held;                  /* 확인 중에 눌려 있는 키 수 */

static void r60_set_flag(void) { nrf_power_gpregret_set(NRF_POWER, 1, R60_FLAG); }

/* 표시를 남겼는데 1초 뒤에도 아직 켜져 있으면(꺼지지 않았으면) 표시를 지움 → 자동 절전에서 깰 때 영향 없게 */
static void r60_flag_clear(struct k_work *w) { nrf_power_gpregret_set(NRF_POWER, 1, 0); }
static K_WORK_DELAYABLE_DEFINE(r60_flag_clear_work, r60_flag_clear);

static void r60_blink_end(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(r60_blink_end_work, r60_blink_end);

static void r60_blink(void) {
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT) && DT_HAS_CHOSEN(zmk_backlight)
    const struct device *bl = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight));
    if (!device_is_ready(bl)) {
        return;
    }
    led_set_brightness(bl, 0, zmk_backlight_is_on() ? 0 : 60);
    k_work_reschedule(&r60_blink_end_work, K_MSEC(250));
#endif
}

static void r60_blink_end(struct k_work *w) {
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT) && DT_HAS_CHOSEN(zmk_backlight)
    const struct device *bl = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight));
    if (device_is_ready(bl)) {
        led_set_brightness(bl, 0, zmk_backlight_is_on() ? zmk_backlight_get_brt() : 0);
    }
#endif
}

/* 3초 지점: 아직 Fn+Esc 가 눌려 있으면 깜빡임 (끌 때·켤 때 모두) */
static void r60_hold_reached(struct k_work *w) {
    if (r60_fn && r60_esc) {
        if (r60_guard) {
            r60_ok = true; /* 켜기 확인 완료 */
        }
        r60_blink();
    }
}
static K_WORK_DELAYABLE_DEFINE(r60_hold_work, r60_hold_reached);

/* 확인 시간 초과 → 다시 끔.
 * Esc 가 계속 눌려 있으면(가방 속에서 눌린 채 등) 바로 끄면 즉시 다시 깨어나 재부팅을 반복하므로, 뗄 때까지 기다렸다가 끔 */
static void r60_guard_timeout(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(r60_guard_work, r60_guard_timeout);
static void r60_guard_timeout(struct k_work *w) {
    if (!r60_guard || r60_ok) {
        return;
    }
    if (r60_esc) {
        k_work_reschedule(&r60_guard_work, K_MSEC(500));
        return;
    }
    r60_set_flag();
    if (zmk_pm_soft_off() != 0) {
        /* 끄기에 실패하면 키가 계속 막히지 않도록 정상 동작으로 되돌림 */
        nrf_power_gpregret_set(NRF_POWER, 1, 0);
        r60_guard = false;
    }
}

/* 이 리스너는 CMakeLists 에서 app 대상의 맨 앞에 링크되어 키맵·hold-tap 보다 먼저 키 이벤트를 받음
 * (ZMK 이벤트 순서 = 링크 순서. 빌드 후 zmk.elf 의 .event_subscription 첫 항목인지 확인함) */
static int r60_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    bool down = ev->state;
    if (ev->position == R60_POS_FN) {
        r60_fn = down;
    }
    if (ev->position == R60_POS_ESC) {
        if (down) {
            r60_esc_t = r60_fn ? ev->timestamp : -1;
        } else if (!r60_guard && r60_esc_t >= 0 && ev->timestamp - r60_esc_t > R60_HOLD_MS) {
            /* 곧 키맵의 gr_off 가 soft off 를 부름 → 키로 끈 것 표시 */
            r60_set_flag();
            k_work_reschedule(&r60_flag_clear_work, K_MSEC(1000));
        }
        r60_esc = down;
    }
    if (r60_fn && r60_esc && (ev->position == R60_POS_FN || ev->position == R60_POS_ESC) && down) {
        k_work_reschedule(&r60_hold_work, K_MSEC(R60_HOLD_MS));
    } else if (!(r60_fn && r60_esc)) {
        k_work_cancel_delayable(&r60_hold_work);
    }

    if (!r60_guard) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    /* 켜기 확인 중: 모든 키 입력을 삼킴, 확인이 끝나고 모든 키를 뗀 뒤부터 정상 동작 */
    r60_held += down ? 1 : -1;
    if (r60_held < 0) {
        r60_held = 0;
    }
    if (r60_ok && r60_held == 0) {
        r60_guard = false;
        k_work_cancel_delayable(&r60_guard_work);
    }
    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(a_ryong60_power_key, r60_listener);
ZMK_SUBSCRIPTION(a_ryong60_power_key, zmk_position_state_changed);

static int r60_wake_check(void) {
    uint32_t why = nrf_power_resetreas_get(NRF_POWER);
    nrf_power_resetreas_clear(NRF_POWER, why);
    bool flag = nrf_power_gpregret_get(NRF_POWER, 1) == R60_FLAG;
    nrf_power_gpregret_set(NRF_POWER, 1, 0);

    /* USB 가 꽂혀 있으면(VBUS 감지) 확인 없이 바로 켬 — 이 시점엔 ZMK USB 상태가 아직 준비 전이라 레지스터를 직접 읽음 */
    bool usb = nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
    bool manual = why & (NRF_POWER_RESETREAS_RESETPIN_MASK | NRF_POWER_RESETREAS_VBUS_MASK);
    if (flag && !usb && !manual) {
        r60_guard = true;
        k_work_schedule(&r60_guard_work, K_MSEC(R60_GUARD_MS));
    }
    return 0;
}

SYS_INIT(r60_wake_check, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
