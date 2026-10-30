# ryong60 board code (soft off: wake with Fn, turn on with Fn + the Soft Off key; RGB power).
# Added to the app target (not a separate library) so its key listener is linked
# before ZMK's own sources and sees key events first while confirming power-on.
if(CONFIG_BOARD_RYONG60)
  target_sources(app PRIVATE boards/ryong/ryong60/ryong60_power.c)
  # Every soft off (key or ZMK Studio binding) goes through the wrapper in ryong60_power.c, which leaves a
  # mark so the next wake asks for Fn + the Soft Off key. 15 min idle sleep does not.
  zephyr_ld_options(-Wl,--wrap=zmk_pm_soft_off)
endif()
