/* Pending physical-button report shared by /frame and /status. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char name[16];
    uint32_t event_id;
} button_report_t;

void button_report_set(button_report_t *report, const char *name, uint32_t event_id);
bool button_report_pending(const button_report_t *report);

/* Append the canonical frame query to an existing URL. Returns the resulting
 * logical length (snprintf semantics); a non-pending report leaves it alone. */
int button_report_append_frame_query(const button_report_t *report,
                                     char *url, size_t url_size, int offset);

/* An acknowledged /frame response means the server received and dispatched
 * the event, so /status must not submit it again. A pre-ack failure retains it
 * for /status as the fallback delivery path. */
void button_report_finish_frame(button_report_t *report, bool acknowledged);
