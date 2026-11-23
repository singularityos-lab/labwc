// SPDX-License-Identifier: GPL-2.0-only
#include "protocols/singularity-input-share.h"
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/backend/wayland.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include "input/cursor.h"
#include "input/keyboard.h"
#include "common/mem.h"
#include "labwc.h"
#include "output.h"
#include "session-lock.h"
#include "view.h"
#include "singularity-input-share-unstable-v1-protocol.h"

struct share_edge {
	struct wl_resource *resource;
	char *output;
	uint32_t side;
	int32_t start, end;
	uint32_t resistance;
	double pressed;
	uint32_t pressed_msec;
	double reported;
	struct wl_list link;
};

#define PUSH_DISTANCE 120.0
#define PUSH_PAUSE_MSEC 400

static struct {
	struct wl_global *global;
	struct wl_list edges;
	struct wl_list probes;
	struct {
		struct wlr_output *output;
		uint32_t side;
		double pushed;
		uint32_t last_msec;
		bool spent;
	} push;
	struct share_edge *active;
	struct {
		struct share_edge *edge;
		struct view *view;
		double pushed;
		uint32_t msec;
		bool through;
	} drag;
	struct seat *seat;
	struct wlr_output *locked_output;
	struct wlr_surface *keyboard_surface;
	struct wl_listener surface_destroy;
} share;

static bool
edge_box(struct share_edge *edge, struct wlr_box *box)
{
	struct output *output = output_from_name(edge->output);
	if (!output || !output->wlr_output || !output->wlr_output->enabled) {
		return false;
	}
	wlr_output_layout_get_box(server.output_layout, output->wlr_output, box);
	return !wlr_box_empty(box);
}

static bool
vertical(uint32_t side)
{
	return side == ZSINGULARITY_INPUT_SHARE_MANAGER_V1_SIDE_LEFT
		|| side == ZSINGULARITY_INPUT_SHARE_MANAGER_V1_SIDE_RIGHT;
}

static void
forget_surface(void)
{
	if (share.keyboard_surface) {
		wl_list_remove(&share.surface_destroy.link);
		share.keyboard_surface = NULL;
	}
}

static void
handle_surface_destroy(struct wl_listener *listener, void *data)
{
	forget_surface();
}

static void
lock_host_pointer(struct wlr_output *output, bool locked)
{
	if (locked && output && wlr_output_is_wl(output)) {
		wlr_wl_output_lock_pointer(output, true);
		share.locked_output = output;
	} else if (!locked && share.locked_output) {
		wlr_wl_output_lock_pointer(share.locked_output, false);
		share.locked_output = NULL;
	}
}

static void
unlock_host_pointer_at(double lx, double ly)
{
	struct wlr_output *output = share.locked_output;
	if (!output) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server.output_layout, output, &box);
	share.locked_output = NULL;
	wlr_wl_output_unlock_pointer_at(output,
		(lx - box.x) * output->scale, (ly - box.y) * output->scale);
}

static void
finish(uint32_t reason, bool warp, double position)
{
	struct share_edge *edge = share.active;
	if (!edge) {
		return;
	}
	share.active = NULL;
	struct seat *seat = share.seat;
	struct wlr_box box;
	if (warp && edge_box(edge, &box)) {
		double x = 0, y = 0;
		if (vertical(edge->side)) {
			y = box.y + position;
			x = edge->side == ZSINGULARITY_INPUT_SHARE_MANAGER_V1_SIDE_LEFT
				? box.x + 1 : box.x + box.width - 2;
		} else {
			x = box.x + position;
			y = edge->side == ZSINGULARITY_INPUT_SHARE_MANAGER_V1_SIDE_TOP
				? box.y + 1 : box.y + box.height - 2;
		}
		unlock_host_pointer_at(x, y);
		wlr_cursor_warp(seat->cursor, NULL, x, y);
	}
	lock_host_pointer(NULL, false);
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat->wlr_seat);
	if (share.keyboard_surface && keyboard) {
		wlr_seat_keyboard_notify_enter(seat->wlr_seat, share.keyboard_surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
	forget_surface();
	cursor_update_focus();
	cursor_update_image(seat);
	zsingularity_input_share_edge_v1_send_left(edge->resource, reason);
	wlr_log(WLR_DEBUG, "input share ended (%u)", reason);
}

static void
start(struct seat *seat, struct share_edge *edge, struct wlr_output *output,
		double position, double length)
{
	share.active = edge;
	share.seat = seat;
	forget_surface();
	struct wlr_surface *focused = seat->wlr_seat->keyboard_state.focused_surface;
	if (focused) {
		share.keyboard_surface = focused;
		share.surface_destroy.notify = handle_surface_destroy;
		wl_signal_add(&focused->events.destroy, &share.surface_destroy);
	}
	wlr_seat_keyboard_notify_clear_focus(seat->wlr_seat);
	wlr_seat_pointer_notify_clear_focus(seat->wlr_seat);
	wlr_cursor_unset_image(seat->cursor);
	lock_host_pointer(output, true);
	double fraction = length > 0 ? position / length : 0;
	zsingularity_input_share_edge_v1_send_entered(edge->resource,
		wl_fixed_from_double(position), wl_fixed_from_double(fraction));
	wlr_log(WLR_DEBUG, "input share started on %s", edge->output);
}

bool
singularity_input_share_active(void)
{
	return share.active != NULL;
}

static void
report_pressing(struct share_edge *edge, double position, double progress)
{
	if (fabs(progress - edge->reported) < 0.01 && progress > 0) {
		return;
	}
	edge->reported = progress;
	double length = edge->end - edge->start;
	zsingularity_input_share_edge_v1_send_pressing(edge->resource,
		wl_fixed_from_double(position),
		wl_fixed_from_double(length > 0 ? position / length : 0),
		wl_fixed_from_double(progress));
}

static void
ease_off(struct share_edge *except)
{
	struct share_edge *edge;
	wl_list_for_each(edge, &share.edges, link) {
		if (edge != except && edge->pressed > 0) {
			edge->pressed = 0;
			report_pressing(edge, 0, 0);
		}
	}
}

static bool
resist(struct share_edge *edge, double push, double position, uint32_t msec)
{
	if (!edge->resistance) {
		return false;
	}
	if (msec - edge->pressed_msec > PUSH_PAUSE_MSEC) {
		edge->pressed = 0;
	}
	edge->pressed_msec = msec;
	edge->pressed += push;
	double progress = edge->pressed / edge->resistance;
	if (progress < 1) {
		report_pressing(edge, position, progress);
		return true;
	}
	edge->resistance = 0;
	edge->pressed = 0;
	edge->reported = 0;
	return false;
}

static bool
on_edge(struct share_edge *edge, double cx, double cy, double dx, double dy,
		bool pushing, double *position)
{
	struct wlr_box box;
	if (!edge_box(edge, &box)) {
		return false;
	}
	bool at = false;
	double nx = cx, ny = cy;
	switch (edge->side) {
	case ZSINGULARITY_INPUT_SHARE_MANAGER_V1_SIDE_LEFT:
		at = (!pushing || dx < 0) && cx <= box.x + 0.5;
		nx = box.x - 1;
		break;
	case ZSINGULARITY_INPUT_SHARE_MANAGER_V1_SIDE_RIGHT:
		at = (!pushing || dx > 0) && cx >= box.x + box.width - 1;
		nx = box.x + box.width + 1;
		break;
	case ZSINGULARITY_INPUT_SHARE_MANAGER_V1_SIDE_TOP:
		at = (!pushing || dy < 0) && cy <= box.y + 0.5;
		ny = box.y - 1;
		break;
	case ZSINGULARITY_INPUT_SHARE_MANAGER_V1_SIDE_BOTTOM:
		at = (!pushing || dy > 0) && cy >= box.y + box.height - 1;
		ny = box.y + box.height + 1;
		break;
	}
	if (!at) {
		return false;
	}
	double along;
	if (vertical(edge->side)) {
		if (cy < box.y || cy >= box.y + box.height) {
			return false;
		}
		along = cy - box.y;
	} else {
		if (cx < box.x || cx >= box.x + box.width) {
			return false;
		}
		along = cx - box.x;
	}
	if (along < edge->start || along >= edge->end) {
		return false;
	}
	if (wlr_output_layout_output_at(server.output_layout, nx, ny)) {
		return false;
	}
	*position = along;
	return true;
}

static bool
cross_edge(struct seat *seat, double dx, double dy, uint32_t msec)
{
	double cx = seat->cursor->x, cy = seat->cursor->y;
	struct share_edge *edge;
	wl_list_for_each(edge, &share.edges, link) {
		double position;
		if (!on_edge(edge, cx, cy, dx, dy, true, &position)) {
			continue;
		}
		ease_off(edge);
		if (resist(edge, vertical(edge->side) ? fabs(dx) : fabs(dy),
				position - edge->start, msec)) {
			return true;
		}
		struct output *output = output_from_name(edge->output);
		start(seat, edge, output ? output->wlr_output : NULL,
			position - edge->start, edge->end - edge->start);
		return true;
	}
	if (dx != 0 || dy != 0) {
		ease_off(NULL);
	}
	return false;
}

static bool
free_side(struct wlr_box *box, uint32_t side, double cx, double cy,
		double dx, double dy, double *position, double *length)
{
	double nx = cx, ny = cy;
	bool at = false;
	switch (side) {
	case ZSINGULARITY_INPUT_SHARE_MANAGER_V1_SIDE_LEFT:
		at = dx < 0 && cx <= box->x + 0.5;
		nx = box->x - 1;
		break;
	case ZSINGULARITY_INPUT_SHARE_MANAGER_V1_SIDE_RIGHT:
		at = dx > 0 && cx >= box->x + box->width - 1;
		nx = box->x + box->width + 1;
		break;
	case ZSINGULARITY_INPUT_SHARE_MANAGER_V1_SIDE_TOP:
		at = dy < 0 && cy <= box->y + 0.5;
		ny = box->y - 1;
		break;
	case ZSINGULARITY_INPUT_SHARE_MANAGER_V1_SIDE_BOTTOM:
		at = dy > 0 && cy >= box->y + box->height - 1;
		ny = box->y + box->height + 1;
		break;
	}
	if (!at || wlr_output_layout_output_at(server.output_layout, nx, ny)) {
		return false;
	}
	*position = vertical(side) ? cy - box->y : cx - box->x;
	*length = vertical(side) ? box->height : box->width;
	return true;
}

static bool
taken(struct wlr_output *output, uint32_t side, double position)
{
	struct share_edge *edge;
	wl_list_for_each(edge, &share.edges, link) {
		if (edge->side == side && !strcmp(edge->output, output->name)
				&& position >= edge->start && position < edge->end) {
			return true;
		}
	}
	return false;
}

static void
probe(struct seat *seat, double dx, double dy, uint32_t msec)
{
	double cx = seat->cursor->x, cy = seat->cursor->y;
	struct wlr_output *output =
		wlr_output_layout_output_at(server.output_layout, cx, cy);
	struct wlr_box box = {0};
	if (output) {
		wlr_output_layout_get_box(server.output_layout, output, &box);
	}
	for (uint32_t side = 0; output
			&& side <= ZSINGULARITY_INPUT_SHARE_MANAGER_V1_SIDE_BOTTOM; side++) {
		double position, length;
		if (!free_side(&box, side, cx, cy, dx, dy, &position, &length)
				|| taken(output, side, position)) {
			continue;
		}
		bool same = share.push.output == output && share.push.side == side
			&& msec - share.push.last_msec <= PUSH_PAUSE_MSEC;
		if (!same) {
			share.push.output = output;
			share.push.side = side;
			share.push.pushed = 0;
			share.push.spent = false;
		}
		share.push.last_msec = msec;
		if (share.push.spent) {
			return;
		}
		share.push.pushed += vertical(side) ? fabs(dx) : fabs(dy);
		if (share.push.pushed < PUSH_DISTANCE) {
			return;
		}
		share.push.spent = true;
		struct wl_resource *resource;
		wl_resource_for_each(resource, &share.probes) {
			zsingularity_input_share_probe_v1_send_pushed(resource,
				output->name, side, wl_fixed_from_double(position),
				wl_fixed_from_double(length));
		}
		return;
	}
	if (share.push.output && (dx != 0 || dy != 0)) {
		share.push.output = NULL;
		share.push.spent = false;
	}
}

static void
drag_reset(void)
{
	struct share_edge *edge = share.drag.edge;
	bool through = share.drag.through;
	share.drag.edge = NULL;
	share.drag.view = NULL;
	share.drag.pushed = 0;
	share.drag.through = false;
	if (edge && through) {
		zsingularity_input_share_edge_v1_send_window_left(edge->resource);
	}
}

static void
drag_through(struct share_edge *edge, struct view *view, double position)
{
	pid_t pid = 0;
	if (view->surface && view->surface->resource) {
		wl_client_get_credentials(
			wl_resource_get_client(view->surface->resource),
			&pid, NULL, NULL);
	}
	double length = edge->end - edge->start;
	share.drag.through = true;
	zsingularity_input_share_edge_v1_send_window_entered(edge->resource,
		view->app_id ? view->app_id : "", pid,
		wl_fixed_from_double(position),
		wl_fixed_from_double(length > 0 ? position / length : 0));
}

static void
drag(struct seat *seat, struct view *view, double dx, double dy, uint32_t msec)
{
	double cx = seat->cursor->x, cy = seat->cursor->y, position;
	if (share.drag.edge && (share.drag.view != view
			|| !on_edge(share.drag.edge, cx, cy, dx, dy, false, &position))) {
		drag_reset();
	}
	struct share_edge *edge;
	wl_list_for_each(edge, &share.edges, link) {
		if (wl_resource_get_version(edge->resource)
				< ZSINGULARITY_INPUT_SHARE_EDGE_V1_WINDOW_ENTERED_SINCE_VERSION
				|| !on_edge(edge, cx, cy, dx, dy, true, &position)) {
			continue;
		}
		if (share.drag.edge != edge) {
			drag_reset();
			share.drag.edge = edge;
			share.drag.view = view;
		}
		if (share.drag.through) {
			return;
		}
		if (msec - share.drag.msec > PUSH_PAUSE_MSEC) {
			share.drag.pushed = 0;
		}
		share.drag.msec = msec;
		share.drag.pushed += vertical(edge->side) ? fabs(dx) : fabs(dy);
		if (share.drag.pushed >= PUSH_DISTANCE) {
			drag_through(edge, view, position - edge->start);
		}
		return;
	}
}

bool
singularity_input_share_drop(struct view *view)
{
	if (!share.drag.edge || share.drag.view != view || !share.drag.through) {
		drag_reset();
		return false;
	}
	struct share_edge *edge = share.drag.edge;
	share.drag.through = false;
	drag_reset();
	zsingularity_input_share_edge_v1_send_window_dropped(edge->resource);
	return true;
}

void
singularity_input_share_drag_cancel(struct view *view)
{
	if (share.drag.view == view) {
		drag_reset();
	}
}

bool
singularity_input_share_cross(struct seat *seat, double dx, double dy,
		uint32_t msec)
{
	if (share.active || server.session_lock_manager->locked) {
		return false;
	}
	if (server.input_mode == LAB_INPUT_STATE_MOVE && server.grabbed_view) {
		drag(seat, server.grabbed_view, dx, dy, msec);
		return false;
	}
	if (server.input_mode != LAB_INPUT_STATE_PASSTHROUGH) {
		return false;
	}
	if (cross_edge(seat, dx, dy, msec)) {
		return true;
	}
	if (!wl_list_empty(&share.probes)) {
		probe(seat, dx, dy, msec);
	}
	return false;
}

void
singularity_input_share_motion(double dx, double dy)
{
	if (share.active) {
		zsingularity_input_share_edge_v1_send_motion(share.active->resource,
			wl_fixed_from_double(dx), wl_fixed_from_double(dy));
	}
}

void
singularity_input_share_button(uint32_t button, bool pressed)
{
	if (share.active) {
		zsingularity_input_share_edge_v1_send_button(share.active->resource,
			button, pressed);
	}
}

void
singularity_input_share_axis(uint32_t axis, double value, int32_t discrete)
{
	if (share.active) {
		zsingularity_input_share_edge_v1_send_axis(share.active->resource,
			axis, wl_fixed_from_double(value), discrete);
	}
}

bool
singularity_input_share_key(struct keyboard *keyboard,
		struct wlr_keyboard_key_event *event)
{
	if (!share.active) {
		return false;
	}
	bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
	uint32_t mods = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	if (pressed && event->keycode == KEY_ESC
			&& (mods & WLR_MODIFIER_CTRL) && (mods & WLR_MODIFIER_LOGO)) {
		finish(ZSINGULARITY_INPUT_SHARE_EDGE_V1_REASON_ESCAPED, false, 0);
		return true;
	}
	zsingularity_input_share_edge_v1_send_key(share.active->resource,
		event->keycode, pressed);
	return true;
}

void
singularity_input_share_cancel(void)
{
	finish(ZSINGULARITY_INPUT_SHARE_EDGE_V1_REASON_CANCELLED, false, 0);
}

static void
handle_edge_destroy(struct wl_client *wl_client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_release(struct wl_client *wl_client, struct wl_resource *resource,
		wl_fixed_t position)
{
	struct share_edge *edge = wl_resource_get_user_data(resource);
	if (edge && share.active == edge) {
		finish(ZSINGULARITY_INPUT_SHARE_EDGE_V1_REASON_RELEASED, true,
			edge->start + wl_fixed_to_double(position));
	}
}

static void
handle_set_resistance(struct wl_client *wl_client, struct wl_resource *resource,
		uint32_t distance)
{
	struct share_edge *edge = wl_resource_get_user_data(resource);
	if (edge) {
		edge->resistance = distance;
		edge->pressed = 0;
		edge->reported = 0;
	}
}

static const struct zsingularity_input_share_edge_v1_interface edge_impl = {
	.destroy = handle_edge_destroy,
	.release = handle_release,
	.set_resistance = handle_set_resistance,
};

static void
edge_resource_destroy(struct wl_resource *resource)
{
	struct share_edge *edge = wl_resource_get_user_data(resource);
	if (!edge) {
		return;
	}
	if (share.active == edge) {
		finish(ZSINGULARITY_INPUT_SHARE_EDGE_V1_REASON_CANCELLED, false, 0);
	}
	if (share.drag.edge == edge) {
		share.drag.through = false;
		drag_reset();
	}
	wl_list_remove(&edge->link);
	free(edge->output);
	free(edge);
}

static void
handle_manager_destroy(struct wl_client *wl_client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_get_edge(struct wl_client *wl_client, struct wl_resource *resource,
		uint32_t id, const char *output, uint32_t side, int32_t start,
		int32_t end)
{
	struct wl_resource *edge_resource = wl_resource_create(wl_client,
		&zsingularity_input_share_edge_v1_interface,
		wl_resource_get_version(resource), id);
	if (!edge_resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	if (side > ZSINGULARITY_INPUT_SHARE_MANAGER_V1_SIDE_BOTTOM || end <= start) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_METHOD,
			"invalid edge");
		return;
	}
	struct share_edge *edge = calloc(1, sizeof(*edge));
	if (!edge) {
		wl_resource_destroy(edge_resource);
		wl_client_post_no_memory(wl_client);
		return;
	}
	edge->resource = edge_resource;
	edge->output = xstrdup(output);
	edge->side = side;
	edge->start = start;
	edge->end = end;
	wl_list_insert(&share.edges, &edge->link);
	wl_resource_set_implementation(edge_resource, &edge_impl, edge,
		edge_resource_destroy);
}

static void
handle_probe_destroy(struct wl_client *wl_client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zsingularity_input_share_probe_v1_interface probe_impl = {
	.destroy = handle_probe_destroy,
};

static void
probe_resource_destroy(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

static void
handle_get_probe(struct wl_client *wl_client, struct wl_resource *resource,
		uint32_t id)
{
	struct wl_resource *probe_resource = wl_resource_create(wl_client,
		&zsingularity_input_share_probe_v1_interface,
		wl_resource_get_version(resource), id);
	if (!probe_resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(probe_resource, &probe_impl, NULL,
		probe_resource_destroy);
	wl_list_insert(&share.probes, wl_resource_get_link(probe_resource));
}

static const struct zsingularity_input_share_manager_v1_interface manager_impl = {
	.destroy = handle_manager_destroy,
	.get_edge = handle_get_edge,
	.get_probe = handle_get_probe,
};

static void
bind_manager(struct wl_client *wl_client, void *data, uint32_t version,
		uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(wl_client,
		&zsingularity_input_share_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

void
singularity_input_share_init(void)
{
	wl_list_init(&share.edges);
	wl_list_init(&share.probes);
	share.global = wl_global_create(server.wl_display,
		&zsingularity_input_share_manager_v1_interface, 2, NULL,
		bind_manager);
}
