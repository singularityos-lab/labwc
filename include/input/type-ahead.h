/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_TYPE_AHEAD_H
#define LABWC_TYPE_AHEAD_H

#include <stdbool.h>
#include <stdint.h>

struct seat;
struct keyboard;
struct wlr_keyboard_key_event;
struct wlr_surface;
struct wlr_keyboard;

void type_ahead_begin(struct seat *seat, uint32_t timeout_ms);
bool type_ahead_take(struct seat *seat, struct keyboard *keyboard,
	struct wlr_keyboard_key_event *event);
void type_ahead_deliver(struct seat *seat);
void type_ahead_focus_changed(struct seat *seat, struct wlr_surface *surface);
void type_ahead_keyboard_gone(struct seat *seat, struct wlr_keyboard *keyboard);
void type_ahead_finish(struct seat *seat);

#endif /* LABWC_TYPE_AHEAD_H */
