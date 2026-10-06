// SPDX-License-Identifier: GPL-2.0-only
#include "protocols/singularity-snap.h"
#include <math.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <scenefx/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "common/node-type.h"
#include "config/rcxml.h"
#include "foreign-toplevel/foreign.h"
#include "labwc.h"
#include "node.h"
#include "output.h"
#include "overlay.h"
#include "protocols/singularity-tiling.h"
#include "regions.h"
#include "singularity-snap-unstable-v1-protocol.h"
#include "ssd-internal.h"
#include "theme.h"
#include "view.h"

#define SNAP_HOVER_DELAY_MS 400
#define SNAP_TOP_BAND 16
#define SNAP_AREA_SLACK 24

struct snap_client {
	struct wl_resource *resource;
	bool enabled;
	struct wl_list link;
};

struct singularity_snap_manager {
	struct wl_global *global;
	struct wl_list clients;
	struct wl_event_source *hover_timer;
	struct ssd_button *hover_button;
	struct wlr_surface *client_surface;
	struct wl_listener client_surface_destroy;
	struct wlr_box client_anchor;
	bool button_visible;
	bool drag_visible;
	struct wlr_box picker_area;
	bool preview_active;
	struct wlr_box preview_box;
};

static struct singularity_snap_manager *snap_manager;

bool
singularity_snap_enabled(void)
{
	if (!snap_manager || singularity_tiling_scrolling_mode_enabled()) {
		return false;
	}
	struct snap_client *client;
	wl_list_for_each(client, &snap_manager->clients, link) {
		if (client->enabled) {
			return true;
		}
	}
	return false;
}

static struct wl_resource *
toplevel_resource_for_client(struct view *view, struct wl_client *client)
{
	if (!view || !view->foreign_toplevel) {
		return NULL;
	}
	struct wlr_foreign_toplevel_handle_v1 *handle =
		foreign_toplevel_get_handle(view->foreign_toplevel);
	if (!handle) {
		return NULL;
	}
	struct wl_resource *resource;
	wl_resource_for_each(resource, &handle->resources) {
		if (wl_resource_get_client(resource) == client) {
			return resource;
		}
	}
	return NULL;
}

static void
send_show(struct view *view, uint32_t source, struct wlr_box anchor,
		struct wlr_box area)
{
	struct snap_client *client;
	wl_list_for_each(client, &snap_manager->clients, link) {
		if (!client->enabled) {
			continue;
		}
		struct wl_resource *toplevel = toplevel_resource_for_client(view,
			wl_resource_get_client(client->resource));
		zsingularity_snap_manager_v1_send_picker_show(client->resource,
			toplevel, source, anchor.x, anchor.y, anchor.width,
			anchor.height, area.x, area.y, area.width, area.height);
	}
}

static void
send_hide(uint32_t source)
{
	struct snap_client *client;
	wl_list_for_each(client, &snap_manager->clients, link) {
		if (client->enabled) {
			zsingularity_snap_manager_v1_send_picker_hide(
				client->resource, source);
		}
	}
}

static void
send_motion(int x, int y)
{
	struct snap_client *client;
	wl_list_for_each(client, &snap_manager->clients, link) {
		if (client->enabled) {
			zsingularity_snap_manager_v1_send_picker_motion(
				client->resource, x, y);
		}
	}
}

static void
send_drop(struct view *view, int x, int y)
{
	struct snap_client *client;
	wl_list_for_each(client, &snap_manager->clients, link) {
		if (!client->enabled) {
			continue;
		}
		struct wl_resource *toplevel = toplevel_resource_for_client(view,
			wl_resource_get_client(client->resource));
		zsingularity_snap_manager_v1_send_picker_drop(client->resource,
			toplevel, x, y);
	}
}

static int
cursor_lx(void)
{
	return (int)round(server.seat.cursor->x);
}

static int
cursor_ly(void)
{
	return (int)round(server.seat.cursor->y);
}

static bool
cursor_in_area(void)
{
	if (wlr_box_empty(&snap_manager->picker_area)) {
		return false;
	}
	struct wlr_box area = snap_manager->picker_area;
	area.x -= SNAP_AREA_SLACK;
	area.y -= SNAP_AREA_SLACK;
	area.width += SNAP_AREA_SLACK * 2;
	area.height += SNAP_AREA_SLACK * 2;
	return wlr_box_contains_point(&area, server.seat.cursor->x,
		server.seat.cursor->y);
}

static void
reset_drag(void)
{
	snap_manager->drag_visible = false;
	snap_manager->preview_active = false;
	snap_manager->picker_area = (struct wlr_box){0};
}

static void
clear_client_button(void)
{
	if (!snap_manager->client_surface) {
		return;
	}
	wl_list_remove(&snap_manager->client_surface_destroy.link);
	wl_list_init(&snap_manager->client_surface_destroy.link);
	snap_manager->client_surface = NULL;
}

static void
handle_client_surface_destroy(struct wl_listener *listener, void *data)
{
	bool visible = snap_manager->button_visible
		&& !snap_manager->hover_button;
	clear_client_button();
	wl_event_source_timer_update(snap_manager->hover_timer, 0);
	if (visible) {
		send_hide(ZSINGULARITY_SNAP_MANAGER_V1_SOURCE_BUTTON);
		snap_manager->button_visible = false;
	}
}

static bool
pointer_over_surface(struct wlr_surface *surface)
{
	struct wlr_surface *focused =
		server.seat.wlr_seat->pointer_state.focused_surface;
	return focused && wlr_surface_get_root_surface(focused) == surface;
}

static void
show_for_client_button(void)
{
	struct wlr_surface *surface = snap_manager->client_surface;
	struct wlr_xdg_surface *xdg_surface =
		wlr_xdg_surface_try_from_wlr_surface(surface);
	struct view *view = view_from_wlr_surface(surface);
	if (!xdg_surface || !view || !view->content_tree || view->fullscreen
			|| !output_is_usable(view->output)
			|| server.input_mode != LAB_INPUT_STATE_PASSTHROUGH
			|| !pointer_over_surface(surface)
			|| !singularity_snap_enabled()) {
		return;
	}
	int lx = 0;
	int ly = 0;
	wlr_scene_node_coords(&view->content_tree->node, &lx, &ly);
	struct wlr_box anchor = snap_manager->client_anchor;
	anchor.x += lx - xdg_surface->geometry.x;
	anchor.y += ly - xdg_surface->geometry.y;
	send_show(view, ZSINGULARITY_SNAP_MANAGER_V1_SOURCE_BUTTON, anchor,
		output_usable_area_in_layout_coords(view->output));
	snap_manager->button_visible = true;
}

static int
handle_hover_timeout(void *data)
{
	struct ssd_button *button = snap_manager->hover_button;
	if (!button && snap_manager->client_surface) {
		show_for_client_button();
		return 0;
	}
	if (!button || button != server.hovered_button
			|| server.input_mode != LAB_INPUT_STATE_PASSTHROUGH
			|| !singularity_snap_enabled()) {
		return 0;
	}
	struct view *view = node_view_from_node(button->node);
	if (!view || view->fullscreen || !output_is_usable(view->output)) {
		return 0;
	}
	int lx = 0;
	int ly = 0;
	wlr_scene_node_coords(button->node, &lx, &ly);
	struct wlr_box anchor = {
		.x = lx,
		.y = ly,
		.width = rc.theme->window_button_width,
		.height = rc.theme->window_button_height,
	};
	send_show(view, ZSINGULARITY_SNAP_MANAGER_V1_SOURCE_BUTTON, anchor,
		output_usable_area_in_layout_coords(view->output));
	snap_manager->button_visible = true;
	return 0;
}

void
singularity_snap_button_hovered(struct ssd_button *button)
{
	if (!snap_manager) {
		return;
	}
	bool maximize = button && button->type == LAB_NODE_BUTTON_MAXIMIZE;
	if (maximize && button == snap_manager->hover_button) {
		return;
	}
	if (!maximize && !snap_manager->hover_button) {
		return;
	}
	clear_client_button();
	if (snap_manager->button_visible) {
		send_hide(ZSINGULARITY_SNAP_MANAGER_V1_SOURCE_BUTTON);
		snap_manager->button_visible = false;
	}
	snap_manager->hover_button = maximize ? button : NULL;
	if (!maximize || !singularity_snap_enabled()) {
		wl_event_source_timer_update(snap_manager->hover_timer, 0);
		return;
	}
	wl_event_source_timer_update(snap_manager->hover_timer,
		SNAP_HOVER_DELAY_MS);
}

void
singularity_snap_button_pressed(void)
{
	if (!snap_manager) {
		return;
	}
	wl_event_source_timer_update(snap_manager->hover_timer, 0);
	if (snap_manager->button_visible) {
		send_hide(ZSINGULARITY_SNAP_MANAGER_V1_SOURCE_BUTTON);
		snap_manager->button_visible = false;
	}
}

void
singularity_snap_move_update(struct view *view)
{
	if (!snap_manager) {
		return;
	}
	if (!singularity_snap_enabled() || !view || !view_is_floating(view)) {
		if (snap_manager->drag_visible) {
			send_hide(ZSINGULARITY_SNAP_MANAGER_V1_SOURCE_DRAG);
			reset_drag();
		}
		return;
	}

	struct output *output = output_nearest_to_cursor();
	if (!output_is_usable(output)) {
		return;
	}
	struct wlr_box area = output_usable_area_in_layout_coords(output);
	int x = cursor_lx();
	int y = cursor_ly();
	bool in_band = y < area.y + SNAP_TOP_BAND
		&& x - area.x >= rc.snap_edge_corner_range
		&& area.x + area.width - x >= rc.snap_edge_corner_range;
	bool in_area = cursor_in_area();

	if (!snap_manager->drag_visible && in_band) {
		struct wlr_box anchor = { .x = x, .y = y };
		send_show(view, ZSINGULARITY_SNAP_MANAGER_V1_SOURCE_DRAG,
			anchor, area);
		snap_manager->drag_visible = true;
	} else if (snap_manager->drag_visible && !in_band && !in_area) {
		send_hide(ZSINGULARITY_SNAP_MANAGER_V1_SOURCE_DRAG);
		reset_drag();
		return;
	}
	if (snap_manager->drag_visible) {
		send_motion(x, y);
	}
}

bool
singularity_snap_move_finish(struct view *view)
{
	if (!snap_manager || !snap_manager->drag_visible) {
		return false;
	}
	bool in_area = cursor_in_area();
	if (in_area) {
		send_drop(view, cursor_lx(), cursor_ly());
	} else {
		send_hide(ZSINGULARITY_SNAP_MANAGER_V1_SOURCE_DRAG);
	}
	reset_drag();
	return in_area;
}

void
singularity_snap_move_cancel(void)
{
	if (!snap_manager || !snap_manager->drag_visible) {
		return;
	}
	send_hide(ZSINGULARITY_SNAP_MANAGER_V1_SOURCE_DRAG);
	reset_drag();
}

bool
singularity_snap_cursor_in_picker(void)
{
	return snap_manager && snap_manager->drag_visible && cursor_in_area();
}

bool
singularity_snap_get_zone_preview(struct wlr_box *box)
{
	if (!snap_manager || !snap_manager->drag_visible
			|| !snap_manager->preview_active || !cursor_in_area()) {
		return false;
	}
	*box = snap_manager->preview_box;
	return true;
}

static void
handle_button_enter(struct wl_client *wl_client, struct wl_resource *resource,
		struct wl_resource *surface_resource, int32_t x, int32_t y,
		int32_t width, int32_t height)
{
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (!surface || !wlr_xdg_surface_try_from_wlr_surface(surface)
			|| !view_from_wlr_surface(surface) || width < 1
			|| height < 1) {
		return;
	}
	struct wlr_box anchor = {
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};
	if (snap_manager->client_surface == surface) {
		snap_manager->client_anchor = anchor;
		if (snap_manager->button_visible) {
			return;
		}
	} else {
		if (snap_manager->button_visible) {
			send_hide(ZSINGULARITY_SNAP_MANAGER_V1_SOURCE_BUTTON);
			snap_manager->button_visible = false;
		}
		clear_client_button();
		snap_manager->hover_button = NULL;
		snap_manager->client_surface = surface;
		snap_manager->client_anchor = anchor;
		wl_signal_add(&surface->events.destroy,
			&snap_manager->client_surface_destroy);
	}
	if (!singularity_snap_enabled()) {
		wl_event_source_timer_update(snap_manager->hover_timer, 0);
		return;
	}
	wl_event_source_timer_update(snap_manager->hover_timer,
		SNAP_HOVER_DELAY_MS);
}

static void
handle_button_leave(struct wl_client *wl_client, struct wl_resource *resource,
		struct wl_resource *surface_resource)
{
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (!surface || snap_manager->client_surface != surface) {
		return;
	}
	clear_client_button();
	wl_event_source_timer_update(snap_manager->hover_timer, 0);
	if (snap_manager->button_visible) {
		send_hide(ZSINGULARITY_SNAP_MANAGER_V1_SOURCE_BUTTON);
		snap_manager->button_visible = false;
	}
}

static void
handle_set_enabled(struct wl_client *wl_client, struct wl_resource *resource,
		uint32_t enabled)
{
	struct snap_client *client = wl_resource_get_user_data(resource);
	client->enabled = enabled != 0;
	if (!singularity_snap_enabled()) {
		wl_event_source_timer_update(snap_manager->hover_timer, 0);
		snap_manager->button_visible = false;
		reset_drag();
	}
}

static void
handle_set_picker_area(struct wl_client *wl_client,
		struct wl_resource *resource, int32_t x, int32_t y,
		int32_t width, int32_t height)
{
	snap_manager->picker_area = (struct wlr_box){
		.x = x,
		.y = y,
		.width = width > 0 ? width : 0,
		.height = height > 0 ? height : 0,
	};
}

static void
handle_set_zone_preview(struct wl_client *wl_client,
		struct wl_resource *resource, int32_t x, int32_t y,
		int32_t width, int32_t height, uint32_t visible)
{
	snap_manager->preview_active = visible && width > 0 && height > 0;
	snap_manager->preview_box = (struct wlr_box){
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};
	if (snap_manager->drag_visible && server.grabbed_view
			&& server.input_mode == LAB_INPUT_STATE_MOVE) {
		overlay_update(&server.seat);
	}
}

static void
handle_snap_to_zone(struct wl_client *wl_client, struct wl_resource *resource,
		struct wl_resource *toplevel_resource, int32_t ref_x,
		int32_t ref_y, uint32_t x, uint32_t y, uint32_t width,
		uint32_t height)
{
	struct wlr_foreign_toplevel_handle_v1 *handle =
		wl_resource_get_user_data(toplevel_resource);
	struct view *view = handle ? handle->data : NULL;
	if (!view || view->fullscreen || width < 1 || height < 1
			|| x + width > 1000 || y + height > 1000) {
		return;
	}
	struct output *output = output_nearest_to(ref_x, ref_y);
	if (!output_is_usable(output)) {
		output = view->output;
	}
	if (!output_is_usable(output)) {
		return;
	}
	struct region *region = regions_snap_zone(output, (struct wlr_box){
		.x = (int)x,
		.y = (int)y,
		.width = (int)width,
		.height = (int)height,
	});
	if (view->minimized) {
		view_minimize(view, false);
	}
	if (view->output != output) {
		view_set_output(view, output);
	}
	view_snap_to_region(view, region);
}

static const struct zsingularity_snap_manager_v1_interface manager_impl = {
	.set_enabled = handle_set_enabled,
	.set_picker_area = handle_set_picker_area,
	.set_zone_preview = handle_set_zone_preview,
	.snap_to_zone = handle_snap_to_zone,
	.button_enter = handle_button_enter,
	.button_leave = handle_button_leave,
};

static void
resource_destroy(struct wl_resource *resource)
{
	struct snap_client *client = wl_resource_get_user_data(resource);
	if (!client) {
		return;
	}
	wl_list_remove(&client->link);
	free(client);
	if (snap_manager && !singularity_snap_enabled()) {
		wl_event_source_timer_update(snap_manager->hover_timer, 0);
		snap_manager->button_visible = false;
		reset_drag();
	}
}

static void
bind_manager(struct wl_client *wl_client, void *data, uint32_t version,
		uint32_t id)
{
	struct singularity_snap_manager *manager = data;
	struct snap_client *client = calloc(1, sizeof(*client));
	if (!client) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	client->resource = wl_resource_create(wl_client,
		&zsingularity_snap_manager_v1_interface, version, id);
	if (!client->resource) {
		free(client);
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(client->resource, &manager_impl, client,
		resource_destroy);
	wl_list_insert(&manager->clients, &client->link);
}

void
singularity_snap_init(void)
{
	snap_manager = calloc(1, sizeof(*snap_manager));
	if (!snap_manager) {
		return;
	}
	wl_list_init(&snap_manager->clients);
	wl_list_init(&snap_manager->client_surface_destroy.link);
	snap_manager->client_surface_destroy.notify =
		handle_client_surface_destroy;
	snap_manager->hover_timer = wl_event_loop_add_timer(
		server.wl_event_loop, handle_hover_timeout, NULL);
	snap_manager->global = wl_global_create(server.wl_display,
		&zsingularity_snap_manager_v1_interface, 2, snap_manager,
		bind_manager);
}
