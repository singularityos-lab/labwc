/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_SERVICE_NOTIFY_H
#define LABWC_SERVICE_NOTIFY_H

struct wl_event_loop;

/*
 * Tell the service manager on $NOTIFY_SOCKET that the compositor is ready
 * and, when $WATCHDOG_USEC is set, keep pinging it from the event loop so a
 * hung compositor is restarted. Without $NOTIFY_SOCKET it does nothing.
 */
void service_notify_ready(struct wl_event_loop *loop);

#endif /* LABWC_SERVICE_NOTIFY_H */
