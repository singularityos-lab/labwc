/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_APP_PAUSE_H
#define LABWC_APP_PAUSE_H

#include <stdbool.h>

struct view;

void app_pause_init(void);
void app_pause_finish(void);
void app_pause_view_mapped(struct view *view);
void app_pause_view_unmapped(struct view *view);
bool app_pause_view_is_paused(struct view *view);
void app_pause_request_resume(struct view *view);

#endif /* LABWC_APP_PAUSE_H */
