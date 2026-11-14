// SPDX-License-Identifier: GPL-2.0-only
#include "app-pause.h"
#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>
#include <scenefx/types/wlr_scene.h>
#include "buffer.h"
#include "common/list.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "labwc.h"
#include "node.h"
#include "output.h"
#include "protocols/singularity-process.h"
#include "ssd.h"
#include "view.h"
#include "view-animation.h"

#define CGROUP_ROOT "/sys/fs/cgroup"
#define BUTTON_SIZE 72

struct watch {
	struct wl_list link;
	int wd;
	char *path;
	bool directory;
};

static struct {
	int fd;
	struct wl_event_source *source;
	struct wl_event_source *idle;
	struct wl_list watches;
} state = { .fd = -1 };

static char *
cgroup_of_pid(pid_t pid)
{
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

static bool
cgroup_is_frozen(const char *cgroup)
{
	char path[4200];
	snprintf(path, sizeof(path), CGROUP_ROOT "%s/cgroup.events", cgroup);
	FILE *file = fopen(path, "re");
	if (!file) {
		return false;
	}
	char line[128];
	bool frozen = false;
	while (fgets(line, sizeof(line), file)) {
		if (strcmp(line, "frozen 1\n") == 0) {
			frozen = true;
		}
	}
	fclose(file);
	return frozen;
}

static void
watch_add(const char *path, bool directory)
{
	struct watch *watch;
	wl_list_for_each(watch, &state.watches, link) {
		if (watch->directory == directory && !strcmp(watch->path, path)) {
			return;
		}
	}
	int wd;
	if (directory) {
		wd = inotify_add_watch(state.fd, path, IN_CREATE | IN_ONLYDIR);
	} else {
		char events[4200];
		snprintf(events, sizeof(events), "%s/cgroup.events", path);
		wd = inotify_add_watch(state.fd, events, IN_MODIFY);
	}
	if (wd < 0) {
		return;
	}
	watch = znew(*watch);
	watch->wd = wd;
	watch->path = xstrdup(path);
	watch->directory = directory;
	wl_list_insert(&state.watches, &watch->link);
}

static void
watch_cgroup(const char *cgroup)
{
	if (!cgroup || !strcmp(cgroup, "/")) {
		return;
	}
	char path[4200];
	snprintf(path, sizeof(path), CGROUP_ROOT "%s", cgroup);
	watch_add(path, false);
	char *slash = strrchr(path, '/');
	if (slash && slash > path + strlen(CGROUP_ROOT)) {
		*slash = '\0';
		watch_add(path, true);
	}
}

static void
draw_button(cairo_t *cairo)
{
	double r = BUTTON_SIZE / 2.0;
	cairo_arc(cairo, r, r, r - 1, 0, 2 * 3.14159265358979);
	cairo_set_source_rgba(cairo, 0.08, 0.08, 0.08, 0.72);
	cairo_fill_preserve(cairo);
	cairo_set_line_width(cairo, 1.5);
	cairo_set_source_rgba(cairo, 1, 1, 1, 0.35);
	cairo_stroke(cairo);

	double h = BUTTON_SIZE * 0.36;
	double w = h * 0.9;
	double x = r - w * 0.38;
	cairo_move_to(cairo, x, r - h / 2);
	cairo_line_to(cairo, x + w, r);
	cairo_line_to(cairo, x, r + h / 2);
	cairo_close_path(cairo);
	cairo_set_line_join(cairo, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_width(cairo, 4);
	cairo_set_source_rgba(cairo, 1, 1, 1, 0.95);
	cairo_stroke_preserve(cairo);
	cairo_fill(cairo);
}

static float
view_scale(struct view *view)
{
	if (view->output && output_is_usable(view->output)) {
		return view->output->wlr_output->scale;
	}
	return 1.0f;
}

static void
overlay_destroy(struct view *view)
{
	if (view->pause.tree) {
		wlr_scene_node_destroy(&view->pause.tree->node);
		view->pause.tree = NULL;
	}
	if (view->content_tree) {
		wlr_scene_node_set_enabled(&view->content_tree->node,
			!view->shaded);
	}
}

static void
overlay_create(struct view *view)
{
	overlay_destroy(view);
	if (!view->mapped || !view->scene_tree) {
		return;
	}
	float scale = view_scale(view);
	struct wlr_box box = ssd_max_extents(view);
	struct wlr_buffer *snapshot = view_render_grayscale_snapshot(view, scale);
	if (!snapshot) {
		return;
	}
	int tx, ty;
	wlr_scene_node_coords(&view->scene_tree->node, &tx, &ty);

	view->pause.tree = lab_wlr_scene_tree_create(view->scene_tree);
	struct wlr_scene_buffer *image =
		lab_wlr_scene_buffer_create(view->pause.tree, snapshot);
	wlr_buffer_drop(snapshot);
	wlr_scene_buffer_set_dest_size(image, box.width, box.height);
	wlr_scene_node_set_position(&image->node, box.x - tx, box.y - ty);

	struct lab_data_buffer *button =
		buffer_create_cairo(BUTTON_SIZE, BUTTON_SIZE, scale);
	if (button) {
		cairo_t *cairo = cairo_create(button->surface);
		draw_button(cairo);
		cairo_destroy(cairo);
		cairo_surface_flush(button->surface);
		struct wlr_scene_buffer *node = lab_wlr_scene_buffer_create(
			view->pause.tree, &button->base);
		wlr_buffer_drop(&button->base);
		wlr_scene_buffer_set_dest_size(node, BUTTON_SIZE, BUTTON_SIZE);
		wlr_scene_node_set_position(&node->node,
			view->current.x - tx + (view->current.width - BUTTON_SIZE) / 2,
			view->current.y - ty + (view->current.height - BUTTON_SIZE) / 2);
		node_descriptor_create(&node->node, LAB_NODE_RESUME_BUTTON,
			view, NULL);
	}
	if (view->content_tree) {
		wlr_scene_node_set_enabled(&view->content_tree->node, false);
	}
}

static void
view_refresh(struct view *view)
{
	bool frozen = false;
	if (view->mapped && view->impl->get_pid) {
		pid_t pid = view->impl->get_pid(view);
		char *cgroup = pid > 1 ? cgroup_of_pid(pid) : NULL;
		if (cgroup) {
			watch_cgroup(cgroup);
			frozen = cgroup_is_frozen(cgroup);
		}
		free(view->pause.cgroup);
		view->pause.cgroup = cgroup;
	}
	if (frozen == view->pause.frozen) {
		return;
	}
	view->pause.frozen = frozen;
	wlr_log(WLR_INFO, "app-pause: %s %s", view->app_id,
		frozen ? "paused" : "resumed");
	if (frozen) {
		overlay_create(view);
	} else {
		overlay_destroy(view);
	}
}

static void
refresh_all(void *data)
{
	state.idle = NULL;
	struct view *view;
	wl_list_for_each(view, &server.views, link) {
		view_refresh(view);
	}
}

static void
schedule_refresh(void)
{
	if (!state.idle) {
		state.idle = wl_event_loop_add_idle(server.wl_event_loop,
			refresh_all, NULL);
	}
}

static int
handle_inotify(int fd, uint32_t mask, void *data)
{
	char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	for (;;) {
		ssize_t len = read(fd, buffer, sizeof(buffer));
		if (len <= 0) {
			break;
		}
		for (char *p = buffer; p < buffer + len;) {
			struct inotify_event *event = (struct inotify_event *)p;
			struct watch *watch, *tmp;
			wl_list_for_each_safe(watch, tmp, &state.watches, link) {
				if (watch->wd != event->wd) {
					continue;
				}
				if (event->mask & IN_IGNORED) {
					wl_list_remove(&watch->link);
					free(watch->path);
					free(watch);
				} else if (watch->directory && event->len
						&& !strncmp(event->name, "app-", 4)) {
					char path[4200];
					snprintf(path, sizeof(path), "%s/%s",
						watch->path, event->name);
					watch_add(path, false);
				}
				break;
			}
			p += sizeof(*event) + event->len;
		}
	}
	schedule_refresh();
	return 0;
}

void
app_pause_init(void)
{
	wl_list_init(&state.watches);
	if (access(CGROUP_ROOT "/cgroup.controllers", R_OK) != 0) {
		return;
	}
	state.fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (state.fd < 0) {
		return;
	}
	state.source = wl_event_loop_add_fd(server.wl_event_loop, state.fd,
		WL_EVENT_READABLE, handle_inotify, NULL);
}

void
app_pause_finish(void)
{
	struct watch *watch, *tmp;
	wl_list_for_each_safe(watch, tmp, &state.watches, link) {
		wl_list_remove(&watch->link);
		free(watch->path);
		free(watch);
	}
	if (state.idle) {
		wl_event_source_remove(state.idle);
		state.idle = NULL;
	}
	if (state.source) {
		wl_event_source_remove(state.source);
		state.source = NULL;
	}
	if (state.fd >= 0) {
		close(state.fd);
		state.fd = -1;
	}
}

void
app_pause_view_mapped(struct view *view)
{
	if (state.fd >= 0) {
		view_refresh(view);
	}
}

void
app_pause_view_unmapped(struct view *view)
{
	view->pause.frozen = false;
	overlay_destroy(view);
	zfree(view->pause.cgroup);
}

bool
app_pause_view_is_paused(struct view *view)
{
	return view && view->pause.frozen;
}

void
app_pause_request_resume(struct view *view)
{
	if (!app_pause_view_is_paused(view)) {
		return;
	}
	if (!singularity_process_request_resume(view)) {
		wlr_log(WLR_INFO, "app-pause: no client to resume %s",
			view->app_id);
	}
}
