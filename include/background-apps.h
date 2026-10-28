/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_BACKGROUND_APPS_H
#define LABWC_BACKGROUND_APPS_H

struct view;

void background_apps_init(void);
void background_apps_finish(void);
void background_apps_view_unmapped(struct view *view);

#endif /* LABWC_BACKGROUND_APPS_H */
