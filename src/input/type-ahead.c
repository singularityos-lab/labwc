// SPDX-License-Identifier: GPL-2.0-only
#include "input/type-ahead.h"
#include <wayland-server-core.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_seat.h>
#include "input/keyboard.h"
#include "labwc.h"

#define TYPE_AHEAD_MAX_KEYS 256

struct held_key {
	uint32_t time_msec;
	uint32_t keycode;
	uint32_t state;
	struct wlr_keyboard *keyboard;
	struct wlr_keyboard_modifiers modifiers;
};

static struct {
	bool active;
	struct wlr_surface *origin;
	struct wl_array keys;
	struct wl_event_source *timer;
} ahead;

static int
handle_timeout(void *data)
{
	type_ahead_deliver(data);
	return 0;
}

void
type_ahead_begin(struct seat *seat, uint32_t timeout_ms)
{
	if (!ahead.timer) {
		wl_array_init(&ahead.keys);
		ahead.timer = wl_event_loop_add_timer(server.wl_event_loop,
			handle_timeout, seat);
	}
	if (!ahead.active) {
		ahead.keys.size = 0;
		ahead.origin = seat->wlr_seat->keyboard_state.focused_surface;
	}
	ahead.active = true;
	wl_event_source_timer_update(ahead.timer, timeout_ms ? timeout_ms : 1);
}

bool
type_ahead_take(struct seat *seat, struct keyboard *keyboard,
		struct wlr_keyboard_key_event *event)
{
	if (!ahead.active) {
		return false;
	}
	if (ahead.keys.size / sizeof(struct held_key) >= TYPE_AHEAD_MAX_KEYS) {
		type_ahead_deliver(seat);
		return false;
	}
	struct held_key *key = wl_array_add(&ahead.keys, sizeof(*key));
	if (!key) {
		type_ahead_deliver(seat);
		return false;
	}
	key->time_msec = event->time_msec;
	key->keycode = event->keycode;
	key->state = event->state;
	key->keyboard = keyboard->wlr_keyboard;
	key->modifiers = keyboard->wlr_keyboard->modifiers;
	return true;
}

void
type_ahead_deliver(struct seat *seat)
{
	if (!ahead.active) {
		return;
	}
	ahead.active = false;
	ahead.origin = NULL;
	wl_event_source_timer_update(ahead.timer, 0);

	struct wlr_seat *wlr_seat = seat->wlr_seat;
	struct wlr_keyboard *group = &seat->keyboard_group->keyboard;
	if (ahead.keys.size == 0) {
		return;
	}
	struct held_key *key;
	wl_array_for_each(key, &ahead.keys) {
		if (wlr_seat_get_keyboard(wlr_seat) != key->keyboard) {
			wlr_seat_set_keyboard(wlr_seat, key->keyboard);
		}
		wlr_seat_keyboard_notify_modifiers(wlr_seat, &key->modifiers);
		wlr_seat_keyboard_notify_key(wlr_seat, key->time_msec,
			key->keycode, key->state);
	}
	ahead.keys.size = 0;
	wlr_seat_set_keyboard(wlr_seat, group);
	wlr_seat_keyboard_notify_modifiers(wlr_seat, &group->modifiers);
}

void
type_ahead_focus_changed(struct seat *seat, struct wlr_surface *surface)
{
	if (ahead.active && surface && surface != ahead.origin) {
		type_ahead_deliver(seat);
	}
}

void
type_ahead_keyboard_gone(struct seat *seat, struct wlr_keyboard *keyboard)
{
	if (!ahead.active) {
		return;
	}
	struct held_key *key;
	wl_array_for_each(key, &ahead.keys) {
		if (key->keyboard == keyboard) {
			type_ahead_deliver(seat);
			return;
		}
	}
}

void
type_ahead_finish(struct seat *seat)
{
	if (ahead.timer) {
		wl_event_source_remove(ahead.timer);
		ahead.timer = NULL;
		wl_array_release(&ahead.keys);
	}
	ahead.active = false;
	ahead.origin = NULL;
}
