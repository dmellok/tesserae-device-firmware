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

static void test_invalid_offsets_leave_url_untouched(void)
{
    button_report_t report = {0};
    char url[32] = "https://host/frame";
    char original[sizeof url];
    memcpy(original, url, sizeof url);
    button_report_set(&report, "refresh", 7);

    assert(button_report_append_frame_query(&report, url, sizeof url, 0) == 0);
    assert(memcmp(url, original, sizeof url) == 0);

    assert(button_report_append_frame_query(&report, url, sizeof url, -1) == -1);
    assert(memcmp(url, original, sizeof url) == 0);

    assert(button_report_append_frame_query(&report, url, sizeof url,
                                            (int)sizeof url) == (int)sizeof url);
    assert(memcmp(url, original, sizeof url) == 0);

    assert(button_report_append_frame_query(&report, url, sizeof url,
                                            (int)sizeof url + 1) == (int)sizeof url + 1);
    assert(memcmp(url, original, sizeof url) == 0);
}

static void test_truncated_query_returns_bounded_offset(void)
{
    button_report_t report = {0};
    char url[24] = "https://host/frame";
    int offset = (int)strlen(url);
    button_report_set(&report, "refresh", 7);

    int next = button_report_append_frame_query(&report, url, sizeof url, offset);

    assert(next == (int)sizeof url);
    assert(next >= offset);
    assert((size_t)next <= sizeof url);
    assert(url[sizeof url - 1] == '\0');
    assert(strncmp(url, "https://host/frame", (size_t)offset) == 0);
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
    test_invalid_offsets_leave_url_untouched();
    test_truncated_query_returns_bounded_offset();
    test_pre_ack_failure_retains_status_fallback();
    test_acknowledged_frame_clears_before_status();
    puts("button_report tests passed");
    return 0;
}
