# ryong60

61-key wireless 60% PCB (GH60 size), based on chcbaram's Rule60 v3 and ZMK's nice!60 board files.

Build target: `ryong60//zmk` (Zephyr hardware model v2, ZMK main).
Board-specific code (soft off: Fn wakes it, Fn + the Soft Off key turns it on; RGB power) lives in `ryong60_power.c`,
added to the build by the repository's top-level `CMakeLists.txt`.
