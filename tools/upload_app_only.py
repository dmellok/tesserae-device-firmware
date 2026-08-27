"""
PlatformIO POST-script: upload the app image only, never the bootloader or the
partition table.

The X4 is a sealed reader whose ESP32-C3 has no exposed BOOT strap, so a unit
that will not enumerate needs the case opened. Keeping the factory bootloader
means a bad app is always recoverable, leaves the vendor SD-card update path
(the only route into a USB-locked unit) intact, and lets the original firmware
go back. It also keeps the stock partition table authoritative, which is why
partitions_xteink_x4.csv transcribes it rather than designing one.

Runs as POST because `pre:` fires before the platform script turns
FLASH_EXTRA_IMAGES into the trailing "<offset> <image>" pairs of UPLOADERFLAGS;
clearing the list early just gets it refilled. Doing it here rather than asking
for `-t app -t upload` (the built-in equivalent) makes the habitual command the
safe one. Use the xteink-x4-full env for a deliberate full flash.

ota_data_initial.bin (0xe000) is KEPT: otadata picks the boot slot, and a device
that has taken an OTA under its previous firmware would otherwise come back up
running the stale one with nothing to explain it. The app offset needs no change
-- ESP32_APP_OFFSET already defaults to 0x10000, where the stock table puts ota_0.
"""

import os

Import("env")

KEEP_OUT = ("bootloader.bin", "partitions.bin")

extras = env.get("FLASH_EXTRA_IMAGES", [])
if extras:
    keep, drop = [], []
    for offset, image in extras:
        path = env.subst(str(image))
        (drop if os.path.basename(path) in KEEP_OUT else keep).append((offset, path))
    if drop:
        # The pairs sit at the tail of UPLOADERFLAGS, two entries each, so
        # rebuild the tail from `keep` rather than filtering individual flags.
        flags = list(env["UPLOADERFLAGS"])[: -2 * len(extras)]
        for offset, path in keep:
            flags += [offset, path]
        env.Replace(UPLOADERFLAGS=flags, FLASH_EXTRA_IMAGES=keep)
        print("upload_app_only: keeping the factory bootloader; not flashing "
              + ", ".join("%s %s" % (o, os.path.basename(p)) for o, p in drop))
