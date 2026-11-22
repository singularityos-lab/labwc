/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_CURSOR_GESTURE_H
#define LABWC_CURSOR_GESTURE_H

#include <stdbool.h>

struct seat;

void cursor_gesture_show(struct seat *seat, const char *shape);
void cursor_gesture_press(struct seat *seat, bool pressed);
void cursor_gesture_clear(struct seat *seat);
void cursor_gesture_finish(struct seat *seat);

#endif /* LABWC_CURSOR_GESTURE_H */
