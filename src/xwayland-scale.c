// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <math.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#include "common/buf.h"
#include "common/mem.h"
#include "config/rcxml.h"
#include "labwc.h"
#include "output.h"
#include "view.h"
#include "xdg-output-unstable-v1-protocol.h"
#include "xwayland.h"

struct scaled_surface {
	struct wlr_scene_buffer *buffer;
	struct wlr_surface *surface;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_list link;
};

struct xwayland_xdg_output {
	struct wl_resource *resource;
	struct wl_resource *output_resource;
	struct wl_listener output_resource_destroy;
	struct wl_list link;
	struct wlr_box sent;
};

static double scale = 1.0;
static wlr_scene_buffer_point_accepts_input_func_t surface_point_accepts_input;
static bool dpi_overridden;
static struct wl_global *xdg_output_global;
static struct wl_list xdg_outputs = { &xdg_outputs, &xdg_outputs };
static struct wl_list scaled_surfaces = { &scaled_surfaces, &scaled_surfaces };

double
xwayland_scale(void)
{
	return scale;
}

int
xwayland_to_x(int value)
{
	return (int)lround(value * scale);
}

int
xwayland_from_x(int value)
{
	return (int)lround(value / scale);
}

double
xwayland_surface_scale(struct wlr_surface *surface)
{
	if (scale == 1.0 || !surface
			|| !wlr_xwayland_surface_try_from_wlr_surface(surface)) {
		return 1.0;
	}
	return scale;
}

static bool
point_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	*sx *= scale;
	*sy *= scale;
	return surface_point_accepts_input(buffer, sx, sy);
}

static void
apply_dest_size(struct scaled_surface *scaled)
{
	if (scale == 1.0) {
		return;
	}
	struct wlr_surface_state *state = &scaled->surface->current;
	wlr_scene_buffer_set_dest_size(scaled->buffer,
		xwayland_from_x(state->width), xwayland_from_x(state->height));
}

static void
handle_scaled_commit(struct wl_listener *listener, void *data)
{
	struct scaled_surface *scaled =
		wl_container_of(listener, scaled, commit);
	apply_dest_size(scaled);
}

static void
handle_scaled_destroy(struct wl_listener *listener, void *data)
{
	struct scaled_surface *scaled =
		wl_container_of(listener, scaled, destroy);
	wl_list_remove(&scaled->commit.link);
	wl_list_remove(&scaled->destroy.link);
	wl_list_remove(&scaled->link);
	free(scaled);
}

void
xwayland_scale_attach(struct wlr_scene_buffer *buffer)
{
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);
	if (!scene_surface) {
		return;
	}
	struct scaled_surface *scaled = znew(*scaled);
	scaled->buffer = buffer;
	scaled->surface = scene_surface->surface;
	if (buffer->point_accepts_input != point_accepts_input) {
		surface_point_accepts_input = buffer->point_accepts_input;
		buffer->point_accepts_input = point_accepts_input;
	}

	scaled->commit.notify = handle_scaled_commit;
	wl_signal_add(&scaled->surface->events.commit, &scaled->commit);
	scaled->destroy.notify = handle_scaled_destroy;
	wl_signal_add(&buffer->node.events.destroy, &scaled->destroy);
	wl_list_insert(&scaled_surfaces, &scaled->link);
	apply_dest_size(scaled);
}

static void
attach_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy,
		void *data)
{
	xwayland_scale_attach(buffer);
}

void
xwayland_scale_attach_tree(struct wlr_scene_tree *tree)
{
	wlr_scene_node_for_each_buffer(&tree->node, attach_buffer_iterator, NULL);
}

static void
send_xdg_output(struct xwayland_xdg_output *xdg_output, bool initial)
{
	struct wlr_output *wlr_output = xdg_output->output_resource
		? wlr_output_from_resource(xdg_output->output_resource) : NULL;
	if (!wlr_output) {
		return;
	}

	struct wlr_box box;
	wlr_output_layout_get_box(server.output_layout, wlr_output, &box);
	struct wlr_box scaled = {
		.x = xwayland_to_x(box.x),
		.y = xwayland_to_x(box.y),
		.width = xwayland_to_x(box.width),
		.height = xwayland_to_x(box.height),
	};
	if (!initial && wlr_box_equal(&scaled, &xdg_output->sent)) {
		return;
	}
	xdg_output->sent = scaled;

	struct wl_resource *resource = xdg_output->resource;
	uint32_t version = wl_resource_get_version(resource);
	zxdg_output_v1_send_logical_position(resource, scaled.x, scaled.y);
	zxdg_output_v1_send_logical_size(resource, scaled.width, scaled.height);
	if (initial && version >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) {
		zxdg_output_v1_send_name(resource, wlr_output->name);
		if (wlr_output->description) {
			zxdg_output_v1_send_description(resource,
				wlr_output->description);
		}
	}
	if (version >= 3) {
		if (wl_resource_get_version(xdg_output->output_resource)
				>= WL_OUTPUT_DONE_SINCE_VERSION) {
			wl_output_send_done(xdg_output->output_resource);
		}
	} else {
		zxdg_output_v1_send_done(resource);
	}
}

static void
handle_output_resource_destroy(struct wl_listener *listener, void *data)
{
	struct xwayland_xdg_output *xdg_output =
		wl_container_of(listener, xdg_output, output_resource_destroy);
	wl_list_remove(&xdg_output->output_resource_destroy.link);
	wl_list_init(&xdg_output->output_resource_destroy.link);
	xdg_output->output_resource = NULL;
}

static void
handle_xdg_output_resource_destroy(struct wl_resource *resource)
{
	struct xwayland_xdg_output *xdg_output =
		wl_resource_get_user_data(resource);
	wl_list_remove(&xdg_output->output_resource_destroy.link);
	wl_list_remove(&xdg_output->link);
	free(xdg_output);
}

static void
handle_resource_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface xdg_output_impl = {
	.destroy = handle_resource_destroy,
};

static void
handle_get_xdg_output(struct wl_client *client, struct wl_resource *manager,
		uint32_t id, struct wl_resource *output_resource)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zxdg_output_v1_interface, wl_resource_get_version(manager), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	struct xwayland_xdg_output *xdg_output = znew(*xdg_output);
	xdg_output->resource = resource;
	xdg_output->output_resource = output_resource;
	xdg_output->output_resource_destroy.notify =
		handle_output_resource_destroy;
	wl_resource_add_destroy_listener(output_resource,
		&xdg_output->output_resource_destroy);
	wl_list_insert(&xdg_outputs, &xdg_output->link);
	wl_resource_set_implementation(resource, &xdg_output_impl, xdg_output,
		handle_xdg_output_resource_destroy);

	send_xdg_output(xdg_output, true);
}

static const struct zxdg_output_manager_v1_interface xdg_output_manager_impl = {
	.destroy = handle_resource_destroy,
	.get_xdg_output = handle_get_xdg_output,
};

static void
bind_xdg_output_manager(struct wl_client *client, void *data,
		uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zxdg_output_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &xdg_output_manager_impl,
		NULL, NULL);
}

bool
xwayland_scale_filter_global(const struct wl_client *client,
		const struct wl_global *global)
{
	bool is_xwayland = server.xwayland && server.xwayland->server
		&& client == server.xwayland->server->client;
	if (xdg_output_global && global == xdg_output_global) {
		return is_xwayland;
	}
	if (is_xwayland && server.xdg_output_manager
			&& global == server.xdg_output_manager->global) {
		return false;
	}
	return true;
}

static void
update_xft_dpi(void)
{
	if (!server.xwayland || !server.xwayland->xwm) {
		return;
	}
	if (scale == 1.0 && !dpi_overridden) {
		return;
	}

	xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(server.xwayland);
	xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(conn)).data->root;
	xcb_get_property_reply_t *reply = xcb_get_property_reply(conn,
		xcb_get_property(conn, 0, root, XCB_ATOM_RESOURCE_MANAGER,
			XCB_ATOM_STRING, 0, UINT32_MAX / 4), NULL);

	struct buf resources = BUF_INIT;
	if (reply) {
		int len = xcb_get_property_value_length(reply);
		char *text = xzalloc(len + 1);
		memcpy(text, xcb_get_property_value(reply), len);
		char *saveptr = NULL;
		for (char *line = strtok_r(text, "\n", &saveptr); line;
				line = strtok_r(NULL, "\n", &saveptr)) {
			if (!strncmp(line, "Xft.dpi:", strlen("Xft.dpi:"))) {
				continue;
			}
			buf_add(&resources, line);
			buf_add_char(&resources, '\n');
		}
		free(text);
		free(reply);
	}
	if (scale != 1.0) {
		buf_add_fmt(&resources, "Xft.dpi:\t%ld\n", lround(96 * scale));
	}
	dpi_overridden = scale != 1.0;

	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root,
		XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 8, resources.len,
		resources.data);
	xcb_flush(conn);
	buf_reset(&resources);
}

static double
compute_scale(void)
{
	if (!rc.xwayland_native_scaling) {
		return 1.0;
	}
	double max = 1.0;
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output_is_usable(output) && output->wlr_output->scale > max) {
			max = output->wlr_output->scale;
		}
	}
	return max;
}

void
xwayland_update_scale(void)
{
	double new_scale = compute_scale();
	bool changed = new_scale != scale;
	scale = new_scale;

	struct xwayland_xdg_output *xdg_output;
	wl_list_for_each(xdg_output, &xdg_outputs, link) {
		send_xdg_output(xdg_output, false);
	}
	if (!changed) {
		return;
	}

	struct scaled_surface *scaled;
	wl_list_for_each(scaled, &scaled_surfaces, link) {
		apply_dest_size(scaled);
	}

	struct view *view;
	wl_list_for_each(view, &server.views, link) {
		if (view->type == LAB_XWAYLAND_VIEW && !wlr_box_empty(&view->pending)) {
			view->impl->configure(view, view->pending);
		}
	}

	struct xwayland_unmanaged *unmanaged;
	wl_list_for_each(unmanaged, &server.unmanaged_surfaces, link) {
		if (unmanaged->node) {
			struct wlr_xwayland_surface *xsurface =
				unmanaged->xwayland_surface;
			wlr_scene_node_set_position(unmanaged->node,
				xwayland_from_x(xsurface->x),
				xwayland_from_x(xsurface->y));
		}
	}

	update_xft_dpi();
}

void
xwayland_scale_xwm_ready(void)
{
	update_xft_dpi();
}

void
xwayland_scale_init(void)
{
	xdg_output_global = wl_global_create(server.wl_display,
		&zxdg_output_manager_v1_interface, 3, NULL,
		bind_xdg_output_manager);
	if (!xdg_output_global) {
		wlr_log(WLR_ERROR, "unable to create xwayland xdg_output manager");
	}
	scale = compute_scale();
}

void
xwayland_scale_finish(void)
{
	if (xdg_output_global) {
		wl_global_destroy(xdg_output_global);
		xdg_output_global = NULL;
	}
}
