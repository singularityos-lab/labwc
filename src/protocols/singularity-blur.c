// SPDX-License-Identifier: GPL-2.0-only

#include <limits.h>
#include <pixman.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>
#include "background-effect.h"
#include "labwc.h"
#include "singularity-blur-unstable-v1-protocol.h"

#define SINGULARITY_BLUR_VERSION 5
#define GLASS_STRENGTH_SCALE 0.6f
#define LIQUID_STRENGTH_SCALE 0.55f
#define LIQUID_BEZEL 18.0f

struct blur_state {
	uint32_t mode;
	bool whole_surface;
	pixman_region32_t region;
	uint32_t strength;
	uint32_t corner_radius;
};

struct singularity_blur_entry {
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct blur_state pending;
	struct wl_listener surface_destroy;
};

static struct wl_global *blur_global;

static void
entry_set_effect(struct singularity_blur_entry *entry,
		const pixman_region32_t *region, float strength_scale,
		float liquid, int corner_radius)
{
	if (wl_resource_get_version(entry->resource) >= 4) {
		background_effect_set_internal_pending(entry->surface, region,
			strength_scale, liquid, corner_radius);
	} else {
		background_effect_set_internal(entry->surface, region,
			strength_scale, liquid, corner_radius);
	}
}

static void
entry_clear_effect(struct singularity_blur_entry *entry)
{
	if (entry->surface) {
		entry_set_effect(entry, NULL, 1.0f, 0.0f, 0);
	}
}

static void
entry_detach(struct singularity_blur_entry *entry)
{
	if (!entry->surface) {
		return;
	}
	entry_clear_effect(entry);
	wl_list_remove(&entry->surface_destroy.link);
	entry->surface = NULL;
}

static void
handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct singularity_blur_entry *entry =
		wl_container_of(listener, entry, surface_destroy);
	wl_list_remove(&entry->surface_destroy.link);
	entry->surface = NULL;
}

static void
blur_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
blur_handle_set_radius(struct wl_client *client, struct wl_resource *resource,
		uint32_t radius)
{
}

static void
blur_handle_set_noise(struct wl_client *client, struct wl_resource *resource,
		uint32_t noise)
{
}

static void
blur_handle_set_mode(struct wl_client *client, struct wl_resource *resource,
		uint32_t mode)
{
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (entry) {
		entry->pending.mode = mode;
	}
}

static void
blur_handle_set_region(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *region_resource)
{
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (!entry) {
		return;
	}
	if (region_resource) {
		pixman_region32_copy(&entry->pending.region,
			wlr_region_from_resource(region_resource));
		entry->pending.whole_surface = false;
	} else {
		pixman_region32_clear(&entry->pending.region);
		entry->pending.whole_surface = true;
	}
}

static void
blur_handle_set_strength(struct wl_client *client, struct wl_resource *resource,
		uint32_t strength)
{
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (entry) {
		entry->pending.strength = strength > 100 ? 100 : strength;
	}
}

static void
blur_handle_set_corner_radius(struct wl_client *client,
		struct wl_resource *resource, uint32_t radius)
{
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (entry) {
		entry->pending.corner_radius = radius > 512 ? 512 : radius;
	}
}

static void
blur_handle_commit(struct wl_client *client, struct wl_resource *resource)
{
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (!entry || !entry->surface) {
		return;
	}
	struct blur_state *state = &entry->pending;
	if (state->mode == ZSINGULARITY_BLUR_V1_MODE_DISABLED
			|| state->strength == 0) {
		entry_clear_effect(entry);
		return;
	}
	float scale = state->strength / 100.0f;
	float liquid = 0.0f;
	if (state->mode == ZSINGULARITY_BLUR_V1_MODE_GLASS) {
		scale *= GLASS_STRENGTH_SCALE;
	} else if (state->mode == ZSINGULARITY_BLUR_V1_MODE_LIQUID) {
		scale *= LIQUID_STRENGTH_SCALE;
		liquid = LIQUID_BEZEL;
	}
	pixman_region32_t region;
	if (state->whole_surface) {
		pixman_region32_init_rect(&region, 0, 0, INT_MAX / 2, INT_MAX / 2);
	} else {
		pixman_region32_init(&region);
		pixman_region32_copy(&region, &state->region);
	}
	entry_set_effect(entry, &region, scale, liquid,
		state->corner_radius);
	pixman_region32_fini(&region);
}

static const struct zsingularity_blur_v1_interface blur_impl = {
	.destroy = blur_handle_destroy,
	.set_radius = blur_handle_set_radius,
	.set_noise = blur_handle_set_noise,
	.commit = blur_handle_commit,
	.set_mode = blur_handle_set_mode,
	.set_region = blur_handle_set_region,
	.set_strength = blur_handle_set_strength,
	.set_corner_radius = blur_handle_set_corner_radius,
};

static void
blur_resource_destroy(struct wl_resource *resource)
{
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (!entry) {
		return;
	}
	entry_detach(entry);
	pixman_region32_fini(&entry->pending.region);
	free(entry);
}

static void
manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
manager_handle_get_blur(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *surface_resource)
{
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (!surface) {
		wl_resource_post_error(resource,
			ZSINGULARITY_BLUR_MANAGER_V1_ERROR_INVALID_SURFACE,
			"invalid surface");
		return;
	}
	struct singularity_blur_entry *entry = calloc(1, sizeof(*entry));
	if (!entry) {
		wl_client_post_no_memory(client);
		return;
	}
	entry->resource = wl_resource_create(client,
		&zsingularity_blur_v1_interface,
		wl_resource_get_version(resource), id);
	if (!entry->resource) {
		free(entry);
		wl_client_post_no_memory(client);
		return;
	}
	entry->pending.mode = ZSINGULARITY_BLUR_V1_MODE_BLUR;
	entry->pending.whole_surface = true;
	entry->pending.strength = 100;
	pixman_region32_init(&entry->pending.region);
	wl_resource_set_implementation(entry->resource, &blur_impl, entry,
		blur_resource_destroy);
	entry->surface = surface;
	entry->surface_destroy.notify = handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &entry->surface_destroy);
}

static const struct zsingularity_blur_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_blur = manager_handle_get_blur,
};

static void
bind_manager(struct wl_client *client, void *data, uint32_t version,
		uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zsingularity_blur_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

void
singularity_blur_init(void)
{
	blur_global = wl_global_create(server.wl_display,
		&zsingularity_blur_manager_v1_interface, SINGULARITY_BLUR_VERSION,
		NULL, bind_manager);
	if (!blur_global) {
		wlr_log(WLR_ERROR, "singularity-blur: failed to create global");
	}
}
