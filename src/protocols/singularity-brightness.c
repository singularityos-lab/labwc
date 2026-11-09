// SPDX-License-Identifier: GPL-2.0-only
#include "protocols/singularity-brightness.h"
#include <inttypes.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/render/color.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_output.h>
#include <scenefx/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "labwc.h"
#include "output.h"
#include "singularity-brightness-unstable-v1-protocol.h"

#ifndef SINGULARITY_BRIGHTNESS_STORE
#define SINGULARITY_BRIGHTNESS_STORE "/var/lib/sinty-powerd/brightness"
#endif

static struct {
	struct wl_global *global;
	struct wl_list resources;
	struct wl_listener set_gamma;
} state;

uint64_t
singularity_brightness_edid_id(const char *connector, bool *found)
{
	*found = false;
	char pattern[512];
	snprintf(pattern, sizeof(pattern), "/sys/class/drm/card*-%s/edid", connector);
	glob_t g;
	if (glob(pattern, 0, NULL, &g) != 0) {
		return 0;
	}
	uint64_t hash = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < g.gl_pathc && !*found; i++) {
		FILE *f = fopen(g.gl_pathv[i], "rb");
		if (!f) {
			continue;
		}
		unsigned char edid[18];
		if (fread(edid, 1, sizeof(edid), f) == sizeof(edid)) {
			for (int b = 8; b < 18; b++) {
				hash ^= edid[b];
				hash *= 0x100000001b3ULL;
			}
			*found = true;
		}
		fclose(f);
	}
	globfree(&g);
	return *found ? hash : 0;
}

static uint32_t
stored_level(const char *connector)
{
	const char *path = getenv("SINGULARITY_BRIGHTNESS_STORE");
	FILE *f = fopen(path && *path ? path : SINGULARITY_BRIGHTNESS_STORE, "r");
	if (!f) {
		return 1000;
	}
	bool found;
	uint64_t id = singularity_brightness_edid_id(connector, &found);
	char want[17];
	snprintf(want, sizeof(want), "%016" PRIx64, id);
	uint32_t level = 1000;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char key[64], method[32];
		unsigned int value;
		if (sscanf(line, "%63s %u %31s", key, &value, method) != 3) {
			continue;
		}
		bool match = found ? !strcmp(key, want) : !strcmp(key, connector);
		if (match && !strcmp(method, "software")) {
			level = value;
		}
	}
	fclose(f);
	return level;
}

static float
clamp_level(uint32_t level)
{
	if (level < SINGULARITY_BRIGHTNESS_MIN) {
		level = SINGULARITY_BRIGHTNESS_MIN;
	}
	if (level > 1000) {
		level = 1000;
	}
	return level / 1000.0f;
}

void
singularity_brightness_apply(struct output *output)
{
	if (!output || !output->scene_output) {
		return;
	}
	struct wlr_scene_output *scene_output = output->scene_output;
	struct wlr_gamma_control_v1 *control = scene_output->WLR_PRIVATE.gamma_lut;
	float factor = output->software_brightness;
	struct wlr_color_transform *transform = NULL;
	if (factor >= 0.999f) {
		transform = wlr_gamma_control_v1_get_color_transform(control);
	} else {
		bool client = control && control->table && control->ramp_size > 1;
		size_t size = client ? control->ramp_size : 256;
		uint16_t *table = calloc(3 * size, sizeof(uint16_t));
		if (!table) {
			return;
		}
		for (size_t c = 0; c < 3; c++) {
			for (size_t i = 0; i < size; i++) {
				float base = client ? control->table[c * size + i]
					: (float)i * 65535.0f / (float)(size - 1);
				table[c * size + i] = (uint16_t)(base * factor + 0.5f);
			}
		}
		transform = wlr_color_transform_init_lut_3x1d(size,
			table, table + size, table + 2 * size);
		free(table);
	}
	wlr_color_transform_unref(scene_output->WLR_PRIVATE.gamma_lut_color_transform);
	scene_output->WLR_PRIVATE.gamma_lut_color_transform = transform;
	scene_output->WLR_PRIVATE.gamma_lut_changed = true;
	wlr_output_schedule_frame(output->wlr_output);
}

void
singularity_brightness_output_ready(struct output *output)
{
	output->software_brightness = clamp_level(stored_level(output->wlr_output->name));
	if (output->software_brightness < 0.999f) {
		wlr_log(WLR_INFO, "Software brightness %.2f on %s",
			output->software_brightness, output->wlr_output->name);
		singularity_brightness_apply(output);
	}
}

void
singularity_brightness_reset(void)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output->software_brightness < 0.999f) {
			output->software_brightness = 1.0f;
			singularity_brightness_apply(output);
		}
	}
	struct wl_resource *resource;
	wl_resource_for_each(resource, &state.resources) {
		zsingularity_brightness_manager_v1_send_reset(resource);
	}
}

static void
handle_set_gamma(struct wl_listener *listener, void *data)
{
	const struct wlr_gamma_control_manager_v1_set_gamma_event *event = data;
	struct output *output = event->output->data;
	if (output && output->software_brightness < 0.999f) {
		singularity_brightness_apply(output);
	}
}

static void
handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_set_brightness(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *output_resource, uint32_t level)
{
	struct wlr_output *wlr_output = wlr_output_from_resource(output_resource);
	struct output *output = wlr_output ? wlr_output->data : NULL;
	if (!output) {
		return;
	}
	float factor = clamp_level(level);
	if (factor == output->software_brightness) {
		return;
	}
	output->software_brightness = factor;
	singularity_brightness_apply(output);
}

static const struct zsingularity_brightness_manager_v1_interface manager_impl = {
	.destroy = handle_destroy,
	.set_brightness = handle_set_brightness,
};

static void
resource_destroy(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

static void
bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zsingularity_brightness_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, resource_destroy);
	wl_list_insert(&state.resources, wl_resource_get_link(resource));
}

void
singularity_brightness_init(void)
{
	wl_list_init(&state.resources);
	state.global = wl_global_create(server.wl_display,
		&zsingularity_brightness_manager_v1_interface, 1, NULL, bind_manager);
	state.set_gamma.notify = handle_set_gamma;
	wl_signal_add(&server.gamma_control_manager_v1->events.set_gamma, &state.set_gamma);
}
