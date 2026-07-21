#include "button_report.h"

#include <stdio.h>

void button_report_set(button_report_t *report, const char *name, uint32_t event_id)
{
    if (name && name[0]) snprintf(report->name, sizeof report->name, "%s", name);
    else                report->name[0] = '\0';
    report->event_id = event_id;
}

bool button_report_pending(const button_report_t *report)
{
    return report->name[0] != '\0';
}

int button_report_append_frame_query(const button_report_t *report,
                                     char *url, size_t url_size, int offset)
{
    if (!button_report_pending(report) || offset <= 0 || (size_t)offset >= url_size)
        return offset;

    int written = snprintf(url + offset, url_size - (size_t)offset,
                           "?button=%s&button_event_id=%u",
                           report->name, (unsigned)report->event_id);
    if (written < 0) return offset;

    size_t next = (size_t)offset + (size_t)written;
    return next >= url_size ? (int)url_size : (int)next;
}

void button_report_finish_frame(button_report_t *report, bool acknowledged)
{
    if (acknowledged) button_report_set(report, NULL, 0);
}
