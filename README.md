# ryong60 ZMK 펌웨어

ryong60(61키 무선, Rule60 v3 6.25u 기반 + 키별 흰색 백라이트) 용 ZMK 설정입니다.
보드 정의는 chcbaram 님의 [zmk-config / zmk-rule60](https://github.com/chcbaram/zmk-config/tree/main/zmk-rule60)(MIT)을 그대로 가져와서 아래만 바꿨습니다.

## 바뀐 점

| 항목 | 내용 |
|---|---|
| 이름 | `CONFIG_ZMK_KEYBOARD_NAME="ryong60"` |
| 백라이트 | P0.03 → PWM0 → AO3400A(저측 스위치) → LED 61개. `zmk,backlight` 등록 |
| 백라이트 기본값 | 처음 켜면 꺼진 상태, 켜면 밝기 40%, 단계 20%, 키 입력이 없으면 자동 꺼짐. PWM 8kHz (가장 어두운 밝기에서도 잔상 없음, P0.03 은 Nordic 권장상 10kHz 이하 핀) |
| RGB 언더글로우 | 사용 안 함 (`CONFIG_ZMK_RGB_UNDERGLOW=n`, `CONFIG_ZMK_EXT_POWER=n`). RGB 전원 스위치는 보드의 풀다운 저항 때문에 기본이 꺼짐이라 RGB LED 대기 전류(약 10mA)도 없음. 다시 쓰려면 `zmk-rule60_defconfig` 안내대로 =y 로 바꾸고 다시 빌드. 그때는 `ryong60_power.c` 가 자동으로 켜져서, RGB 가 꺼진 채 부팅하면 2초 뒤 RGB 전원도 끄고, RGB 켜기 키를 누르면 전원이 다시 켜짐 (두 설정 모두 빌드 확인) |
| 절전 | 15분 동안 입력이 없으면 잠듦 (BT60 V2 설정과 같음), 아무 키나 누르면 깸 |
| 전원 끄기/켜기 | **Fn + Esc 3초** (백라이트가 한 번 깜빡이면 손 떼기). 끈 뒤에는 켤 때도 Fn + Esc 3초 — 아니면 8초 안에 다시 꺼짐. USB 연결·리셋 버튼·슬라이드 스위치로는 바로 켜짐. Fn + Esc 짧게 = ` |
| ZMK Studio | `CONFIG_ZMK_STUDIO=y` (USB 연결로 키 배치 변경) |
| ZMK 버전 | `v0.3` 으로 고정 (보드 폴더가 이 버전 구조에 맞음) |
| 키맵 | 사용자의 BT60 V2 키맵([hiryong/zmk-config-studio](https://github.com/hiryong/zmk-config-studio))을 그대로 옮김 |

## 키 조작 (BT60 V2 와 같음)

- **기본(base)**: Mac 배열 — 아래 줄 Ctrl · Option(Alt) · Cmd · Space · Cmd · Option · Ctrl · **Fn**. Esc 자리는 Shift/GUI 와 함께 누르면 `~`.
- **Fn + Tab**: Windows 배열 켜기/끄기 — 아래 줄 Ctrl · Win · Alt · Space · Alt · 메뉴 · Ctrl · Fn
- **Fn 레이어**
  - Esc = `` ` ``, 1~0, -, = = F1~F12, Backspace = Delete
  - Q / W / E / R = 블루투스 기기 1 / 2 / 3 / 4, T = USB ↔ 블루투스 전환
  - I = Insert, P = Print Screen, `\` = ZMK Studio 잠금 해제, Enter = 부트로더(UF2) 진입
  - Z = 백라이트 켜기/끄기, X / C = 어둡게 / 밝게
  - / = ↑, 오른쪽 Cmd(Alt) / Option(메뉴) / Ctrl 자리 = ← / ↓ / →
- **Fn + 오른쪽 Shift 를 누른 채 (media)**
  - 1 / 2 = 화면 밝기 −/+, 3 = 모든 창 보기, 4 = 검색, 7 / 8 / 9 = 이전 / 재생 / 다음, 0 = 음소거, - / = = 볼륨 −/+
  - A = 블루투스 기기 전부 지우기, D = 지금 기기 지우기
  - / = Page Up, ← ↓ → 자리 = Home / Page Down / End
- Studio 로 언제든 바꿀 수 있습니다: https://zmk.studio (크롬/엣지, USB 연결)

## 빌드 (GitHub Actions, 가장 쉬움)

1. 이 폴더를 GitHub 새 저장소로 올립니다 (폴더 구조 그대로).
2. 저장소의 **Actions** 탭에서 빌드가 자동으로 돌고, 끝나면 `ryong60` 아티팩트(.uf2)가 생깁니다.
3. 빌드 목록은 `build.yaml` (보드 `zmk-rule60`, Studio 용 snippet `studio-rpc-usb-uart`).

> **빌드 확인 완료 (2026-10-29)**: ZMK v0.3 / Zephyr 3.5 / Zephyr SDK 0.16.8 로 이 폴더 그대로 빌드 성공.
> 플래시 27% · RAM 26% 사용, 백라이트 PWM 핀 P0.03 (8kHz), 블루투스 이름 ryong60, 절전 15분, 백라이트 기본 꺼짐, RGB 사용 안 함 확인.
> 그때 만든 펌웨어가 `ryong60.uf2` 로 들어 있어 GitHub 없이 바로 써도 됩니다.

## 처음 한 번: 부트로더 굽기

빈 nRF52840(E73 모듈)에는 부트로더가 없으므로 SWD 로 한 번 구워야 합니다.

1. 부트로더: chcbaram 님 포크 [Adafruit_nRF52_Bootloader](https://github.com/chcbaram/Adafruit_nRF52_Bootloader) 의 `baram_nrf52_zmk` 보드.
2. PCB 의 SWD 헤더(H2: SWDIO / SWCLK / GND / 3V3)에 J-Link, DAPLink, ST-Link(OpenOCD) 등 연결.
3. 부트로더 hex 를 굽고 나면 USB 연결 시 드라이브로 잡힙니다 → `.uf2` 를 끌어다 놓으면 펌웨어 설치 끝.
4. 이후 업데이트: Fn + Enter (부트로더) 또는 리셋 버튼 두 번 → `.uf2` 복사.

## 라이선스

원본 보드 정의와 키맵: MIT, © chcbaram (Hancheol Cho) / 원 출처 nice!60 © Nick Winans — `LICENSE-chcbaram-zmk-config` 참고.
