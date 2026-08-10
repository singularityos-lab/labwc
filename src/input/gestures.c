// SPDX-License-Identifier: GPL-2.0-only
#include "input/gestures.h"
#include <math.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include "action.h"
#include "common/macros.h"
#include "config/gesturebind.h"
#include "labwc.h"
#include "idle.h"
#include "protocols/singularity-gesture.h"
#include "protocols/singularity-tiling.h"
#include "workspaces.h"

#define SWIPE_DIRECTION_LOCK_DISTANCE 10.0

static void
handle_pinch_begin(struct wl_listener *listener, void *data)
{
	struct seat *seat = wl_container_of(listener, seat, pinch_begin);
	struct wlr_pointer_pinch_begin_event *event = data;

	idle_manager_notify_activity(seat->wlr_seat);
	cursor_set_visible(seat, /* visible */ true);

	wlr_pointer_gestures_v1_send_pinch_begin(seat->pointer_gestures,
		seat->wlr_seat, event->time_msec, event->fingers);
}

static void
handle_pinch_update(struct wl_listener *listener, void *data)
{
	struct seat *seat = wl_container_of(listener, seat, pinch_update);
	struct wlr_pointer_pinch_update_event *event = data;

	idle_manager_notify_activity(seat->wlr_seat);
	cursor_set_visible(seat, /* visible */ true);

	wlr_pointer_gestures_v1_send_pinch_update(seat->pointer_gestures,
		seat->wlr_seat, event->time_msec, event->dx, event->dy,
		event->scale, event->rotation);
}

static void
handle_pinch_end(struct wl_listener *listener, void *data)
{
	struct seat *seat = wl_container_of(listener, seat, pinch_end);
	struct wlr_pointer_pinch_end_event *event = data;

	idle_manager_notify_activity(seat->wlr_seat);
	cursor_set_visible(seat, /* visible */ true);

	wlr_pointer_gestures_v1_send_pinch_end(seat->pointer_gestures,
		seat->wlr_seat, event->time_msec, event->cancelled);
}

static void
handle_swipe_begin(struct wl_listener *listener, void *data)
{
	struct seat *seat = wl_container_of(listener, seat, swipe_begin);
	struct wlr_pointer_swipe_begin_event *event = data;

	idle_manager_notify_activity(seat->wlr_seat);
	cursor_set_visible(seat, /* visible */ true);
	seat->swipe.claimed = false;
	bool scrolling_candidate = event->fingers == 3
		&& singularity_tiling_scrolling_mode_enabled();
	bool workspace_candidate = event->fingers == 4;
	seat->swipe.forwarded = !scrolling_candidate && !workspace_candidate
		&& !gesturebind_claims(LAB_GESTURE_SWIPE, event->fingers);
	seat->swipe.shell_gesture = false;
	seat->swipe.workspace_gesture = false;
	seat->swipe.fingers = event->fingers;
	seat->swipe.direction = LAB_DIRECTION_INVALID;
	seat->swipe.dx = 0;
	seat->swipe.dy = 0;

	if (seat->swipe.forwarded) {
		wlr_pointer_gestures_v1_send_swipe_begin(seat->pointer_gestures,
			seat->wlr_seat, event->time_msec, event->fingers);
	}
}

static void
handle_swipe_update(struct wl_listener *listener, void *data)
{
	struct seat *seat = wl_container_of(listener, seat, swipe_update);
	struct wlr_pointer_swipe_update_event *event = data;

	idle_manager_notify_activity(seat->wlr_seat);
	cursor_set_visible(seat, /* visible */ true);
	seat->swipe.dx += event->dx;
	seat->swipe.dy += event->dy;

	if (!seat->swipe.claimed && !seat->swipe.forwarded
			&& MAX(fabs(seat->swipe.dx), fabs(seat->swipe.dy))
				>= SWIPE_DIRECTION_LOCK_DISTANCE) {
		seat->swipe.direction = gesture_direction_from_delta(
			seat->swipe.dx, seat->swipe.dy);
		bool scrolling_swipe = seat->swipe.fingers == 3
			&& (seat->swipe.direction == LAB_DIRECTION_LEFT
				|| seat->swipe.direction == LAB_DIRECTION_RIGHT)
			&& singularity_tiling_scrolling_mode_enabled();
		bool workspace_swipe = seat->swipe.fingers == 4
			&& (seat->swipe.direction == LAB_DIRECTION_LEFT
				|| seat->swipe.direction == LAB_DIRECTION_RIGHT);
		seat->swipe.claimed = scrolling_swipe || workspace_swipe
			|| gesturebind_claims_direction(LAB_GESTURE_SWIPE,
				seat->swipe.fingers, seat->swipe.direction);
		if (seat->swipe.claimed && (seat->swipe.fingers == 3
				|| seat->swipe.fingers == 4)) {
			if (seat->swipe.fingers == 4
					&& (seat->swipe.direction == LAB_DIRECTION_LEFT
					|| seat->swipe.direction == LAB_DIRECTION_RIGHT)) {
				seat->swipe.workspace_gesture = workspaces_swipe_begin(
					seat->swipe.direction);
			} else if (singularity_gesture_has_clients()) {
				seat->swipe.shell_gesture = true;
				singularity_gesture_send_begin(seat->swipe.fingers,
					seat->swipe.direction);
			}
		}
		if (!seat->swipe.claimed) {
			seat->swipe.forwarded = true;
			wlr_pointer_gestures_v1_send_swipe_begin(
				seat->pointer_gestures, seat->wlr_seat,
				event->time_msec, seat->swipe.fingers);
			wlr_pointer_gestures_v1_send_swipe_update(
				seat->pointer_gestures, seat->wlr_seat,
				event->time_msec, seat->swipe.dx, seat->swipe.dy);
			return;
		}
	}

	if (seat->swipe.workspace_gesture) {
		workspaces_swipe_update(seat->swipe.dx);
	} else if (seat->swipe.shell_gesture) {
		singularity_gesture_send_update(seat->swipe.dx, seat->swipe.dy);
	}

	if (seat->swipe.forwarded) {
		wlr_pointer_gestures_v1_send_swipe_update(seat->pointer_gestures,
			seat->wlr_seat, event->time_msec, event->dx, event->dy);
	}
}

static void
forward_accumulated_swipe(struct seat *seat, uint32_t time_msec,
	bool cancelled)
{
	wlr_pointer_gestures_v1_send_swipe_begin(seat->pointer_gestures,
		seat->wlr_seat, time_msec, seat->swipe.fingers);
	if (seat->swipe.dx != 0 || seat->swipe.dy != 0) {
		wlr_pointer_gestures_v1_send_swipe_update(seat->pointer_gestures,
			seat->wlr_seat, time_msec, seat->swipe.dx, seat->swipe.dy);
	}
	wlr_pointer_gestures_v1_send_swipe_end(seat->pointer_gestures,
		seat->wlr_seat, time_msec, cancelled);
}

static void
handle_swipe_end(struct wl_listener *listener, void *data)
{
	struct seat *seat = wl_container_of(listener, seat, swipe_end);
	struct wlr_pointer_swipe_end_event *event = data;

	idle_manager_notify_activity(seat->wlr_seat);
	cursor_set_visible(seat, /* visible */ true);

	bool handled = false;
	if (seat->swipe.claimed && !event->cancelled) {
		struct gesturebind *bind = gesturebind_match_direction(
				LAB_GESTURE_SWIPE, seat->swipe.fingers,
				seat->swipe.direction, seat->swipe.dx, seat->swipe.dy);
		bool commit = bind != NULL;
		if (seat->swipe.workspace_gesture) {
			workspaces_swipe_end(commit);
			handled = true;
		} else if (seat->swipe.shell_gesture) {
			singularity_gesture_send_end(false, commit);
			handled = true;
		} else if (bind) {
			actions_run(NULL, &bind->actions, NULL);
			handled = true;
		}
	}
	if (event->cancelled && seat->swipe.workspace_gesture) {
		workspaces_swipe_end(false);
		handled = true;
	} else if (event->cancelled && seat->swipe.shell_gesture) {
		singularity_gesture_send_end(true, false);
		handled = true;
	}
	if (!handled) {
		if (!seat->swipe.forwarded) {
			forward_accumulated_swipe(seat, event->time_msec,
				event->cancelled);
		} else {
			wlr_pointer_gestures_v1_send_swipe_end(
				seat->pointer_gestures, seat->wlr_seat,
				event->time_msec, event->cancelled);
		}
	}
	seat->swipe.claimed = false;
	seat->swipe.forwarded = false;
	seat->swipe.shell_gesture = false;
	seat->swipe.workspace_gesture = false;
	seat->swipe.fingers = 0;
	seat->swipe.direction = LAB_DIRECTION_INVALID;
	seat->swipe.dx = 0;
	seat->swipe.dy = 0;
}

static void
handle_hold_begin(struct wl_listener *listener, void *data)
{
	struct seat *seat = wl_container_of(listener, seat, hold_begin);
	struct wlr_pointer_hold_begin_event *event = data;

	idle_manager_notify_activity(seat->wlr_seat);
	cursor_set_visible(seat, /* visible */ true);

	wlr_pointer_gestures_v1_send_hold_begin(seat->pointer_gestures,
		seat->wlr_seat, event->time_msec, event->fingers);
}

static void
handle_hold_end(struct wl_listener *listener, void *data)
{
	struct seat *seat = wl_container_of(listener, seat, hold_end);
	struct wlr_pointer_hold_end_event *event = data;

	idle_manager_notify_activity(seat->wlr_seat);
	cursor_set_visible(seat, /* visible */ true);

	wlr_pointer_gestures_v1_send_hold_end(seat->pointer_gestures,
		seat->wlr_seat, event->time_msec, event->cancelled);
}

void
gestures_init(struct seat *seat)
{
	seat->pointer_gestures = wlr_pointer_gestures_v1_create(server.wl_display);

	CONNECT_SIGNAL(seat->cursor, seat, pinch_begin);
	CONNECT_SIGNAL(seat->cursor, seat, pinch_update);
	CONNECT_SIGNAL(seat->cursor, seat, pinch_end);
	CONNECT_SIGNAL(seat->cursor, seat, swipe_begin);
	CONNECT_SIGNAL(seat->cursor, seat, swipe_update);
	CONNECT_SIGNAL(seat->cursor, seat, swipe_end);
	CONNECT_SIGNAL(seat->cursor, seat, hold_begin);
	CONNECT_SIGNAL(seat->cursor, seat, hold_end);
}

void
gestures_finish(struct seat *seat)
{
	wl_list_remove(&seat->pinch_begin.link);
	wl_list_remove(&seat->pinch_update.link);
	wl_list_remove(&seat->pinch_end.link);
	wl_list_remove(&seat->swipe_begin.link);
	wl_list_remove(&seat->swipe_update.link);
	wl_list_remove(&seat->swipe_end.link);
	wl_list_remove(&seat->hold_begin.link);
	wl_list_remove(&seat->hold_end.link);
}
