// SPDX-License-Identifier: GPL-2.0-only
#include "input/gestures.h"
#include <math.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include "action.h"
#include "common/macros.h"
#include "config/gesturebind.h"
#include "config/rcxml.h"
#include "labwc.h"
#include "idle.h"
#include "protocols/singularity-gesture.h"
#include "protocols/singularity-tiling.h"
#include "workspaces.h"


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

#define PAGE_WIDTH 400.0
#define PAGE_HEIGHT 240.0
#define VELOCITY_WINDOW_MS 150
#define FLING_VELOCITY 0.6
#define AXIS_SWITCH_DISTANCE 60.0

static bool
desktop_fingers(uint32_t fingers)
{
	return rc.gestures_enabled && fingers < 32
		&& (rc.gesture_fingers & (1u << fingers)) != 0;
}

static void
swipe_reset(struct seat *seat)
{
	seat->swipe.claimed = false;
	seat->swipe.forwarded = false;
	seat->swipe.shell_gesture = false;
	seat->swipe.workspace_gesture = false;
	seat->swipe.desktop = false;
	seat->swipe.fingers = 0;
	seat->swipe.direction = LAB_DIRECTION_INVALID;
	seat->swipe.dx = 0;
	seat->swipe.dy = 0;
	seat->swipe.sdx = 0;
	seat->swipe.sdy = 0;
	seat->swipe.origin_x = 0;
	seat->swipe.origin_y = 0;
	seat->swipe.history_len = 0;
	seat->swipe.history_next = 0;
}

static void
swipe_record(struct seat *seat, uint32_t time_msec, double dx, double dy)
{
	int i = seat->swipe.history_next;
	seat->swipe.history_time[i] = time_msec;
	seat->swipe.history_dx[i] = dx;
	seat->swipe.history_dy[i] = dy;
	seat->swipe.history_next = (i + 1) % 32;
	if (seat->swipe.history_len < 32) {
		seat->swipe.history_len++;
	}
}

static void
swipe_velocity(struct seat *seat, uint32_t now, double *vx, double *vy)
{
	*vx = 0;
	*vy = 0;
	int len = seat->swipe.history_len;
	if (len < 2) {
		return;
	}
	double sum_x = 0, sum_y = 0;
	uint32_t first = now, last = 0;
	int used = 0;
	for (int k = 0; k < len; k++) {
		int i = (seat->swipe.history_next - 1 - k + 32) % 32;
		uint32_t t = seat->swipe.history_time[i];
		if (now - t > VELOCITY_WINDOW_MS) {
			break;
		}
		if (used > 0) {
			sum_x += seat->swipe.history_dx[(i + 1) % 32];
			sum_y += seat->swipe.history_dy[(i + 1) % 32];
		}
		first = t;
		if (used == 0) {
			last = t;
		}
		used++;
	}
	if (used < 2 || last <= first) {
		return;
	}
	*vx = sum_x / (double)(last - first);
	*vy = sum_y / (double)(last - first);
}

static bool
decide_commit(double progress, double velocity_along)
{
	if (velocity_along >= FLING_VELOCITY) {
		return true;
	}
	if (velocity_along <= -FLING_VELOCITY) {
		return false;
	}
	return progress >= 0.5;
}

static void
update_workspace_swipe(struct seat *seat)
{
	int width = workspaces_swipe_width();
	if (width < 1) {
		return;
	}
	double moved = seat->swipe.sdx - seat->swipe.origin_x;
	workspaces_swipe_update(moved / PAGE_WIDTH * width);
	singularity_gesture_send_update(moved, 0);
}

static void
start_shell_gesture(struct seat *seat, enum direction direction)
{
	seat->swipe.shell_gesture = true;
	seat->swipe.direction = direction;
	singularity_gesture_send_begin(seat->swipe.fingers, direction);
}

static void
handle_swipe_begin(struct wl_listener *listener, void *data)
{
	struct seat *seat = wl_container_of(listener, seat, swipe_begin);
	struct wlr_pointer_swipe_begin_event *event = data;

	idle_manager_notify_activity(seat->wlr_seat);
	cursor_set_visible(seat, /* visible */ true);
	swipe_reset(seat);
	bool scrolling_candidate = event->fingers == 3
		&& singularity_tiling_scrolling_mode_enabled();
	seat->swipe.desktop = desktop_fingers(event->fingers);
	seat->swipe.forwarded = !scrolling_candidate && !seat->swipe.desktop
		&& !gesturebind_claims(LAB_GESTURE_SWIPE, event->fingers);
	seat->swipe.fingers = event->fingers;

	if (seat->swipe.forwarded) {
		wlr_pointer_gestures_v1_send_swipe_begin(seat->pointer_gestures,
			seat->wlr_seat, event->time_msec, event->fingers);
	}
}

static void
decide_swipe(struct seat *seat, uint32_t time_msec)
{
	bool horizontal;
	if (seat->swipe.desktop) {
		seat->swipe.direction = gesture_direction_from_delta(
			seat->swipe.sdx, seat->swipe.sdy);
	} else {
		seat->swipe.direction = gesture_direction_from_delta(
			seat->swipe.dx, seat->swipe.dy);
	}
	horizontal = seat->swipe.direction == LAB_DIRECTION_LEFT
		|| seat->swipe.direction == LAB_DIRECTION_RIGHT;
	bool scrolling_swipe = seat->swipe.fingers == 3 && horizontal
		&& singularity_tiling_scrolling_mode_enabled();

	if (scrolling_swipe) {
		seat->swipe.claimed = true;
		if (singularity_gesture_has_clients()) {
			start_shell_gesture(seat, seat->swipe.direction);
		}
		return;
	}
	if (seat->swipe.desktop) {
		seat->swipe.claimed = true;
		if (horizontal) {
			seat->swipe.workspace_gesture =
				workspaces_swipe_begin(seat->swipe.direction);
			if (seat->swipe.workspace_gesture) {
				singularity_gesture_send_begin(seat->swipe.fingers,
					seat->swipe.direction);
				return;
			}
		} else if (singularity_gesture_has_clients()) {
			start_shell_gesture(seat, seat->swipe.direction);
			return;
		}
		seat->swipe.claimed = gesturebind_claims_direction(LAB_GESTURE_SWIPE,
			seat->swipe.fingers, seat->swipe.direction);
		if (seat->swipe.claimed) {
			return;
		}
	} else {
		seat->swipe.claimed = gesturebind_claims_direction(LAB_GESTURE_SWIPE,
			seat->swipe.fingers, seat->swipe.direction);
		if (seat->swipe.claimed) {
			if ((seat->swipe.fingers == 3 || seat->swipe.fingers == 4)
					&& singularity_gesture_has_clients()) {
				start_shell_gesture(seat, seat->swipe.direction);
			}
			return;
		}
	}
	seat->swipe.forwarded = true;
	wlr_pointer_gestures_v1_send_swipe_begin(seat->pointer_gestures,
		seat->wlr_seat, time_msec, seat->swipe.fingers);
	wlr_pointer_gestures_v1_send_swipe_update(seat->pointer_gestures,
		seat->wlr_seat, time_msec, seat->swipe.dx, seat->swipe.dy);
}

static void
maybe_switch_axis(struct seat *seat)
{
	if (!rc.gesture_two_dimensional || !seat->swipe.workspace_gesture) {
		return;
	}
	double vertical = seat->swipe.sdy - seat->swipe.origin_y;
	double horizontal = seat->swipe.sdx - seat->swipe.origin_x;
	if (fabs(vertical) < AXIS_SWITCH_DISTANCE
			|| fabs(vertical) < fabs(horizontal) * 0.6
			|| !singularity_gesture_has_clients()) {
		return;
	}
	bool commit = workspaces_swipe_fraction() >= 0.5;
	singularity_gesture_send_end(false, commit);
	workspaces_swipe_end_now(commit);
	seat->swipe.workspace_gesture = false;
	seat->swipe.origin_x = seat->swipe.sdx;
	start_shell_gesture(seat, vertical > 0 ? LAB_DIRECTION_DOWN : LAB_DIRECTION_UP);
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
	double factor = rc.gesture_sensitivity * (rc.gesture_invert ? -1.0 : 1.0);
	double sdx = seat->swipe.desktop ? event->dx * factor : event->dx;
	double sdy = seat->swipe.desktop ? event->dy * factor : event->dy;
	seat->swipe.sdx += sdx;
	seat->swipe.sdy += sdy;
	swipe_record(seat, event->time_msec, sdx, sdy);

	if (!seat->swipe.claimed && !seat->swipe.forwarded
			&& hypot(seat->swipe.dx, seat->swipe.dy)
				>= rc.gesture_threshold) {
		decide_swipe(seat, event->time_msec);
		if (seat->swipe.forwarded) {
			return;
		}
	}

	if (seat->swipe.workspace_gesture) {
		maybe_switch_axis(seat);
	}
	if (seat->swipe.workspace_gesture) {
		update_workspace_swipe(seat);
	} else if (seat->swipe.shell_gesture) {
		singularity_gesture_send_update(
			seat->swipe.sdx - seat->swipe.origin_x,
			seat->swipe.sdy - seat->swipe.origin_y);
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

static double
along(enum direction direction, double x, double y)
{
	switch (direction) {
	case LAB_DIRECTION_LEFT: return -x;
	case LAB_DIRECTION_RIGHT: return x;
	case LAB_DIRECTION_UP: return -y;
	case LAB_DIRECTION_DOWN: return y;
	default: return 0;
	}
}

static void
handle_swipe_end(struct wl_listener *listener, void *data)
{
	struct seat *seat = wl_container_of(listener, seat, swipe_end);
	struct wlr_pointer_swipe_end_event *event = data;

	idle_manager_notify_activity(seat->wlr_seat);
	cursor_set_visible(seat, /* visible */ true);

	double vx, vy;
	swipe_velocity(seat, event->time_msec, &vx, &vy);
	bool handled = false;
	if (seat->swipe.workspace_gesture) {
		bool commit = false;
		if (!event->cancelled) {
			double v = along(seat->swipe.direction, vx, vy);
			if (rc.gesture_two_dimensional && fabs(vy) > fabs(vx)) {
				v = 0;
			}
			commit = decide_commit(workspaces_swipe_fraction(), v);
		} else {
			commit = workspaces_swipe_fraction() >= 0.5;
		}
		singularity_gesture_send_end(event->cancelled, commit);
		int width = workspaces_swipe_width();
		double velocity = event->cancelled ? 0.0
			: vx * width / PAGE_WIDTH * 1000.0;
		workspaces_swipe_release(commit, velocity);
		handled = true;
	} else if (seat->swipe.shell_gesture && seat->swipe.desktop) {
		double moved = along(seat->swipe.direction,
			seat->swipe.sdx - seat->swipe.origin_x,
			seat->swipe.sdy - seat->swipe.origin_y);
		double v = along(seat->swipe.direction, vx, vy);
		if (rc.gesture_two_dimensional && fabs(vx) > fabs(vy)) {
			v = 0;
		}
		bool commit = !event->cancelled && decide_commit(moved / PAGE_HEIGHT, v);
		singularity_gesture_send_end(event->cancelled, commit);
		handled = true;
	} else if (seat->swipe.claimed && !event->cancelled) {
		struct gesturebind *bind = gesturebind_match_direction(
				LAB_GESTURE_SWIPE, seat->swipe.fingers,
				seat->swipe.direction, seat->swipe.dx, seat->swipe.dy);
		bool commit = bind != NULL;
		if (seat->swipe.shell_gesture) {
			singularity_gesture_send_end(false, commit);
			handled = true;
		} else if (bind) {
			actions_run(NULL, &bind->actions, NULL);
			handled = true;
		}
	} else if (event->cancelled && seat->swipe.shell_gesture) {
		singularity_gesture_send_end(true, false);
		handled = true;
	}
	if (!handled && !seat->swipe.claimed) {
		if (!seat->swipe.forwarded) {
			forward_accumulated_swipe(seat, event->time_msec,
				event->cancelled);
		} else {
			wlr_pointer_gestures_v1_send_swipe_end(
				seat->pointer_gestures, seat->wlr_seat,
				event->time_msec, event->cancelled);
		}
	}
	swipe_reset(seat);
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
