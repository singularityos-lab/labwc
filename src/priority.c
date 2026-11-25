// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE
#include "priority.h"
#include <gio/gio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/util/log.h>

#define MAIN_THREAD_NICE -10

void
priority_raise_main_thread(void)
{
	const char *env = getenv("LABWC_NICE");
	int nice = env ? atoi(env) : MAIN_THREAD_NICE;
	if (nice >= 0) {
		return;
	}

	GError *err = NULL;
	GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!bus) {
		wlr_log(WLR_INFO, "no system bus for priority: %s", err->message);
		g_error_free(err);
		return;
	}
	GVariant *ret = g_dbus_connection_call_sync(bus,
		"org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1",
		"org.freedesktop.RealtimeKit1", "MakeThreadHighPriority",
		g_variant_new("(ti)", (guint64)gettid(), (gint32)nice),
		NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, &err);
	if (ret) {
		wlr_log(WLR_INFO, "main thread runs at nice %d", nice);
		g_variant_unref(ret);
	} else {
		wlr_log(WLR_INFO, "priority not raised: %s", err->message);
		g_error_free(err);
	}
	g_object_unref(bus);
}
