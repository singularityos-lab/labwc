// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "input/cursor-gesture.h"
#include <stdio.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/xcursor.h>
#include "labwc.h"
#include "motion.h"

#define GESTURE_NAME_MAX 64

static struct wlr_xcursor *
gesture_find(struct seat *seat, const char *shape, const char *gesture,
		char name[static GESTURE_NAME_MAX])
{
	if (!motion_enabled()) {
		return NULL;
	}
	int len = snprintf(name, GESTURE_NAME_MAX, "%s-%s", shape, gesture);
	if (len >= GESTURE_NAME_MAX) {
		return NULL;
	}
	return wlr_xcursor_manager_get_xcursor(seat->xcursor_manager, name, 1);
}

static int
handle_gesture_end(void *data)
{
	struct seat *seat = data;
	struct cursor_gesture *state = &seat->cursor_gesture;

	if (state->shape && !state->pressing && seat->cursor_visible) {
		wlr_cursor_set_xcursor(seat->cursor, seat->xcursor_manager,
			state->shape);
	}
	return 0;
}

static void
gesture_cancel(struct cursor_gesture *state)
{
	if (state->timer) {
		wl_event_source_timer_update(state->timer, 0);
	}
	state->pressing = false;
}

static bool
gesture_play(struct seat *seat, const char *shape)
{
	struct cursor_gesture *state = &seat->cursor_gesture;
	char name[GESTURE_NAME_MAX];
	struct wlr_xcursor *xcursor = gesture_find(seat, shape, "enter", name);
	if (!xcursor) {
		return false;
	}
	if (!state->timer) {
		state->timer = wl_event_loop_add_timer(server.wl_event_loop,
			handle_gesture_end, seat);
		if (!state->timer) {
			return false;
		}
	}

	uint32_t duration = 1;
	for (unsigned int i = 0; i < xcursor->image_count; i++) {
		duration += xcursor->images[i]->delay;
	}
	wlr_cursor_set_xcursor(seat->cursor, seat->xcursor_manager, name);
	wl_event_source_timer_update(state->timer, duration);
	return true;
}

void
cursor_gesture_show(struct seat *seat, const char *shape)
{
	struct cursor_gesture *state = &seat->cursor_gesture;
	if (state->shape && !strcmp(state->shape, shape)) {
		return;
	}

	bool closed = state->pressing && !strcmp(state->shape, "grab")
		&& !strcmp(shape, "grabbing");

	gesture_cancel(state);
	state->shape = shape;
	if (closed || !gesture_play(seat, shape)) {
		wlr_cursor_set_xcursor(seat->cursor, seat->xcursor_manager, shape);
	}
}

void
cursor_gesture_press(struct seat *seat, bool pressed)
{
	struct cursor_gesture *state = &seat->cursor_gesture;
	if (!state->shape || !seat->cursor_visible) {
		return;
	}

	if (!pressed) {
		if (state->pressing) {
			gesture_cancel(state);
			wlr_cursor_set_xcursor(seat->cursor, seat->xcursor_manager,
				state->shape);
		}
		return;
	}

	char name[GESTURE_NAME_MAX];
	if (gesture_find(seat, state->shape, "press", name)) {
		gesture_cancel(state);
		state->pressing = true;
		wlr_cursor_set_xcursor(seat->cursor, seat->xcursor_manager, name);
	}
}

void
cursor_gesture_clear(struct seat *seat)
{
	gesture_cancel(&seat->cursor_gesture);
	seat->cursor_gesture.shape = NULL;
}

void
cursor_gesture_finish(struct seat *seat)
{
	cursor_gesture_clear(seat);
	if (seat->cursor_gesture.timer) {
		wl_event_source_remove(seat->cursor_gesture.timer);
		seat->cursor_gesture.timer = NULL;
	}
}
