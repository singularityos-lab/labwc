// SPDX-License-Identifier: GPL-2.0-only
#include "protocols/singularity-gesture.h"
#include <stdlib.h>
#include <wayland-server-core.h>
#include "labwc.h"
#include "singularity-gesture-unstable-v1-protocol.h"

struct singularity_gesture_manager {
	struct wl_global *global;
	struct wl_list resources;
};

static struct singularity_gesture_manager *gesture_manager;

static void
handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zsingularity_gesture_manager_v1_interface manager_impl = {
	.destroy = handle_destroy,
};

static void
resource_destroy(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

static void
bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct singularity_gesture_manager *manager = data;
	struct wl_resource *resource = wl_resource_create(client,
		&zsingularity_gesture_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager,
		resource_destroy);
	wl_list_insert(&manager->resources, wl_resource_get_link(resource));
}

void
singularity_gesture_init(void)
{
	gesture_manager = calloc(1, sizeof(*gesture_manager));
	if (!gesture_manager) {
		return;
	}
	wl_list_init(&gesture_manager->resources);
	gesture_manager->global = wl_global_create(server.wl_display,
		&zsingularity_gesture_manager_v1_interface, 1,
		gesture_manager, bind_manager);
}

bool
singularity_gesture_has_clients(void)
{
	return gesture_manager && !wl_list_empty(&gesture_manager->resources);
}

void
singularity_gesture_send_begin(uint32_t fingers, enum direction direction)
{
	if (!gesture_manager) {
		return;
	}
	struct wl_resource *resource;
	wl_resource_for_each(resource, &gesture_manager->resources) {
		zsingularity_gesture_manager_v1_send_begin(resource, fingers, direction);
	}
}

void
singularity_gesture_send_update(double dx, double dy)
{
	if (!gesture_manager) {
		return;
	}
	struct wl_resource *resource;
	wl_resource_for_each(resource, &gesture_manager->resources) {
		zsingularity_gesture_manager_v1_send_update(resource,
			wl_fixed_from_double(dx), wl_fixed_from_double(dy));
	}
}

void
singularity_gesture_send_end(bool cancelled, bool committed)
{
	if (!gesture_manager) {
		return;
	}
	struct wl_resource *resource;
	wl_resource_for_each(resource, &gesture_manager->resources) {
		zsingularity_gesture_manager_v1_send_end(resource,
			cancelled ? 1u : 0u, committed ? 1u : 0u);
	}
}
