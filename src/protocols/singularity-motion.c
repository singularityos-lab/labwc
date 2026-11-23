// SPDX-License-Identifier: GPL-2.0-only
#include "protocols/singularity-motion.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <scenefx/types/wlr_scene.h>
#include "common/mem.h"
#include "labwc.h"
#include "motion.h"
#include "output.h"
#include "singularity-motion-unstable-v1-protocol.h"
#include "view.h"
#include "view-animation.h"
#include "singularity-veil.h"

#define LAUNCH_EXPIRY_NS 8000000000LL
#define MAX_LAUNCHES 8

struct icon_entry {
	struct wl_list link;
	struct wl_resource *manager;
	struct wlr_surface *surface;
	char *app_id;
	struct wlr_box box;
	struct wl_listener surface_destroy;
};

struct launch_entry {
	char *app_id;
	int64_t time_ns;
};

static struct {
	struct wl_global *global;
	struct wl_list icons;
	struct launch_entry launches[MAX_LAUNCHES];
	struct wl_resource *veil_owner;
} motion_manager;

static void
normalize_app_id(const char *app_id, char *out, size_t size)
{
	snprintf(out, size, "%s", app_id ? app_id : "");
	size_t length = strlen(out);
	const char *suffix = ".desktop";
	size_t suffix_length = strlen(suffix);
	if (length > suffix_length
			&& !strcasecmp(out + length - suffix_length, suffix)) {
		out[length - suffix_length] = '\0';
	}
}

static const char *
last_component(const char *app_id)
{
	const char *dot = strrchr(app_id, '.');
	return dot && dot[1] ? dot + 1 : app_id;
}

static bool
app_ids_match(const char *a, const char *b)
{
	char left[256];
	char right[256];
	normalize_app_id(a, left, sizeof(left));
	normalize_app_id(b, right, sizeof(right));
	if (!*left || !*right) {
		return false;
	}
	if (!strcasecmp(left, right)) {
		return true;
	}
	return !strcasecmp(last_component(left), last_component(right));
}

static void
icon_entry_destroy(struct icon_entry *entry)
{
	wl_list_remove(&entry->link);
	wl_list_remove(&entry->surface_destroy.link);
	free(entry->app_id);
	free(entry);
}

static void
handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct icon_entry *entry =
		wl_container_of(listener, entry, surface_destroy);
	icon_entry_destroy(entry);
}

static void
remove_icons(struct wl_resource *manager, struct wlr_surface *surface,
		const char *app_id)
{
	struct icon_entry *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &motion_manager.icons, link) {
		if (manager && entry->manager != manager) {
			continue;
		}
		if (surface && entry->surface != surface) {
			continue;
		}
		if (app_id && strcmp(entry->app_id, app_id)) {
			continue;
		}
		icon_entry_destroy(entry);
	}
}

static void
handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_set_icon_rect(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *surface_resource, const char *app_id,
		int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (!surface || !app_id) {
		return;
	}
	remove_icons(resource, surface, app_id);
	if (width <= 0 || height <= 0) {
		return;
	}
	struct icon_entry *entry = znew(*entry);
	entry->manager = resource;
	entry->surface = surface;
	entry->app_id = xstrdup(app_id);
	entry->box = (struct wlr_box){ x, y, width, height };
	entry->surface_destroy.notify = handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &entry->surface_destroy);
	wl_list_insert(&motion_manager.icons, &entry->link);
}

static void
handle_clear_icon_rects(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *surface_resource)
{
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (surface) {
		remove_icons(resource, surface, NULL);
	}
}

static void
handle_launch(struct wl_client *client, struct wl_resource *resource,
		const char *app_id)
{
	if (!app_id || !*app_id) {
		return;
	}
	int slot = 0;
	for (int i = 0; i < MAX_LAUNCHES; i++) {
		struct launch_entry *launch = &motion_manager.launches[i];
		if (!launch->app_id || !strcmp(launch->app_id, app_id)) {
			slot = i;
			break;
		}
		if (launch->time_ns < motion_manager.launches[slot].time_ns) {
			slot = i;
		}
	}
	struct launch_entry *launch = &motion_manager.launches[slot];
	free(launch->app_id);
	launch->app_id = xstrdup(app_id);
	launch->time_ns = motion_now_ns();
}

struct card_handle {
	struct wl_resource *resource;
	struct view_card *card;
};

static struct card_handle *
card_handle_from_resource(struct wl_resource *resource)
{
	return wl_resource_get_user_data(resource);
}

static void
handle_card_done(void *data)
{
	struct card_handle *handle = data;
	handle->card = NULL;
	zsingularity_motion_card_v1_send_done(handle->resource);
}

static void
handle_card_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_card_set_rect(struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y, int32_t width, int32_t height, int32_t radius)
{
	struct card_handle *handle = card_handle_from_resource(resource);
	if (!handle || !handle->card) {
		return;
	}
	view_card_set_rect(handle->card,
		(struct wlr_box){ x, y, width, height }, radius);
}

static void
handle_card_release(struct wl_client *client, struct wl_resource *resource,
		uint32_t action, int32_t x, int32_t y, int32_t width, int32_t height,
		int32_t radius, int32_t velocity_x, int32_t velocity_y)
{
	struct card_handle *handle = card_handle_from_resource(resource);
	if (!handle || !handle->card) {
		return;
	}
	view_card_release(handle->card,
		action == ZSINGULARITY_MOTION_CARD_V1_RELEASE_ACTION_MINIMIZE
			? VIEW_CARD_MINIMIZE : VIEW_CARD_SHOW,
		(struct wlr_box){ x, y, width, height }, radius,
		velocity_x, velocity_y);
}

static const struct zsingularity_motion_card_v1_interface card_impl = {
	.destroy = handle_card_destroy,
	.set_rect = handle_card_set_rect,
	.release = handle_card_release,
};

static void
card_resource_destroy(struct wl_resource *resource)
{
	struct card_handle *handle = card_handle_from_resource(resource);
	if (!handle) {
		return;
	}
	if (handle->card) {
		view_card_destroy(handle->card);
	}
	free(handle);
}

static void
handle_get_card(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *toplevel_resource)
{
	struct wl_resource *card_resource = wl_resource_create(client,
		&zsingularity_motion_card_v1_interface,
		wl_resource_get_version(resource), id);
	if (!card_resource) {
		wl_client_post_no_memory(client);
		return;
	}
	struct card_handle *handle = znew(*handle);
	handle->resource = card_resource;
	wl_resource_set_implementation(card_resource, &card_impl, handle,
		card_resource_destroy);
	struct wlr_foreign_toplevel_handle_v1 *toplevel =
		wl_resource_get_user_data(toplevel_resource);
	struct view *view = toplevel ? toplevel->data : NULL;
	if (view) {
		handle->card = view_card_create(view, handle_card_done, handle);
	}
	if (!handle->card) {
		zsingularity_motion_card_v1_send_done(card_resource);
	}
}

static void
handle_fade_windows(struct wl_client *client, struct wl_resource *resource,
		uint32_t faded, uint32_t duration)
{
	(void)client;
	motion_manager.veil_owner = faded ? resource : NULL;
	singularity_veil_fade(faded != 0, (int)(duration > 5000 ? 5000 : duration));
}

static const struct zsingularity_motion_manager_v1_interface manager_impl = {
	.destroy = handle_destroy,
	.set_icon_rect = handle_set_icon_rect,
	.clear_icon_rects = handle_clear_icon_rects,
	.launch = handle_launch,
	.get_card = handle_get_card,
	.fade_windows = handle_fade_windows,
};

static void
resource_destroy(struct wl_resource *resource)
{
	remove_icons(resource, NULL, NULL);
	if (motion_manager.veil_owner == resource) {
		motion_manager.veil_owner = NULL;
		singularity_veil_finish();
	}
}

static void
bind_manager(struct wl_client *client, void *data, uint32_t version,
		uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zsingularity_motion_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL,
		resource_destroy);
}

void
singularity_motion_init(void)
{
	wl_list_init(&motion_manager.icons);
	motion_manager.global = wl_global_create(server.wl_display,
		&zsingularity_motion_manager_v1_interface, 3, NULL, bind_manager);
}

static bool
entry_layout_box(struct icon_entry *entry, struct wlr_box *box)
{
	struct wlr_surface *surface = entry->surface;
	if (!surface->mapped
			|| !wlr_layer_surface_v1_try_from_wlr_surface(surface)) {
		return false;
	}
	struct wlr_scene_tree *tree = surface->data;
	if (!tree) {
		return false;
	}
	int lx, ly;
	if (!wlr_scene_node_coords(&tree->node, &lx, &ly)) {
		return false;
	}
	*box = entry->box;
	box->x += lx;
	box->y += ly;
	return true;
}

bool
singularity_motion_icon_box(const char *app_id, struct output *output,
		struct wlr_box *box)
{
	if (!app_id || !motion_manager.global) {
		return false;
	}
	bool found = false;
	struct icon_entry *entry;
	wl_list_for_each(entry, &motion_manager.icons, link) {
		if (!app_ids_match(entry->app_id, app_id)) {
			continue;
		}
		struct wlr_box candidate;
		if (!entry_layout_box(entry, &candidate)) {
			continue;
		}
		bool on_output = output && output->wlr_output
			&& wlr_output_layout_contains_point(server.output_layout,
				output->wlr_output,
				candidate.x + candidate.width / 2,
				candidate.y + candidate.height / 2);
		if (!found || on_output) {
			*box = candidate;
			found = true;
		}
		if (on_output) {
			break;
		}
	}
	return found;
}

bool
singularity_motion_take_launch(const char *app_id)
{
	if (!app_id) {
		return false;
	}
	int64_t now = motion_now_ns();
	for (int i = 0; i < MAX_LAUNCHES; i++) {
		struct launch_entry *launch = &motion_manager.launches[i];
		if (!launch->app_id) {
			continue;
		}
		if (now - launch->time_ns > LAUNCH_EXPIRY_NS) {
			free(launch->app_id);
			launch->app_id = NULL;
			continue;
		}
		if (app_ids_match(launch->app_id, app_id)) {
			free(launch->app_id);
			launch->app_id = NULL;
			return true;
		}
	}
	return false;
}
