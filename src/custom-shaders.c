// SPDX-License-Identifier: GPL-2.0-only
#include "custom-shaders.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <scenefx/render/fx_renderer/fx_renderer.h>
#include <wlr/util/log.h>
#include "common/buf.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "labwc.h"
#include "output.h"

static char *liquid_path;
static bool liquid_custom;
static int watch_fd = -1;
static struct wl_event_source *watch_source;

static char *
resolve_path(const char *value)
{
	struct buf b = BUF_INIT;
	buf_add(&b, value);
	buf_expand_tilde(&b);
	buf_expand_shell_variables(&b);
	char *path = xstrdup(b.data);
	buf_reset(&b);
	return path;
}

static char *
read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	struct buf b = BUF_INIT;
	char chunk[4096];
	size_t n;
	while ((n = fread(chunk, 1, sizeof(chunk) - 1, f)) > 0) {
		chunk[n] = '\0';
		buf_add(&b, chunk);
	}
	fclose(f);
	char *source = xstrdup(b.data);
	buf_reset(&b);
	return source;
}

static void
damage_all_outputs(void)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output->scene_output) {
			lab_wlr_scene_output_damage_whole(output->scene_output);
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
}

static void
liquid_restore(void)
{
	if (liquid_custom) {
		fx_renderer_set_liquid_shader(server.renderer, NULL, NULL);
		liquid_custom = false;
		wlr_log(WLR_INFO, "liquid shader: restored the built-in shader");
		damage_all_outputs();
	}
}

static void
liquid_load(void)
{
	if (!liquid_path) {
		liquid_restore();
		return;
	}
	char *source = read_file(liquid_path);
	if (!source) {
		if (errno == ENOENT) {
			wlr_log(WLR_DEBUG, "liquid shader: %s not found, using the "
				"built-in shader", liquid_path);
		} else {
			wlr_log(WLR_ERROR, "liquid shader: cannot read %s: %s",
				liquid_path, strerror(errno));
		}
		liquid_restore();
		return;
	}
	char *error = NULL;
	if (fx_renderer_set_liquid_shader(server.renderer, source, &error)) {
		liquid_custom = true;
		wlr_log(WLR_INFO, "liquid shader: loaded %s", liquid_path);
		damage_all_outputs();
	} else {
		wlr_log(WLR_ERROR, "liquid shader: %s rejected, keeping the "
			"shader in use:\n%s", liquid_path, error ? error : "");
	}
	free(error);
	free(source);
}

static int
handle_watch(int fd, uint32_t mask, void *data)
{
	char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	const char *slash = liquid_path ? strrchr(liquid_path, '/') : NULL;
	const char *base = slash ? slash + 1 : liquid_path;
	bool changed = false;
	ssize_t len;
	while ((len = read(fd, buffer, sizeof(buffer))) > 0) {
		for (char *p = buffer; p < buffer + len;) {
			struct inotify_event *event = (struct inotify_event *)p;
			if (event->len && base && !strcmp(event->name, base)) {
				changed = true;
			}
			p += sizeof(*event) + event->len;
		}
	}
	if (changed) {
		liquid_load();
	}
	return 0;
}

static void
watch_stop(void)
{
	if (watch_source) {
		wl_event_source_remove(watch_source);
		watch_source = NULL;
	}
	if (watch_fd >= 0) {
		close(watch_fd);
		watch_fd = -1;
	}
}

static void
watch_start(void)
{
	watch_stop();
	if (!liquid_path) {
		return;
	}
	char *dir = xstrdup(liquid_path);
	char *slash = strrchr(dir, '/');
	if (slash == dir) {
		slash[1] = '\0';
	} else if (slash) {
		*slash = '\0';
	}
	watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (watch_fd < 0 || inotify_add_watch(watch_fd, dir,
			IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM) < 0) {
		wlr_log(errno == ENOENT ? WLR_DEBUG : WLR_ERROR,
			"liquid shader: cannot watch %s: %s", dir, strerror(errno));
		watch_stop();
		free(dir);
		return;
	}
	watch_source = wl_event_loop_add_fd(server.wl_event_loop, watch_fd,
		WL_EVENT_READABLE, handle_watch, NULL);
	free(dir);
}

void
custom_shaders_reconfigure(void)
{
	zfree(liquid_path);
	if (rc.shaders.liquid && *rc.shaders.liquid) {
		liquid_path = resolve_path(rc.shaders.liquid);
	}
	watch_start();
	liquid_load();
}

void
custom_shaders_finish(void)
{
	watch_stop();
	zfree(liquid_path);
	liquid_custom = false;
}
