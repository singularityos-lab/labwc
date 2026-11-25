// SPDX-License-Identifier: GPL-2.0-only

#include "frame-stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include "common/mem.h"

struct frame_stats_output {
	struct wlr_output *wlr_output;
	struct wl_listener present;
	struct wl_listener destroy;
};

static FILE *log_file;

static int64_t
ns(const struct timespec *ts)
{
	return (int64_t)ts->tv_sec * 1000000000LL + ts->tv_nsec;
}

int64_t
frame_stats_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ns(&ts);
}

bool
frame_stats_enabled(void)
{
	return log_file;
}

void
frame_stats_init(void)
{
	const char *path = getenv("LABWC_FRAME_LOG");
	if (!path || !*path) {
		return;
	}
	log_file = fopen(path, "we");
	if (!log_file) {
		wlr_log_errno(WLR_ERROR, "cannot open frame log %s", path);
		return;
	}
	setvbuf(log_file, NULL, _IOFBF, 1 << 16);
	wlr_log(WLR_INFO, "logging frame timing to %s", path);
}

void
frame_stats_finish(void)
{
	if (log_file) {
		fclose(log_file);
		log_file = NULL;
	}
}

static void
handle_present(struct wl_listener *listener, void *data)
{
	struct frame_stats_output *fo = wl_container_of(listener, fo, present);
	struct wlr_output_event_present *event = data;
	if (!log_file) {
		return;
	}
	fprintf(log_file, "P %s %lld %d %u %d\n", fo->wlr_output->name,
		event->presented ? (long long)ns(&event->when) : 0LL,
		event->refresh, event->commit_seq, event->presented ? 1 : 0);
}

static void
handle_destroy(struct wl_listener *listener, void *data)
{
	struct frame_stats_output *fo = wl_container_of(listener, fo, destroy);
	wl_list_remove(&fo->present.link);
	wl_list_remove(&fo->destroy.link);
	free(fo);
	if (log_file) {
		fflush(log_file);
	}
}

void
frame_stats_add_output(struct wlr_output *wlr_output)
{
	if (!log_file) {
		return;
	}
	struct frame_stats_output *fo = znew(*fo);
	fo->wlr_output = wlr_output;
	fo->present.notify = handle_present;
	wl_signal_add(&wlr_output->events.present, &fo->present);
	fo->destroy.notify = handle_destroy;
	wl_signal_add(&wlr_output->events.destroy, &fo->destroy);
}

void
frame_stats_commit(struct wlr_output *wlr_output, int64_t start_ns)
{
	if (!log_file) {
		return;
	}
	fprintf(log_file, "C %s %lld %lld %u\n", wlr_output->name,
		(long long)start_ns, (long long)frame_stats_now(),
		wlr_output->commit_seq);
}
