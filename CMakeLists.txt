# ryong60 board code (soft off / wake with Fn+Space, RGB power).
# Added to the app target (not a separate library) so its key listener is linked
# before ZMK's own sources and sees key events first while confirming power-on.
if(CONFIG_BOARD_RYONG60)
  target_sources(app PRIVATE boards/ryong/ryong60/ryong60_power.c)
endif()
