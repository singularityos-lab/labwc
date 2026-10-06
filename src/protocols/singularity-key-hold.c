// SPDX-License-Identifier: GPL-2.0-only
#include "protocols/singularity-key-hold.h"
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>
#include "labwc.h"
#include "session-lock.h"
#include "singularity-key-hold-unstable-v1-protocol.h"

struct key_hold_client {
	struct wl_resource *resource;
	uint32_t delay_ms;
	struct wl_list link;
};

static struct {
	struct wl_global *global;
	struct wl_list clients;
	struct wl_event_source *timer;
	uint32_t keycode;
	bool pending;
	bool active;
	bool swallow;
} hold;

static uint32_t
hold_delay(void)
{
	uint32_t delay = 0;
	struct key_hold_client *client;
	wl_list_for_each(client, &hold.clients, link) {
		if (client->delay_ms > delay) {
			delay = client->delay_ms;
		}
	}
	return delay;
}

static void
disarm(void)
{
	hold.pending = false;
	if (hold.timer) {
		wl_event_source_timer_update(hold.timer, 0);
	}
}

static void
send_end(uint32_t reason)
{
	struct key_hold_client *client;
	wl_list_for_each(client, &hold.clients, link) {
		if (client->delay_ms > 0) {
			zsingularity_key_hold_manager_v1_send_hold_end(
				client->resource, reason);
		}
	}
}

static int
handle_timer(void *data)
{
	if (!hold.pending) {
		return 0;
	}
	hold.pending = false;
	if (server.session_lock_manager->locked
			|| server.input_mode != LAB_INPUT_STATE_PASSTHROUGH) {
		return 0;
	}
	hold.active = true;
	hold.swallow = true;
	wlr_log(WLR_DEBUG, "key hold started");
	struct key_hold_client *client;
	wl_list_for_each(client, &hold.clients, link) {
		if (client->delay_ms > 0) {
			zsingularity_key_hold_manager_v1_send_hold_start(
				client->resource);
		}
	}
	return 0;
}

static void
handle_destroy(struct wl_client *wl_client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_set_delay(struct wl_client *wl_client, struct wl_resource *resource,
		uint32_t delay_ms)
{
	struct key_hold_client *client = wl_resource_get_user_data(resource);
	if (client) {
		client->delay_ms = delay_ms > 10000 ? 10000 : delay_ms;
	}
}

static void
handle_finish(struct wl_client *wl_client, struct wl_resource *resource)
{
	disarm();
	hold.active = false;
}

static const struct zsingularity_key_hold_manager_v1_interface manager_impl = {
	.destroy = handle_destroy,
	.set_delay = handle_set_delay,
	.finish = handle_finish,
};

static void
resource_destroy(struct wl_resource *resource)
{
	struct key_hold_client *client = wl_resource_get_user_data(resource);
	if (!client) {
		return;
	}
	wl_list_remove(&client->link);
	free(client);
	if (hold_delay() == 0) {
		disarm();
		hold.active = false;
	}
}

static void
bind_manager(struct wl_client *wl_client, void *data, uint32_t version,
		uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(wl_client,
		&zsingularity_key_hold_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	struct key_hold_client *client = calloc(1, sizeof(*client));
	if (!client) {
		wl_resource_destroy(resource);
		wl_client_post_no_memory(wl_client);
		return;
	}
	client->resource = resource;
	wl_list_insert(&hold.clients, &client->link);
	wl_resource_set_implementation(resource, &manager_impl, client,
		resource_destroy);
}

void
singularity_key_hold_init(void)
{
	wl_list_init(&hold.clients);
	hold.timer = wl_event_loop_add_timer(server.wl_event_loop,
		handle_timer, NULL);
	hold.global = wl_global_create(server.wl_display,
		&zsingularity_key_hold_manager_v1_interface, 1, NULL,
		bind_manager);
}

bool
singularity_key_hold_active(void)
{
	return hold.active;
}

void
singularity_key_hold_press(uint32_t keycode, bool super_alone)
{
	if (hold.active) {
		return;
	}
	disarm();
	hold.swallow = false;
	if (!super_alone || server.session_lock_manager->locked) {
		return;
	}
	uint32_t delay = hold_delay();
	if (delay == 0 || !hold.timer) {
		return;
	}
	hold.keycode = keycode;
	hold.pending = true;
	wl_event_source_timer_update(hold.timer, (int)delay);
}

bool
singularity_key_hold_release(uint32_t keycode)
{
	if (keycode != hold.keycode) {
		return false;
	}
	disarm();
	bool swallow = hold.swallow;
	hold.swallow = false;
	if (hold.active) {
		hold.active = false;
		send_end(ZSINGULARITY_KEY_HOLD_MANAGER_V1_END_REASON_RELEASED);
	}
	return swallow;
}

void
singularity_key_hold_pointer_button(void)
{
	disarm();
}

void
singularity_key_hold_cancel(void)
{
	disarm();
	if (hold.active) {
		hold.active = false;
		send_end(ZSINGULARITY_KEY_HOLD_MANAGER_V1_END_REASON_CANCELLED);
	}
}
