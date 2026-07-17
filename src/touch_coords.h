/*
 * touch_coords.h -- pure GT911-raw -> frame-pixel coordinate translation.
 *
 * Deliberately free of any ESP-IDF / hardware dependency so it can be unit
 * tested on the host (see test/test_touch_coords.c). The firmware feeds it the
 * GT911's reported raw coordinate, the controller's configured output maximum,
 * the served frame's pixel dimensions, and the panel's orientation flags; it
 * returns a pixel in the frame's coordinate space (the .bin as downloaded,
 * before any panel-side mirror -- the server works in that space).
 */
#pragma once

#include <stdbool.h>

/* Translate one GT911 raw point (rx,ry, range [0,rmax_x]x[0,rmax_y]) into a
 * frame pixel (fx,fy, range [0,frame_w-1]x[0,frame_h-1]).
 *
 * Orientation is applied in this order: invert (mirror an axis end-for-end),
 * then swap (exchange x/y for a 90-degree-mounted digitiser), then scale to the
 * frame. This matches the paint path's orientation handling: set the flags so a
 * touch at the panel's top-left lands at frame (0,0).
 *
 * Robust to zero/negative maxima (treated as no scaling on that axis) and always
 * clamps the result into the frame. Both output pointers must be non-NULL. */
void touch_raw_to_frame(int rx, int ry, int rmax_x, int rmax_y,
                        int frame_w, int frame_h,
                        bool swap_xy, bool invert_x, bool invert_y,
                        int *fx, int *fy);
