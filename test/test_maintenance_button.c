// SPDX-License-Identifier: AGPL-3.0-or-later
#include <assert.h>
#include <stdio.h>
#include "maintenance_button.h"

static void check_hold(uint32_t duration, bool exits)
{
    maintenance_button_t b = maintenance_button_init(false, 0);
    assert(!maintenance_button_poll(&b, false, 50));
    assert(!maintenance_button_poll(&b, true, 100));
    assert(!maintenance_button_poll(&b, true, 150));
    assert(!maintenance_button_poll(&b, false, 100 + duration));
    assert(maintenance_button_poll(&b, false, 150 + duration) == exits);
    assert(!maintenance_button_poll(&b, false, 200 + duration));
}

int main(void)
{
    check_hold(50, true);
    check_hold(200, true);
    check_hold(2999, true);
    check_hold(3000, false);
    check_hold(5000, false);
    check_hold(20000, false);

    // Enter with the activation button still down: even a very long hold and
    // its release only arm the next press, never exit or request a reset.
    maintenance_button_t b = maintenance_button_init(true, 0);
    assert(!maintenance_button_poll(&b, true, 30000));
    assert(!maintenance_button_poll(&b, false, 30020));
    assert(!maintenance_button_poll(&b, false, 30070));
    assert(!maintenance_button_poll(&b, true, 30100));
    assert(!maintenance_button_poll(&b, true, 30150));
    assert(!maintenance_button_poll(&b, false, 30300));
    assert(maintenance_button_poll(&b, false, 30350));

    // A release glitch on the original entry hold must not arm the exit.
    b = maintenance_button_init(true, 0);
    assert(!maintenance_button_poll(&b, false, 100));
    assert(!maintenance_button_poll(&b, true, 120));
    assert(!maintenance_button_poll(&b, true, 200));
    assert(!maintenance_button_poll(&b, false, 300));
    assert(!maintenance_button_poll(&b, false, 350));

    // Sub-debounce pulses and release bounce are ignored; exit fires once,
    // only after both a stable press and a stable release.
    b = maintenance_button_init(false, 0);
    assert(!maintenance_button_poll(&b, false, 50));
    assert(!maintenance_button_poll(&b, true, 100));
    assert(!maintenance_button_poll(&b, false, 149));
    assert(!maintenance_button_poll(&b, false, 200));
    assert(!maintenance_button_poll(&b, true, 300));
    assert(!maintenance_button_poll(&b, true, 350));
    assert(!maintenance_button_poll(&b, false, 400));
    assert(!maintenance_button_poll(&b, true, 420));
    assert(!maintenance_button_poll(&b, false, 440));
    assert(!maintenance_button_poll(&b, false, 489));
    assert(maintenance_button_poll(&b, false, 490));
    assert(!maintenance_button_poll(&b, false, 1000));

    // A long hold doesn't poison the next short press.
    b = maintenance_button_init(false, 0);
    assert(!maintenance_button_poll(&b, false, 50));
    assert(!maintenance_button_poll(&b, true, 100));
    assert(!maintenance_button_poll(&b, true, 150));
    assert(!maintenance_button_poll(&b, false, 5100));
    assert(!maintenance_button_poll(&b, false, 5150));
    assert(!maintenance_button_poll(&b, true, 5200));
    assert(!maintenance_button_poll(&b, true, 5250));
    assert(!maintenance_button_poll(&b, false, 5400));
    assert(maintenance_button_poll(&b, false, 5450));

    // Millisecond counter wrap does not change debounce or hold duration.
    b = maintenance_button_init(false, UINT32_MAX - 200);
    assert(!maintenance_button_poll(&b, false, UINT32_MAX - 150));
    assert(!maintenance_button_poll(&b, true, UINT32_MAX - 100));
    assert(!maintenance_button_poll(&b, true, UINT32_MAX - 50));
    assert(!maintenance_button_poll(&b, false, 100));
    assert(maintenance_button_poll(&b, false, 150));
    puts("maintenance_button: release arming, debounce, short/long holds and clock wrap passed");
    return 0;
}
