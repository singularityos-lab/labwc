/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_ACCESSX_H
#define LABWC_ACCESSX_H

#include <stdbool.h>

struct keyboard;
struct wlr_keyboard_key_event;

void accessx_init(void);
void accessx_finish(void);
void accessx_reconfigure(void);

/* Returns true when the key event is consumed and must not reach clients */
bool accessx_filter_key(struct keyboard *keyboard,
	struct wlr_keyboard_key_event *event);
void accessx_keyboard_destroy(struct keyboard *keyboard);
void accessx_cancel_key_taps(void);

#endif /* LABWC_ACCESSX_H */
