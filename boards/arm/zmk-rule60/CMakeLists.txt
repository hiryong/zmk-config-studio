# ryong60: 보드 전용 코드 (RGB 전원 기본 끄기, Fn+Esc 3초 전원 끄기/켜기)
# app 대상에 직접 넣어 키 이벤트를 키맵보다 먼저 받게 함 (켜기 확인 중 키 입력이 나가지 않도록)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/ryong60_power.c)
