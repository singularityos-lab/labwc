// SPDX-License-Identifier: GPL-2.0-only
#include "protocols/singularity-input-share.h"
#include <linux/input-event-codes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/backend/wayland.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/util/log.h>
#include "input/cursor.h"
#include "input/keyboard.h"
#include "common/macros.h"
#include "common/mem.h"
#include "foreign-toplevel/foreign.h"
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

struct share_window {
	struct wl_resource *resource;
	struct view *view;
	struct xkb_state *xkb;
	double x, y;
	uint32_t buttons[8];
	int nbuttons;
	uint32_t keys[32];
	int nkeys;
	struct wlr_box geometry;
	struct wl_listener view_destroy;
	struct wl_listener commit;
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
	int windows;
	struct share_window *holding;
	struct wlr_seat_pointer_grab grab;
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

static uint32_t
now_msec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint32_t
singularity_input_share_capabilities(void)
{
	return share.windows > 0
		? WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD : 0;
}

static void window_hold(struct share_window *window, bool hold);

static void
window_close(struct share_window *window)
{
	if (!window->view) {
		return;
	}
	window->view = NULL;
	window_hold(window, false);
	window->nbuttons = 0;
	wl_list_remove(&window->view_destroy.link);
	wl_list_init(&window->view_destroy.link);
	wl_list_remove(&window->commit.link);
	wl_list_init(&window->commit.link);
	zsingularity_input_share_window_v1_send_closed(window->resource);
}

static void
handle_window_view_destroy(struct wl_listener *listener, void *data)
{
	struct share_window *window =
		wl_container_of(listener, window, view_destroy);
	window_close(window);
}

static void
extents(struct wlr_scene_node *node, int lx, int ly, int *x1, int *y1,
		int *x2, int *y2)
{
	int w = 0, h = 0;
	switch (node->type) {
	case WLR_SCENE_NODE_TREE:;
		struct wlr_scene_node *child;
		wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link) {
			extents(child, lx + child->x, ly + child->y, x1, y1, x2, y2);
		}
		return;
	case WLR_SCENE_NODE_RECT:
		w = wlr_scene_rect_from_node(node)->width;
		h = wlr_scene_rect_from_node(node)->height;
		break;
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		if (buffer->dst_width > 0 && buffer->dst_height > 0) {
			w = buffer->dst_width;
			h = buffer->dst_height;
		} else if (buffer->buffer) {
			w = buffer->buffer->width;
			h = buffer->buffer->height;
			if (buffer->transform & WL_OUTPUT_TRANSFORM_90) {
				int t = w;
				w = h;
				h = t;
			}
		}
		break;
	}
	*x1 = MIN(*x1, lx);
	*y1 = MIN(*y1, ly);
	*x2 = MAX(*x2, lx + w);
	*y2 = MAX(*y2, ly + h);
}

static bool
image_box(struct view *view, struct wlr_box *box)
{
	struct wlr_scene_node *root = &view->capture.scene->tree.node;
	int lx = 0, ly = 0;
	wlr_scene_node_coords(root, &lx, &ly);
	int x1 = INT_MAX, y1 = INT_MAX, x2 = INT_MIN, y2 = INT_MIN;
	extents(root, lx, ly, &x1, &y1, &x2, &y2);
	if (x1 >= x2 || y1 >= y2) {
		return false;
	}
	*box = (struct wlr_box){ x1, y1, x2 - x1, y2 - y1 };
	return true;
}

static void
window_geometry(struct share_window *window)
{
	struct wlr_box image;
	if (!window->view || !image_box(window->view, &image)) {
		return;
	}
	struct wlr_box g = {
		.x = window->view->current.x, .y = window->view->current.y,
		.width = window->view->current.width,
		.height = window->view->current.height,
	};
	struct wlr_box content = { 0, 0, g.width, g.height };
	struct wlr_xdg_surface *xdg =
		wlr_xdg_surface_try_from_wlr_surface(window->view->surface);
	if (xdg) {
		content.x = xdg->geometry.x;
		content.y = xdg->geometry.y;
		content.width = xdg->geometry.width;
		content.height = xdg->geometry.height;
	}
	struct wlr_box geometry = {
		content.x - image.x, content.y - image.y,
		content.width, content.height,
	};
	if (wlr_box_empty(&geometry)) {
		geometry = (struct wlr_box){ 0, 0, image.width, image.height };
	}
	if (wlr_box_equal(&geometry, &window->geometry)) {
		return;
	}
	window->geometry = geometry;
	zsingularity_input_share_window_v1_send_geometry(window->resource,
		geometry.x, geometry.y, geometry.width, geometry.height);
}

static void
handle_window_commit(struct wl_listener *listener, void *data)
{
	struct share_window *window = wl_container_of(listener, window, commit);
	window_geometry(window);
}

static struct wlr_surface *
window_surface_at(struct share_window *window, double x, double y,
		double *sx, double *sy)
{
	struct wlr_scene_node *root = &window->view->capture.scene->tree.node;
	struct wlr_box image;
	if (!image_box(window->view, &image)) {
		return NULL;
	}
	int x1 = image.x, y1 = image.y;
	struct wlr_scene_node *node = wlr_scene_node_at(root, x1 + x, y1 + y,
		sx, sy);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_surface *surface = wlr_scene_surface_try_from_buffer(
		wlr_scene_buffer_from_node(node));
	return surface ? surface->surface : NULL;
}

static struct share_window *
window_from(struct wl_resource *resource)
{
	struct share_window *window = wl_resource_get_user_data(resource);
	return window && window->view ? window : NULL;
}

static void grab_enter(struct wlr_seat_pointer_grab *grab,
		struct wlr_surface *surface, double sx, double sy) {}
static void grab_clear_focus(struct wlr_seat_pointer_grab *grab) {}
static void grab_motion(struct wlr_seat_pointer_grab *grab, uint32_t msec,
		double sx, double sy) {}
static uint32_t grab_button(struct wlr_seat_pointer_grab *grab, uint32_t msec,
		uint32_t button, enum wl_pointer_button_state state) { return 0; }
static void grab_axis(struct wlr_seat_pointer_grab *grab, uint32_t msec,
		enum wl_pointer_axis orientation, double value, int32_t discrete,
		enum wl_pointer_axis_source source,
		enum wl_pointer_axis_relative_direction direction) {}
static void grab_frame(struct wlr_seat_pointer_grab *grab) {}

static void
grab_cancel(struct wlr_seat_pointer_grab *grab)
{
	share.holding = NULL;
}

static const struct wlr_pointer_grab_interface grab_impl = {
	.enter = grab_enter,
	.clear_focus = grab_clear_focus,
	.motion = grab_motion,
	.button = grab_button,
	.axis = grab_axis,
	.frame = grab_frame,
	.cancel = grab_cancel,
};

static void
window_hold(struct share_window *window, bool hold)
{
	struct wlr_seat *seat = server.seat.wlr_seat;
	if (hold && !share.holding) {
		share.grab.interface = &grab_impl;
		share.grab.data = window;
		wlr_seat_pointer_start_grab(seat, &share.grab);
		share.holding = window;
	} else if (!hold && share.holding == window) {
		share.holding = NULL;
		if (seat->pointer_state.grab == &share.grab) {
			wlr_seat_pointer_end_grab(seat);
		}
	}
}

static void
window_pointer_at(struct share_window *window, uint32_t msec)
{
	struct wlr_seat *seat = server.seat.wlr_seat;
	double sx, sy;
	struct wlr_surface *surface =
		window_surface_at(window, window->x, window->y, &sx, &sy);
	if (!surface) {
		return;
	}
	if (seat->pointer_state.focused_surface != surface) {
		wlr_seat_pointer_enter(seat, surface, sx, sy);
	}
	wlr_seat_pointer_send_motion(seat, msec, sx, sy);
}

static void
window_activate(struct share_window *window)
{
	if (server.seat.wlr_seat->keyboard_state.focused_surface
			!= window->view->surface) {
		desktop_focus_view(window->view, false);
	}
}

static void
handle_window_destroy(struct wl_client *wl_client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_window_motion(struct wl_client *wl_client, struct wl_resource *resource,
		wl_fixed_t x, wl_fixed_t y)
{
	struct share_window *window = window_from(resource);
	if (!window) {
		return;
	}
	window_geometry(window);
	window->x = wl_fixed_to_double(x);
	window->y = wl_fixed_to_double(y);
	window_pointer_at(window, now_msec());
	wlr_seat_pointer_send_frame(server.seat.wlr_seat);
}

static bool
held(uint32_t *list, int *n, int cap, uint32_t code, bool pressed)
{
	for (int i = 0; i < *n; i++) {
		if (list[i] != code) {
			continue;
		}
		if (!pressed) {
			list[i] = list[--*n];
		}
		return !pressed;
	}
	if (!pressed || *n >= cap) {
		return false;
	}
	list[(*n)++] = code;
	return true;
}

static void
handle_window_button(struct wl_client *wl_client, struct wl_resource *resource,
		uint32_t button, uint32_t state)
{
	struct share_window *window = window_from(resource);
	bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (!window || !held(window->buttons, &window->nbuttons, 8, button, pressed)) {
		return;
	}
	uint32_t msec = now_msec();
	if (pressed) {
		window_activate(window);
		window_hold(window, true);
	}
	window_pointer_at(window, msec);
	wlr_seat_pointer_send_button(server.seat.wlr_seat, msec, button,
		pressed ? WL_POINTER_BUTTON_STATE_PRESSED
			: WL_POINTER_BUTTON_STATE_RELEASED);
	wlr_seat_pointer_send_frame(server.seat.wlr_seat);
	if (window->nbuttons == 0) {
		window_hold(window, false);
	}
}

static void
handle_window_axis(struct wl_client *wl_client, struct wl_resource *resource,
		uint32_t axis, int32_t steps)
{
	struct share_window *window = window_from(resource);
	if (!window || axis > WL_POINTER_AXIS_HORIZONTAL_SCROLL || !steps) {
		return;
	}
	uint32_t msec = now_msec();
	window_pointer_at(window, msec);
	wlr_seat_pointer_send_axis(server.seat.wlr_seat, msec, axis,
		15.0 * steps, 120 * steps, WL_POINTER_AXIS_SOURCE_WHEEL,
		WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
	wlr_seat_pointer_send_frame(server.seat.wlr_seat);
}

static void
window_modifiers(struct share_window *window)
{
	struct wlr_keyboard_modifiers mods = {
		.depressed = xkb_state_serialize_mods(window->xkb, XKB_STATE_MODS_DEPRESSED),
		.latched = xkb_state_serialize_mods(window->xkb, XKB_STATE_MODS_LATCHED),
		.locked = xkb_state_serialize_mods(window->xkb, XKB_STATE_MODS_LOCKED),
		.group = xkb_state_serialize_layout(window->xkb, XKB_STATE_LAYOUT_EFFECTIVE),
	};
	wlr_seat_keyboard_notify_modifiers(server.seat.wlr_seat, &mods);
}

static void
handle_window_key(struct wl_client *wl_client, struct wl_resource *resource,
		uint32_t key, uint32_t state)
{
	struct share_window *window = window_from(resource);
	bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	if (!window || !held(window->keys, &window->nkeys, 32, key, pressed)) {
		return;
	}
	struct xkb_keymap *keymap = server.seat.keyboard_group->keyboard.keymap;
	if (!keymap) {
		return;
	}
	if (window->xkb && xkb_state_get_keymap(window->xkb) != keymap) {
		xkb_state_unref(window->xkb);
		window->xkb = NULL;
	}
	if (!window->xkb) {
		window->xkb = xkb_state_new(keymap);
		if (!window->xkb) {
			return;
		}
	}
	if (!wlr_seat_get_keyboard(server.seat.wlr_seat)) {
		wlr_seat_set_keyboard(server.seat.wlr_seat,
			&server.seat.keyboard_group->keyboard);
	}
	if (pressed) {
		window_activate(window);
	}
	xkb_state_update_key(window->xkb, key + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
	wlr_seat_keyboard_notify_key(server.seat.wlr_seat, now_msec(), key,
		pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
			: WL_KEYBOARD_KEY_STATE_RELEASED);
	window_modifiers(window);
}

static const struct zsingularity_input_share_window_v1_interface window_impl = {
	.destroy = handle_window_destroy,
	.motion = handle_window_motion,
	.button = handle_window_button,
	.axis = handle_window_axis,
	.key = handle_window_key,
};

static void
window_resource_destroy(struct wl_resource *resource)
{
	struct share_window *window = wl_resource_get_user_data(resource);
	if (window->view) {
		uint32_t msec = now_msec();
		while (window->nbuttons > 0) {
			wlr_seat_pointer_send_button(server.seat.wlr_seat, msec,
				window->buttons[--window->nbuttons],
				WL_POINTER_BUTTON_STATE_RELEASED);
		}
		wlr_seat_pointer_send_frame(server.seat.wlr_seat);
		while (window->nkeys > 0) {
			uint32_t key = window->keys[--window->nkeys];
			if (window->xkb) {
				xkb_state_update_key(window->xkb, key + 8, XKB_KEY_UP);
			}
			wlr_seat_keyboard_notify_key(server.seat.wlr_seat, msec, key,
				WL_KEYBOARD_KEY_STATE_RELEASED);
		}
		if (window->xkb) {
			window_modifiers(window);
		}
	}
	window_hold(window, false);
	wl_list_remove(&window->view_destroy.link);
	wl_list_remove(&window->commit.link);
	if (window->xkb) {
		xkb_state_unref(window->xkb);
	}
	free(window);
	if (--share.windows == 0) {
		seat_update_capabilities(&server.seat);
	}
}

static struct view *
find_window(const char *app_id, int32_t pid)
{
	struct view *view;
	wl_list_for_each(view, &server.views, link) {
		if (!view->mapped || !view->surface || !view->surface->resource
				|| !view->foreign_toplevel || !view->app_id
				|| strcmp(view->app_id, app_id)) {
			continue;
		}
		if (pid > 0) {
			pid_t client_pid = 0;
			wl_client_get_credentials(
				wl_resource_get_client(view->surface->resource),
				&client_pid, NULL, NULL);
			if (client_pid != pid) {
				continue;
			}
		}
		return view;
	}
	return NULL;
}

static void
handle_get_window(struct wl_client *wl_client, struct wl_resource *resource,
		uint32_t id, const char *app_id, int32_t pid)
{
	struct wl_resource *window_resource = wl_resource_create(wl_client,
		&zsingularity_input_share_window_v1_interface,
		wl_resource_get_version(resource), id);
	if (!window_resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	struct share_window *window = znew(*window);
	window->resource = window_resource;
	wl_list_init(&window->view_destroy.link);
	wl_list_init(&window->commit.link);
	wl_resource_set_implementation(window_resource, &window_impl, window,
		window_resource_destroy);
	if (share.windows++ == 0) {
		seat_update_capabilities(&server.seat);
	}
	struct view *view = find_window(app_id, pid);
	struct wlr_ext_foreign_toplevel_handle_v1 *handle = view
		? foreign_toplevel_get_ext_handle(view->foreign_toplevel) : NULL;
	if (!handle || !handle->identifier) {
		zsingularity_input_share_window_v1_send_closed(window_resource);
		return;
	}
	window->view = view;
	window->view_destroy.notify = handle_window_view_destroy;
	wl_signal_add(&view->events.destroy, &window->view_destroy);
	window->commit.notify = handle_window_commit;
	wl_signal_add(&view->surface->events.commit, &window->commit);
	zsingularity_input_share_window_v1_send_identifier(window_resource,
		handle->identifier);
	window_geometry(window);
}

static const struct zsingularity_input_share_manager_v1_interface manager_impl = {
	.destroy = handle_manager_destroy,
	.get_edge = handle_get_edge,
	.get_probe = handle_get_probe,
	.get_window = handle_get_window,
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
		&zsingularity_input_share_manager_v1_interface, 3, NULL,
		bind_manager);
}
