// SPDX-License-Identifier: GPL-2.0-only
#include "protocols/singularity-pacing.h"
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include "labwc.h"
#include "output.h"
#include "singularity-pacing-unstable-v1-protocol.h"

static struct {
	struct wl_global *global;
} state;

static int64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int
handle_timer(void *data)
{
	struct output *output = data;
	wlr_output_schedule_frame(output->wlr_output);
	return 0;
}

static bool
limit_applies(struct output *output)
{
	if (!output->pacing_rate) {
		return false;
	}
	int32_t refresh = output->wlr_output->refresh;
	return refresh <= 0 || (int64_t)output->pacing_rate * 1000 < refresh;
}

bool
singularity_pacing_hold(struct output *output)
{
	if (!limit_applies(output)) {
		return false;
	}
	int64_t now = now_ns();
	int64_t interval = 1000000000LL / output->pacing_rate;
	int32_t refresh = output->wlr_output->refresh;
	int64_t slack = refresh > 0 ? 500000000000LL / refresh : 0;
	int64_t elapsed = now - output->pacing_last;
	if (output->pacing_last && elapsed + slack < interval) {
		if (!output->pacing_timer) {
			output->pacing_timer = wl_event_loop_add_timer(
				server.wl_event_loop, handle_timer, output);
		}
		if (output->pacing_timer) {
			int64_t wait = interval - elapsed - slack;
			int ms = (int)((wait + 999999) / 1000000);
			wl_event_source_timer_update(output->pacing_timer, ms > 0 ? ms : 1);
		}
		return true;
	}
	output->pacing_last = now;
	return false;
}

static void
set_rate(struct output *output, uint32_t rate, struct wl_resource *owner)
{
	if (output->pacing_rate == rate && output->pacing_owner == owner) {
		return;
	}
	output->pacing_rate = rate;
	output->pacing_owner = rate ? owner : NULL;
	output->pacing_last = 0;
	if (output->pacing_timer) {
		wl_event_source_timer_update(output->pacing_timer, 0);
	}
	wlr_log(WLR_INFO, "Frame rate limit on %s: %u", output->wlr_output->name, rate);
	wlr_output_schedule_frame(output->wlr_output);
}

void
singularity_pacing_output_destroy(struct output *output)
{
	if (output->pacing_timer) {
		wl_event_source_remove(output->pacing_timer);
		output->pacing_timer = NULL;
	}
}

static void
handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_set_max_rate(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *output_resource, uint32_t rate)
{
	struct wlr_output *wlr_output = wlr_output_from_resource(output_resource);
	struct output *output = wlr_output ? wlr_output->data : NULL;
	if (!output) {
		return;
	}
	set_rate(output, rate, resource);
}

static const struct zsingularity_pacing_manager_v1_interface manager_impl = {
	.destroy = handle_destroy,
	.set_max_rate = handle_set_max_rate,
};

static void
resource_destroy(struct wl_resource *resource)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output->pacing_owner == resource) {
			set_rate(output, 0, NULL);
		}
	}
}

static void
bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zsingularity_pacing_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, resource_destroy);
}

void
singularity_pacing_init(void)
{
	state.global = wl_global_create(server.wl_display,
		&zsingularity_pacing_manager_v1_interface, 1, NULL, bind_manager);
}
