/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <cairo.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output_layout.h>
#include <scenefx/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "config/rcxml.h"
#include "labwc.h"
#include "motion.h"
#include "output.h"
#include "buffer.h"
#include "img/img.h"
#include "common/scene-helpers.h"
#include "singularity-splash.h"

#define SINGULARITY_SPLASH_LOGO "/usr/share/singularity/splash-logo.png"
#define SINGULARITY_SPLASH_MARKER "/run/singularity/compositor-first-frame"
#define SINGULARITY_BOOT_PROGRESS "/run/boot-progress"
#define SINGULARITY_SHELL_READY "/run/singularity/shell-ready"
#define SINGULARITY_SPLASH_TICK_MS 33
#define SINGULARITY_SPLASH_FINISH_MS 300
#define SINGULARITY_SPLASH_HOLD_LIMIT_MS 30000
#define SINGULARITY_SPLASH_MAX_OUTPUTS 8

struct brand_layout {
	double logo_x, logo_y, logo_size;
	double bar_x, bar_y, bar_w, bar_h;
};

static struct wlr_scene_tree *splash_tree;
static struct wlr_scene_rect *bar_fill[SINGULARITY_SPLASH_MAX_OUTPUTS];
static struct wlr_scene_rect *bar_tracks[SINGULARITY_SPLASH_MAX_OUTPUTS];
static int bar_fill_width[SINGULARITY_SPLASH_MAX_OUTPUTS];
static int bar_count;
static bool splash_shown;
static bool splash_dismissed;
static bool splash_held;
static bool splash_finishing;
static bool marker_written;
static struct timespec splash_shown_at;
static struct timespec finish_at;
static double progress_from;
static double progress_target;
static double progress_tau;
static double progress_now;
static double finish_from;
static struct wl_event_source *tick_timer;

static const float splash_bg[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
static const float bar_track[4] = { 0.18f, 0.18f, 0.18f, 0.18f };
static const float bar_color[4] = { 0.92f, 0.92f, 0.92f, 0.92f };

static void destroy_splash(void);

static int
elapsed_ms(const struct timespec *since)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - since->tv_sec) * 1000
		+ (int)((now.tv_nsec - since->tv_nsec) / 1000000);
}

static void
write_first_frame_marker(void)
{
	if (marker_written) {
		return;
	}
	int fd = open(SINGULARITY_SPLASH_MARKER,
		O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		close(fd);
		marker_written = true;
	}
}

static double
read_boot_progress(void)
{
	FILE *f = fopen(SINGULARITY_BOOT_PROGRESS, "r");
	if (!f) {
		return 0.0;
	}
	double v = 0.0;
	if (fscanf(f, "%lf", &v) != 1 || v < 0.0 || v > 1.0) {
		v = 0.0;
	}
	fclose(f);
	return v;
}

static void
brand_layout(double width, double height, double scale, struct brand_layout *out)
{
	if (scale <= 0.0) {
		scale = 1.0;
	}
	double pw = width * scale, ph = height * scale;
	double k = ph >= 2000.0 ? 2.0 : (ph >= 1400.0 ? 1.5 : 1.0);
	double size = floor(256.0 * k);
	if (size > pw * 0.5) {
		size = floor(pw * 0.5);
	}
	if (size > ph * 0.5) {
		size = floor(ph * 0.5);
	}
	double top = floor((ph - size) / 2.0);
	double left = floor((pw - size) / 2.0);
	double bar_w = floor(size * 220.0 / 256.0);
	double bar_h = fmax(2.0, floor(size / 64.0));
	out->logo_size = size / scale;
	out->logo_x = left / scale;
	out->logo_y = top / scale;
	out->bar_w = bar_w / scale;
	out->bar_h = bar_h / scale;
	out->bar_x = floor((pw - bar_w) / 2.0) / scale;
	out->bar_y = (top + size + floor(size * 48.0 / 256.0)) / scale;
}

static void
add_rect(struct wlr_scene_tree *tree, double x, double y, double w, double h,
		const float color[4])
{
	int iw = (int)lround(w), ih = (int)lround(h);
	if (iw < 1 || ih < 1) {
		return;
	}
	struct wlr_scene_rect *r = lab_wlr_scene_rect_create(tree, iw, ih, color);
	wlr_scene_node_set_position(&r->node, (int)lround(x), (int)lround(y));
}

static void
output_size(struct output *output, int *width, int *height, double *scale)
{
	struct wlr_output *o = output->wlr_output;
	struct wlr_output_state *pending = &output->pending;
	*scale = (pending->committed & WLR_OUTPUT_STATE_SCALE)
		? pending->scale : o->scale;
	if (*scale <= 0.0f) {
		*scale = 1.0;
	}
	int pw = o->width, ph = o->height;
	if (pending->committed & WLR_OUTPUT_STATE_MODE) {
		if (pending->mode_type == WLR_OUTPUT_STATE_MODE_FIXED && pending->mode) {
			pw = pending->mode->width;
			ph = pending->mode->height;
		} else if (pending->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
			pw = pending->custom_mode.width;
			ph = pending->custom_mode.height;
		}
	}
	*width = (int)lround(pw / *scale);
	*height = (int)lround(ph / *scale);
	struct wlr_box box;
	wlr_output_layout_get_box(server.output_layout, o, &box);
	if (box.width > 0 && box.height > 0) {
		*width = box.width;
		*height = box.height;
	}
}

static void
build_brand_frame(struct wlr_scene_tree *tree, struct output *output,
		int x, int y)
{
	int width, height;
	double scale;
	output_size(output, &width, &height, &scale);
	struct brand_layout l;
	brand_layout(width, height, scale, &l);

	int lx = x + (int)lround(l.logo_x), ly = y + (int)lround(l.logo_y);
	int size = (int)lround(l.logo_size);
	add_rect(tree, x, y, width, ly - y, splash_bg);
	add_rect(tree, x, ly + size, width, y + height - ly - size, splash_bg);
	add_rect(tree, x, ly, lx - x, size, splash_bg);
	add_rect(tree, lx + size, ly, x + width - lx - size, size, splash_bg);

	struct lab_data_buffer *pixels = NULL;
	const char *logo_path = getenv("SINGULARITY_SPLASH_LOGO");
	if (!logo_path || !*logo_path) {
		logo_path = SINGULARITY_SPLASH_LOGO;
	}
	struct lab_img *logo = lab_img_load(LAB_IMG_PNG, logo_path, NULL);
	if (logo) {
		pixels = lab_img_render(logo, size, size, scale);
		lab_img_destroy(logo);
	}
	if (pixels) {
		cairo_t *cairo = cairo_create(pixels->surface);
		cairo_set_operator(cairo, CAIRO_OPERATOR_DEST_OVER);
		cairo_set_source_rgba(cairo, splash_bg[0], splash_bg[1],
			splash_bg[2], splash_bg[3]);
		cairo_paint(cairo);
		cairo_destroy(cairo);
		cairo_surface_flush(pixels->surface);
		struct wlr_scene_buffer *sb =
			wlr_scene_buffer_create(tree, &pixels->base);
		wlr_buffer_drop(&pixels->base);
		if (sb) {
			wlr_scene_buffer_set_dest_size(sb, size, size);
			wlr_scene_node_set_position(&sb->node, lx, ly);
		}
	} else {
		add_rect(tree, lx, ly, size, size, splash_bg);
	}

	int bx = x + (int)lround(l.bar_x), by = y + (int)lround(l.bar_y);
	int bw = (int)lround(l.bar_w), bh = (int)lround(l.bar_h);
	if (bh < 1) {
		bh = 1;
	}
	struct wlr_scene_rect *track =
		lab_wlr_scene_rect_create(tree, bw, bh, bar_track);
	wlr_scene_node_set_position(&track->node, bx, by);
	wlr_scene_rect_set_corner_radius(track, bh / 2);
	if (bar_count < SINGULARITY_SPLASH_MAX_OUTPUTS) {
		bar_tracks[bar_count] = track;
		struct wlr_scene_rect *fill =
			lab_wlr_scene_rect_create(tree, 1, bh, bar_color);
		wlr_scene_node_set_position(&fill->node, bx, by);
		wlr_scene_rect_set_corner_radius(fill, bh / 2);
		wlr_scene_node_set_enabled(&fill->node, false);
		bar_fill[bar_count] = fill;
		bar_fill_width[bar_count] = bw;
		bar_count++;
	}
}

static void
set_bar(double progress)
{
	if (progress < 0.0) {
		progress = 0.0;
	}
	if (progress > 1.0) {
		progress = 1.0;
	}
	progress_now = progress;
	for (int i = 0; i < bar_count; i++) {
		int h = bar_fill[i]->height;
		int w = (int)lround(bar_fill_width[i] * progress);
		if (w < h) {
			wlr_scene_node_set_enabled(&bar_fill[i]->node, false);
			continue;
		}
		wlr_scene_rect_set_size(bar_fill[i], w, h);
		wlr_scene_node_set_enabled(&bar_fill[i]->node, true);
	}
}

static void
start_progress(double from, double target, double tau)
{
	progress_from = from;
	progress_target = target;
	progress_tau = tau;
	splash_finishing = false;
	set_bar(from);
}

static double
current_progress(void)
{
	if (splash_finishing) {
		double t = elapsed_ms(&finish_at) / (double)SINGULARITY_SPLASH_FINISH_MS;
		if (t >= 1.0) {
			return 1.0;
		}
		double eased = 1.0 - (1.0 - t) * (1.0 - t);
		return finish_from + (1.0 - finish_from) * eased;
	}
	double e = elapsed_ms(&splash_shown_at) / 1000.0;
	return progress_from + (progress_target - progress_from)
		* (1.0 - exp(-e / progress_tau));
}

static void
finish(void)
{
	if (splash_finishing) {
		return;
	}
	finish_from = progress_now;
	clock_gettime(CLOCK_MONOTONIC, &finish_at);
	splash_finishing = true;
}

static int
handle_tick(void *data)
{
	if (!splash_tree || (splash_dismissed && !splash_held && !splash_finishing)) {
		return 0;
	}
	set_bar(current_progress());
	if (splash_held && !splash_finishing) {
		if (access(SINGULARITY_SHELL_READY, F_OK) == 0
				|| elapsed_ms(&splash_shown_at) > SINGULARITY_SPLASH_HOLD_LIMIT_MS) {
			finish();
		}
	}
	if (splash_finishing
			&& elapsed_ms(&finish_at) >= SINGULARITY_SPLASH_FINISH_MS) {
		splash_held = false;
		destroy_splash();
		return 0;
	}
	wl_event_source_timer_update(tick_timer, SINGULARITY_SPLASH_TICK_MS);
	return 0;
}

static void
start_ticking(void)
{
	if (!tick_timer) {
		tick_timer = wl_event_loop_add_timer(server.wl_event_loop,
			handle_tick, NULL);
	}
	if (tick_timer) {
		wl_event_source_timer_update(tick_timer, SINGULARITY_SPLASH_TICK_MS);
	}
}

static void
new_tree(void)
{
	splash_tree = lab_wlr_scene_tree_create(&server.scene->tree);
	wlr_scene_node_set_position(&splash_tree->node, 0, 0);
	bar_count = 0;
}

void
singularity_splash_maybe_show(struct output *output)
{
	if (splash_dismissed) {
		return;
	}
	if (splash_shown) {
		write_first_frame_marker();
		return;
	}
	if (!output || !output->wlr_output) {
		return;
	}
	int width, height;
	double scale;
	output_size(output, &width, &height, &scale);
	if (width <= 0 || height <= 0) {
		return;
	}

	new_tree();
	build_brand_frame(splash_tree, output, 0, 0);
	wlr_scene_node_raise_to_top(&splash_tree->node);
	splash_shown = true;
	clock_gettime(CLOCK_MONOTONIC, &splash_shown_at);
	start_progress(read_boot_progress(), 0.92, 3.0);
	start_ticking();
}

static struct motion_timed splash_fade;

static void
set_splash_opacity(float opacity)
{
	struct wlr_scene_node *node;
	wl_list_for_each(node, &splash_tree->children, link) {
		if (node->type == WLR_SCENE_NODE_RECT) {
			struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
			const float *base = splash_bg;
			for (int i = 0; i < bar_count; i++) {
				if (rect == bar_fill[i]) {
					base = bar_color;
				} else if (rect == bar_tracks[i]) {
					base = bar_track;
				}
			}
			float color[4] = { base[0] * opacity, base[1] * opacity,
				base[2] * opacity, base[3] * opacity };
			wlr_scene_rect_set_color(rect, color);
		} else if (node->type == WLR_SCENE_NODE_BUFFER) {
			wlr_scene_buffer_set_opacity(
				wlr_scene_buffer_from_node(node), opacity);
		}
	}
}

static void
handle_splash_fade_update(struct motion_timed *timed, double eased,
		double linear)
{
	if (splash_tree) {
		set_splash_opacity(1.0f - (float)eased);
	}
}

static void
handle_splash_fade_done(struct motion_timed *timed)
{
	if (splash_tree) {
		wlr_scene_node_destroy(&splash_tree->node);
		splash_tree = NULL;
		bar_count = 0;
	}
}

static void
destroy_splash(void)
{
	splash_dismissed = true;
	if (!splash_tree || motion_timed_running(&splash_fade)) {
		return;
	}
	splash_fade.update = handle_splash_fade_update;
	splash_fade.done = handle_splash_fade_done;
	motion_timed_start_custom(&splash_fade, MOTION_SPLASH_FADE,
		motion_enabled() ? motion_duration_ms(MOTION_SPLASH_FADE) : 0,
		rc.motion.timing[MOTION_SPLASH_FADE].curve);
}

void
singularity_splash_dismiss(void)
{
	if (splash_held) {
		if (splash_tree) {
			wlr_scene_node_raise_to_top(&splash_tree->node);
		}
		return;
	}
	if (splash_dismissed || splash_finishing) {
		return;
	}
	if (!splash_shown) {
		splash_dismissed = true;
		return;
	}
	finish();
	start_ticking();
}

void
singularity_splash_hold(void)
{
	splash_dismissed = true;
	splash_finishing = false;
	motion_timed_cancel(&splash_fade);
	if (splash_tree) {
		wlr_scene_node_destroy(&splash_tree->node);
		splash_tree = NULL;
	}
	unlink(SINGULARITY_SHELL_READY);
	new_tree();
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output_is_usable(output)) {
			continue;
		}
		struct wlr_box box;
		wlr_output_layout_get_box(server.output_layout,
			output->wlr_output, &box);
		build_brand_frame(splash_tree, output, box.x, box.y);
	}
	wlr_scene_node_raise_to_top(&splash_tree->node);
	splash_held = true;
	clock_gettime(CLOCK_MONOTONIC, &splash_shown_at);
	start_progress(0.0, 0.92, 3.0);
	start_ticking();
	wlr_log(WLR_INFO, "singularity splash: holding the brand frame until the shell is ready");
}
