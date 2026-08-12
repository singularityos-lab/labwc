// SPDX-License-Identifier: GPL-2.0-only
#include "protocols/singularity-tiling.h"
#include <math.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include "foreign-toplevel/foreign.h"
#include "labwc.h"
#include "output.h"
#include "overlay.h"
#include "singularity-tiling-unstable-v1-protocol.h"
#include "snap.h"
#include "view.h"
#include "view-animation.h"
#include "workspaces.h"

struct singularity_tiling_manager {
	struct wl_global *global;
	struct wl_list resources;
	bool scrolling_mode_enabled;
	bool drop_preview_active;
	struct wlr_box drop_preview_box;
};

static struct singularity_tiling_manager *tiling_manager;

static bool
view_is_tileable(struct view *view)
{
	return view && (!view->impl->get_parent || !view->impl->get_parent(view))
		&& (!view->impl->is_modal_dialog
			|| !view->impl->is_modal_dialog(view))
		&& (!view->impl->contains_window_type
			|| !view->impl->contains_window_type(view,
				LAB_WINDOW_TYPE_DIALOG));
}

static struct view *
view_from_toplevel_resource(struct wl_resource *resource)
{
	struct wlr_foreign_toplevel_handle_v1 *toplevel =
		wl_resource_get_user_data(resource);
	return toplevel ? toplevel->data : NULL;
}

static void
handle_set_geometry(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *toplevel_resource, int32_t x, int32_t y,
	int32_t width, int32_t height)
{
	struct view *view = view_from_toplevel_resource(toplevel_resource);
	if (!view || width < 1 || height < 1) {
		return;
	}
	view_move_resize(view, (struct wlr_box){
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	});
}

static void
handle_set_tiled(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *toplevel_resource, uint32_t tiled)
{
	struct view *view = view_from_toplevel_resource(toplevel_resource);
	if (!view) {
		return;
	}
	if (tiled) {
		if (!view_is_tileable(view)) {
			return;
		}
		if (!view->singularity_tiling_ssd_mode_valid) {
			view->singularity_tiling_ssd_mode = view->ssd_mode;
			view->singularity_tiling_ssd_mode_valid = true;
		}
		if (!view_is_tiled(view)) {
			view_store_natural_geometry(view);
		}
		view_maximize(view, VIEW_AXIS_NONE);
		view_animation_cancel(view);
		view->tiled = LAB_EDGE_TOP | LAB_EDGE_BOTTOM
			| LAB_EDGE_LEFT | LAB_EDGE_RIGHT;
		view->singularity_scrolling_tiled = true;
		view_set_decorations(view, LAB_SSD_MODE_BORDER, true);
	} else if (view_is_tiled(view)) {
		struct wlr_box natural = view->natural_geometry;
		enum lab_ssd_mode ssd_mode = view->singularity_tiling_ssd_mode_valid
			? view->singularity_tiling_ssd_mode : LAB_SSD_MODE_FULL;
		view->singularity_tiling_ssd_mode_valid = false;
		view->singularity_scrolling_tiled = false;
		view_set_untiled(view);
		view_set_decorations(view, ssd_mode, true);
		if (!wlr_box_empty(&natural)) {
			view_move_resize(view, natural);
		}
	} else {
		view->singularity_scrolling_tiled = false;
	}
}

static void
handle_get_tileable(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *toplevel_resource)
{
	struct view *view = view_from_toplevel_resource(toplevel_resource);
	zsingularity_tiling_manager_v1_send_tileable(resource, toplevel_resource,
		view_is_tileable(view));
}

static void
handle_set_scrolling_mode(struct wl_client *client, struct wl_resource *resource,
	uint32_t enabled)
{
	struct singularity_tiling_manager *manager =
		wl_resource_get_user_data(resource);
	manager->scrolling_mode_enabled = enabled != 0;
	if (!manager->scrolling_mode_enabled) {
		manager->drop_preview_active = false;
		overlay_finish(&server.seat);
	}
}

static void
handle_set_drop_preview(struct wl_client *client, struct wl_resource *resource,
	int32_t x, int32_t y, int32_t width, int32_t height, uint32_t visible)
{
	struct singularity_tiling_manager *manager =
		wl_resource_get_user_data(resource);
	manager->drop_preview_active = visible && width > 0 && height > 0;
	manager->drop_preview_box = (struct wlr_box){
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};
	overlay_update(&server.seat);
}

static void
handle_detach_tiled(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *toplevel_resource)
{
	struct view *view = view_from_toplevel_resource(toplevel_resource);
	if (!view || !view->singularity_scrolling_tiled) {
		return;
	}
	struct wlr_box geometry = view->pending;
	enum lab_ssd_mode ssd_mode = view->singularity_tiling_ssd_mode_valid
		? view->singularity_tiling_ssd_mode : LAB_SSD_MODE_FULL;
	view->singularity_tiling_ssd_mode_valid = false;
	view->singularity_scrolling_tiled = false;
	view_set_untiled(view);
	view->natural_geometry = geometry;
	view_set_decorations(view, ssd_mode, true);
}

static void
handle_snap_view(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *toplevel_resource, uint32_t direction)
{
	struct view *view = view_from_toplevel_resource(toplevel_resource);
	if (!view) {
		return;
	}
	enum lab_edge edge = LAB_EDGE_NONE;
	switch (direction) {
	case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_LEFT:
		edge = LAB_EDGE_LEFT;
		break;
	case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_RIGHT:
		edge = LAB_EDGE_RIGHT;
		break;
	case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_TOP:
		edge = LAB_EDGE_TOP;
		break;
	case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_BOTTOM:
		edge = LAB_EDGE_BOTTOM;
		break;
	case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_TOP_LEFT:
		edge = LAB_EDGE_TOP | LAB_EDGE_LEFT;
		break;
	case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_TOP_RIGHT:
		edge = LAB_EDGE_TOP | LAB_EDGE_RIGHT;
		break;
	case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_BOTTOM_LEFT:
		edge = LAB_EDGE_BOTTOM | LAB_EDGE_LEFT;
		break;
	case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_BOTTOM_RIGHT:
		edge = LAB_EDGE_BOTTOM | LAB_EDGE_RIGHT;
		break;
	case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_MAXIMIZE:
		view_maximize(view, VIEW_AXIS_BOTH);
		return;
	default:
		return;
	}
	view_snap_to_edge(view, edge, false, false);
}

static void
handle_get_geometry(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *toplevel_resource)
{
	struct view *view = view_from_toplevel_resource(toplevel_resource);
	if (!view) {
		return;
	}
	const char *output_name = "";
	if (view->output && view->output->wlr_output
			&& view->output->wlr_output->name) {
		output_name = view->output->wlr_output->name;
	}
	zsingularity_tiling_manager_v1_send_geometry(resource, toplevel_resource,
		view->current.x, view->current.y,
		view->current.width, view->current.height,
		view->maximized ? 1u : 0u, view->fullscreen ? 1u : 0u,
		output_name);
}

static void
handle_get_workarea(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *toplevel_resource)
{
	struct view *view = view_from_toplevel_resource(toplevel_resource);
	if (!view || !output_is_usable(view->output)) {
		return;
	}
	struct wlr_box box = output_usable_area_in_layout_coords(view->output);
	zsingularity_tiling_manager_v1_send_workarea(resource, toplevel_resource,
		box.x, box.y, box.width, box.height);
}

static void
handle_move_to_workspace(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *toplevel_resource, uint32_t workspace_index)
{
	struct view *view = view_from_toplevel_resource(toplevel_resource);
	if (!view) {
		return;
	}
	uint32_t index = 0;
	struct workspace *workspace;
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		if (index++ == workspace_index) {
			view_move_to_workspace(view, workspace);
			return;
		}
	}
}

static const struct zsingularity_tiling_manager_v1_interface manager_impl = {
	.get_geometry = handle_get_geometry,
	.set_geometry = handle_set_geometry,
	.set_tiled = handle_set_tiled,
	.snap_view = handle_snap_view,
	.move_to_workspace = handle_move_to_workspace,
	.get_workarea = handle_get_workarea,
	.set_scrolling_mode = handle_set_scrolling_mode,
	.detach_tiled = handle_detach_tiled,
	.set_drop_preview = handle_set_drop_preview,
	.get_tileable = handle_get_tileable,
};

static void
resource_destroy(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

static void
bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct singularity_tiling_manager *manager = data;
	struct wl_resource *resource = wl_resource_create(client,
		&zsingularity_tiling_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager,
		resource_destroy);
	wl_list_insert(&manager->resources, wl_resource_get_link(resource));
}

void
singularity_tiling_init(void)
{
	tiling_manager = calloc(1, sizeof(*tiling_manager));
	if (!tiling_manager) {
		return;
	}
	wl_list_init(&tiling_manager->resources);
	tiling_manager->global = wl_global_create(server.wl_display,
		&zsingularity_tiling_manager_v1_interface, 7,
		tiling_manager, bind_manager);
}

bool
singularity_tiling_scrolling_mode_enabled(void)
{
	return tiling_manager && tiling_manager->scrolling_mode_enabled;
}

bool
singularity_tiling_get_float_drop_box(struct view *view, struct wlr_box *box)
{
	if (!view || !box || !output_is_usable(view->output)) {
		return false;
	}
	struct wlr_box usable =
		output_usable_area_in_layout_coords(view->output);
	struct wlr_box output_box;
	wlr_output_layout_get_box(server.output_layout,
		view->output->wlr_output, &output_box);
	int available_height = output_box.y + output_box.height
		- (usable.y + usable.height);
	if (available_height < 24) {
		return false;
	}
	box->width = output_box.width / 3 < 240
		? output_box.width / 3 : 240;
	box->height = available_height < 64 ? available_height : 64;
	box->x = output_box.x + (output_box.width - box->width) / 2;
	box->y = output_box.y + output_box.height - box->height;
	return true;
}

bool
singularity_tiling_get_drop_preview_box(struct wlr_box *box)
{
	if (!tiling_manager || !tiling_manager->drop_preview_active || !box) {
		return false;
	}
	*box = tiling_manager->drop_preview_box;
	return true;
}

bool
singularity_tiling_float_candidate(struct view *view)
{
	struct wlr_box box;
	if (!singularity_tiling_get_float_drop_box(view, &box)) {
		return false;
	}
	double x = server.seat.cursor->x;
	double y = server.seat.cursor->y;
	return x >= box.x && x < box.x + box.width
		&& y >= box.y && y < box.y + box.height;
}

static struct wl_resource *
toplevel_resource_for_client(struct view *view, struct wl_client *client)
{
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

void
singularity_tiling_send_interaction(struct view *view,
	enum singularity_tiling_interaction_phase phase,
	enum singularity_tiling_interaction_kind kind,
	const struct wlr_box *geometry, uint32_t edges)
{
	if (!tiling_manager || !view || !view->singularity_scrolling_tiled) {
		return;
	}
	const struct wlr_box *box = geometry ? geometry : &view->pending;
	struct wl_resource *resource;
	wl_resource_for_each(resource, &tiling_manager->resources) {
		if (wl_resource_get_version(resource) < 5) {
			continue;
		}
		struct wl_resource *toplevel = toplevel_resource_for_client(view,
			wl_resource_get_client(resource));
		if (!toplevel) {
			continue;
		}
		zsingularity_tiling_manager_v1_send_interaction(resource, toplevel,
			phase, kind, box->x, box->y, box->width, box->height,
			(int32_t)round(server.seat.cursor->x),
			(int32_t)round(server.seat.cursor->y), edges,
			singularity_tiling_float_candidate(view) ? 1u : 0u);
	}
}
