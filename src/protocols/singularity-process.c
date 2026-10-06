// SPDX-License-Identifier: GPL-2.0-only
#include "protocols/singularity-process.h"
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include "labwc.h"
#include "singularity-process-unstable-v1-protocol.h"
#include "view.h"
#if HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif

static struct wl_global *process_global;

static bool
pid_is_xwayland(pid_t pid)
{
#if HAVE_XWAYLAND
	return server.xwayland && server.xwayland->server
		&& server.xwayland->server->pid == pid;
#else
	return false;
#endif
}

static void
handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_get_pid(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *toplevel_resource)
{
	struct wlr_foreign_toplevel_handle_v1 *toplevel =
		wl_resource_get_user_data(toplevel_resource);
	struct view *view = toplevel ? toplevel->data : NULL;
	pid_t pid = 0;
	uint32_t source = ZSINGULARITY_PROCESS_MANAGER_V1_SOURCE_NONE;

	if (view && view->impl && view->impl->get_pid) {
		pid = view->impl->get_pid(view);
#if HAVE_XWAYLAND
		source = view->type == LAB_XWAYLAND_VIEW
			? ZSINGULARITY_PROCESS_MANAGER_V1_SOURCE_X11
			: ZSINGULARITY_PROCESS_MANAGER_V1_SOURCE_WAYLAND;
#else
		source = ZSINGULARITY_PROCESS_MANAGER_V1_SOURCE_WAYLAND;
#endif
	}
	if (pid <= 1 || pid == getpid() || pid_is_xwayland(pid)) {
		pid = 0;
		source = ZSINGULARITY_PROCESS_MANAGER_V1_SOURCE_NONE;
	}
	zsingularity_process_manager_v1_send_pid(resource, toplevel_resource,
		pid, source);
}

static const struct zsingularity_process_manager_v1_interface manager_impl = {
	.destroy = handle_destroy,
	.get_pid = handle_get_pid,
};

static void
bind_manager(struct wl_client *client, void *data, uint32_t version,
	uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zsingularity_process_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

void
singularity_process_init(void)
{
	process_global = wl_global_create(server.wl_display,
		&zsingularity_process_manager_v1_interface, 1, NULL, bind_manager);
}
