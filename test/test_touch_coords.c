/*
 * Host-side unit test for the pure GT911-raw -> frame-pixel translation.
 *
 * No ESP-IDF / hardware needed. Build and run on the host:
 *
 *   cc -I src test/test_touch_coords.c src/touch_coords.c -o /tmp/tt && /tmp/tt
 *
 * Exits non-zero (via assert) on the first failure; prints "touch_coords: N
 * checks passed" on success.
 */
#include "touch_coords.h"

#include <assert.h>
#include <stdio.h>

static int checks = 0;

static void expect(int rx, int ry, int rmax_x, int rmax_y, int fw, int fh,
                   int swap, int inv_x, int inv_y, int want_x, int want_y)
{
    int gx = -1, gy = -1;
    touch_raw_to_frame(rx, ry, rmax_x, rmax_y, fw, fh, swap, inv_x, inv_y, &gx, &gy);
    if (gx != want_x || gy != want_y) {
        fprintf(stderr,
                "FAIL raw=(%d,%d) max=(%d,%d) frame=%dx%d swap=%d invx=%d invy=%d"
                " => (%d,%d), want (%d,%d)\n",
                rx, ry, rmax_x, rmax_y, fw, fh, swap, inv_x, inv_y,
                gx, gy, want_x, want_y);
    }
    assert(gx == want_x && gy == want_y);
    checks++;
}

int main(void)
{
    const int W = 1872, H = 1404;   /* E1003 frame */

    /* Identity: GT911 max == frame dims, no orientation. Corners + centre. */
    expect(0, 0, W, H, W, H, 0, 0, 0, 0, 0);
    expect(W, H, W, H, W, H, 0, 0, 0, W - 1, H - 1);
    expect(W / 2, H / 2, W, H, W, H, 0, 0, 0, (W - 1) / 2 + 1, (H - 1) / 2 + 1);

    /* Scale up: controller reports in a smaller native range. */
    expect(0,    0,    936, 702, W, H, 0, 0, 0, 0,       0);
    expect(936,  702,  936, 702, W, H, 0, 0, 0, W - 1,   H - 1);
    expect(468,  351,  936, 702, W, H, 0, 0, 0, 936,     702);   /* midpoint */

    /* Invert X: raw 0 -> right edge, raw max -> left edge. */
    expect(0, 0, W, H, W, H, 0, 1, 0, W - 1, 0);
    expect(W, 0, W, H, W, H, 0, 1, 0, 0,     0);

    /* Invert Y: raw 0 -> bottom edge. */
    expect(0, 0, W, H, W, H, 0, 0, 1, 0, H - 1);
    expect(0, H, W, H, W, H, 0, 0, 1, 0, 0);

    /* Swap XY: a portrait digitiser (1404x1872) onto a landscape frame. A raw
     * point at (rmax_x, 0) lands at frame (0, H-1) after the axis exchange. */
    expect(1404, 0, 1404, 1872, W, H, 1, 0, 0, 0, H - 1);
    expect(0, 1872, 1404, 1872, W, H, 1, 0, 0, W - 1, 0);

    /* Clamp: raw beyond the configured max is pinned to the frame edge. */
    expect(9999, 9999, W, H, W, H, 0, 0, 0, W - 1, H - 1);
    expect(-5,   -5,   W, H, W, H, 0, 0, 0, 0,     0);

    /* Degenerate max: no scaling, but still clamped into the frame. */
    expect(50, 40, 0, 0, W, H, 0, 0, 0, 50, 40);

    printf("touch_coords: %d checks passed\n", checks);
    return 0;
}
