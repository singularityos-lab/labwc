// SPDX-License-Identifier: GPL-2.0-only

#define _POSIX_C_SOURCE 200809L
#include "service-notify.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>

static struct {
	int fd;
	struct sockaddr_un addr;
	socklen_t addr_len;
	int interval_ms;
	struct wl_event_source *timer;
} notify = { .fd = -1 };

static void
send_state(const char *state)
{
	if (notify.fd < 0) {
		return;
	}
	if (sendto(notify.fd, state, strlen(state), MSG_NOSIGNAL,
			(struct sockaddr *)&notify.addr, notify.addr_len) < 0) {
		wlr_log_errno(WLR_DEBUG, "cannot notify the service manager");
	}
}

static int
handle_watchdog(void *data)
{
	send_state("WATCHDOG=1");
	wl_event_source_timer_update(notify.timer, notify.interval_ms);
	return 0;
}

void
service_notify_ready(struct wl_event_loop *loop)
{
	const char *path = getenv("NOTIFY_SOCKET");
	if (!path || !*path || strlen(path) >= sizeof(notify.addr.sun_path)) {
		return;
	}
	notify.addr.sun_family = AF_UNIX;
	strcpy(notify.addr.sun_path, path);
	if (path[0] == '@') {
		notify.addr.sun_path[0] = '\0';
	}
	notify.addr_len = offsetof(struct sockaddr_un, sun_path) + strlen(path);
	notify.fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (notify.fd < 0) {
		wlr_log_errno(WLR_ERROR, "cannot open the notify socket");
		return;
	}

	const char *usec = getenv("WATCHDOG_USEC");
	long long interval = usec ? atoll(usec) / 2000 : 0;
	if (interval > 0) {
		notify.interval_ms = interval > 0x7fffffff ? 0x7fffffff : (int)interval;
		notify.timer = wl_event_loop_add_timer(loop, handle_watchdog, NULL);
		if (notify.timer) {
			wl_event_source_timer_update(notify.timer, notify.interval_ms);
		}
	}
	send_state("READY=1");
	unsetenv("NOTIFY_SOCKET");
	unsetenv("WATCHDOG_USEC");
	unsetenv("WATCHDOG_PID");
}
