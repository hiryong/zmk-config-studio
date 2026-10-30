if(CONFIG_BT60_POWER_CONFIRM)
  # Added to the app target (not a separate library) so this listener is linked
  # before ZMK's own sources and sees key events first.
  target_sources(app PRIVATE src/bt60_power_confirm.c)
  # Every soft-off (key or ZMK Studio binding) goes through the wrapper, which
  # leaves a mark so the next wake asks for Fn+Space. Idle sleep does not.
  zephyr_ld_options(-Wl,--wrap=zmk_pm_soft_off)
endif()
