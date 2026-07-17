/* touch_coords.c -- pure coordinate translation. See touch_coords.h. */

#include "touch_coords.h"

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Scale n from [0,in_max] to [0,out_max], rounded, guarding a zero in_max. */
static int scale_axis(int n, int in_max, int out_max)
{
    if (in_max <= 0) return clampi(n, 0, out_max);
    if (n < 0) n = 0;
    if (n > in_max) n = in_max;
    /* +in_max/2 rounds to nearest; use long to avoid overflow on big panels. */
    long scaled = ((long)n * out_max + in_max / 2) / in_max;
    return clampi((int)scaled, 0, out_max);
}

void touch_raw_to_frame(int rx, int ry, int rmax_x, int rmax_y,
                        int frame_w, int frame_h,
                        bool swap_xy, bool invert_x, bool invert_y,
                        int *fx, int *fy)
{
    /* 1. mirror an axis end-for-end (needs the raw max, before any swap). */
    if (invert_x && rmax_x > 0) rx = rmax_x - rx;
    if (invert_y && rmax_y > 0) ry = rmax_y - ry;

    /* 2. exchange axes for a digitiser mounted 90 degrees to the panel. */
    if (swap_xy) {
        int t;
        t = rx; rx = ry; ry = t;
        t = rmax_x; rmax_x = rmax_y; rmax_y = t;
    }

    /* 3. scale each axis into the frame's pixel space (max index = dim - 1). */
    *fx = scale_axis(rx, rmax_x, frame_w > 0 ? frame_w - 1 : 0);
    *fy = scale_axis(ry, rmax_y, frame_h > 0 ? frame_h - 1 : 0);
}
