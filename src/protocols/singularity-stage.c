// SPDX-License-Identifier: GPL-2.0-only
#include "protocols/singularity-stage.h"
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include "labwc.h"
#include "singularity-stage-unstable-v1-protocol.h"
#include "view.h"

static struct wl_global *stage_global;

static struct view *
view_from_toplevel_resource(struct wl_resource *resource)
{
	struct wlr_foreign_toplevel_handle_v1 *toplevel =
		wl_resource_get_user_data(resource);
	return toplevel ? toplevel->data : NULL;
}

static void
handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_set_hidden(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *toplevel_resource, uint32_t hidden,
	int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct view *view = view_from_toplevel_resource(toplevel_resource);
	if (!view || view->minimized == (hidden != 0)) {
		return;
	}
	if (width > 0 && height > 0) {
		view->stage_target = (struct wlr_box){
			.x = x,
			.y = y,
			.width = width,
			.height = height,
		};
		view->stage_target_valid = true;
	}
	view_minimize(view, hidden != 0);
	view->stage_target_valid = false;
	view->stage_hidden = hidden != 0 && view->minimized;
}

static void
handle_set_group(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *toplevel_resource, uint32_t group)
{
	struct view *view = view_from_toplevel_resource(toplevel_resource);
	if (view) {
		view->stage_group = group;
	}
}

static void
handle_get_group(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *toplevel_resource)
{
	struct view *view = view_from_toplevel_resource(toplevel_resource);
	uint32_t group = view ? view->stage_group : 0;
	uint32_t hidden = view && view->stage_hidden && view->minimized ? 1 : 0;
	zsingularity_stage_manager_v1_send_group(resource, toplevel_resource,
		group, hidden);
}

static const struct zsingularity_stage_manager_v1_interface manager_impl = {
	.destroy = handle_destroy,
	.set_hidden = handle_set_hidden,
	.set_group = handle_set_group,
	.get_group = handle_get_group,
};

static void
bind_manager(struct wl_client *client, void *data, uint32_t version,
	uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zsingularity_stage_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

void
singularity_stage_init(void)
{
	stage_global = wl_global_create(server.wl_display,
		&zsingularity_stage_manager_v1_interface, 2, NULL, bind_manager);
}
