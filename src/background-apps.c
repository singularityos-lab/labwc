// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "background-apps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <scenefx/types/wlr_scene.h>
#include "common/mem.h"
#include "common/string-helpers.h"
#include "labwc.h"
#include "view.h"

#define TICK_MS 250
#define SUSPEND_AFTER_MS 3000
#define BACKGROUND_AFTER_MS 10000

static struct {
	struct wl_event_source *timer;
	char *flag;
	char *list;
	char *written;
	bool active;
} state;

static int64_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
buffer_visible(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	if (buffer->primary_output) {
		*(bool *)data = true;
	}
}

static bool
view_hidden(struct view *view)
{
	if (!view->mapped || view->minimized || !view->content_tree
			|| !view->content_tree->node.enabled) {
		return true;
	}
	bool visible = false;
	wlr_scene_node_for_each_buffer(&view->content_tree->node,
		buffer_visible, &visible);
	return !visible;
}

static void
set_suspended(struct view *view, bool suspended)
{
	if (view->background.suspended == suspended) {
		return;
	}
	view->background.suspended = suspended;
	if (view->type != LAB_XDG_SHELL_VIEW || !view->surface) {
		return;
	}
	struct wlr_xdg_toplevel *toplevel =
		wlr_xdg_toplevel_try_from_wlr_surface(view->surface);
	if (toplevel) {
		wlr_xdg_toplevel_set_suspended(toplevel, suspended);
	}
}

static char *
cgroup_of(struct view *view)
{
	if (!view->impl->get_pid) {
		return NULL;
	}
	pid_t pid = view->impl->get_pid(view);
	if (pid <= 1) {
		return NULL;
	}
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);
	FILE *file = fopen(path, "re");
	if (!file) {
		return NULL;
	}
	char line[4096];
	char *result = NULL;
	while (fgets(line, sizeof(line), file)) {
		if (strncmp(line, "0::", 3) == 0) {
			line[strcspn(line, "\n")] = '\0';
			result = xstrdup(line + 3);
			break;
		}
	}
	fclose(file);
	return result;
}

static void
write_list(const char *contents)
{
	if (state.written && !strcmp(state.written, contents)) {
		return;
	}
	char tmp[4200];
	snprintf(tmp, sizeof(tmp), "%s.tmp", state.list);
	FILE *file = fopen(tmp, "we");
	if (!file) {
		return;
	}
	fputs(contents, file);
	if (fclose(file) != 0 || rename(tmp, state.list) != 0) {
		unlink(tmp);
		return;
	}
	free(state.written);
	state.written = xstrdup(contents);
}

static void
reset(void)
{
	struct view *view;
	wl_list_for_each(view, &server.views, link) {
		set_suspended(view, false);
		view->background.hidden_since = 0;
	}
	if (state.written) {
		unlink(state.list);
		zfree(state.written);
	}
}

static void
refresh_cgroups(void)
{
	struct view *view;
	wl_list_for_each(view, &server.views, link) {
		if (!view->mapped) {
			continue;
		}
		char *cgroup = cgroup_of(view);
		free(view->background.cgroup);
		view->background.cgroup = cgroup;
	}
}

static void
tick(void)
{
	static unsigned int ticks;
	bool active = access(state.flag, F_OK) == 0;
	if (!active) {
		if (state.active) {
			reset();
			state.active = false;
		}
		return;
	}
	state.active = true;
	if (ticks++ % 8 == 0) {
		refresh_cgroups();
	}

	int64_t now = now_ms();
	struct view *view;
	wl_list_for_each(view, &server.views, link) {
		if (view_hidden(view)) {
			if (!view->background.hidden_since) {
				view->background.hidden_since = now;
			}
		} else {
			view->background.hidden_since = 0;
		}
		set_suspended(view, view->background.hidden_since
			&& now - view->background.hidden_since >= SUSPEND_AFTER_MS);
	}

	size_t size = 0;
	char *contents = NULL;
	FILE *out = open_memstream(&contents, &size);
	if (!out) {
		return;
	}
	wl_list_for_each(view, &server.views, link) {
		const char *cgroup = view->background.cgroup;
		if (!view->mapped || !cgroup || !strcmp(cgroup, "/")) {
			continue;
		}
		bool seen = false;
		bool background = true;
		struct view *other;
		wl_list_for_each(other, &server.views, link) {
			if (!other->mapped || !other->background.cgroup
					|| strcmp(other->background.cgroup, cgroup)) {
				continue;
			}
			if (other == view) {
				break;
			}
			seen = true;
		}
		if (seen) {
			continue;
		}
		wl_list_for_each(other, &server.views, link) {
			if (!other->mapped || !other->background.cgroup
					|| strcmp(other->background.cgroup, cgroup)) {
				continue;
			}
			if (!other->background.hidden_since || now
					- other->background.hidden_since < BACKGROUND_AFTER_MS) {
				background = false;
				break;
			}
		}
		if (background) {
			fprintf(out, "%s\n", cgroup);
		}
	}
	fclose(out);
	write_list(contents ? contents : "");
	free(contents);
}

static int
handle_timer(void *data)
{
	tick();
	wl_event_source_timer_update(state.timer, TICK_MS);
	return 0;
}

void
background_apps_init(void)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime) {
		return;
	}
	state.flag = strdup_printf("%s/singularity/background-apps", runtime);
	state.list = strdup_printf("%s/singularity/background-apps.list", runtime);
	unlink(state.list);
	state.timer = wl_event_loop_add_timer(server.wl_event_loop,
		handle_timer, NULL);
	wl_event_source_timer_update(state.timer, TICK_MS);
}

void
background_apps_finish(void)
{
	if (state.timer) {
		wl_event_source_remove(state.timer);
		state.timer = NULL;
	}
	if (state.list) {
		unlink(state.list);
	}
	zfree(state.flag);
	zfree(state.list);
	zfree(state.written);
}

void
background_apps_view_unmapped(struct view *view)
{
	view->background.hidden_since = 0;
	view->background.suspended = false;
	zfree(view->background.cgroup);
}
