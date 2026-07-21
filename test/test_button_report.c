#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "button_report.h"

static void test_uses_canonical_event_parameter(void)
{
    button_report_t report = {0};
    char url[160] = "https://host/api/v1/device/panel/frame";
    int offset = (int)strlen(url);

    button_report_set(&report, "refresh", 7);
    int written = button_report_append_frame_query(&report, url, sizeof url, offset);

    assert(written == (int)strlen(url));
    assert(strcmp(url,
                  "https://host/api/v1/device/panel/frame"
                  "?button=refresh&button_event_id=7") == 0);
}

static void test_pre_ack_failure_retains_status_fallback(void)
{
    button_report_t report = {0};
    button_report_set(&report, "left", 8);

    button_report_finish_frame(&report, false);

    assert(button_report_pending(&report));
    assert(strcmp(report.name, "left") == 0);
    assert(report.event_id == 8);
}

static void test_acknowledged_frame_clears_before_status(void)
{
    button_report_t report = {0};
    char url[80] = "https://host/frame";
    int offset = (int)strlen(url);
    button_report_set(&report, "right", 9);

    button_report_finish_frame(&report, true);

    assert(!button_report_pending(&report));
    assert(report.event_id == 0);
    assert(button_report_append_frame_query(&report, url, sizeof url, offset) == offset);
    assert(strcmp(url, "https://host/frame") == 0);
}

int main(void)
{
    test_uses_canonical_event_parameter();
    test_pre_ack_failure_retains_status_fallback();
    test_acknowledged_frame_clears_before_status();
    puts("button_report tests passed");
    return 0;
}
