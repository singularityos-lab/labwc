/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <cairo.h>
#include <fcntl.h>
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
#include "scaled-buffer/scaled-img-buffer.h"
#include "singularity-splash.h"

#define SINGULARITY_SPLASH_LOGO "/usr/share/singularity/splash-logo.png"
#define SINGULARITY_SPLASH_MARKER "/run/singularity/compositor-first-frame"
#define SINGULARITY_SPLASH_LOGO_SIZE 256
#define SINGULARITY_SPLASH_MIN_SHOW_MS 400

static struct wlr_scene_tree *splash_tree;
static bool splash_shown;
static bool splash_dismissed;
static bool splash_held;
static bool marker_written;
static struct timespec splash_shown_at;
static struct wl_event_source *dismiss_timer;

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

static const float splash_bg[4] = { 0.05f, 0.05f, 0.07f, 1.0f };

static void
build_brand_frame(struct wlr_scene_tree *tree, struct output *output,
		int x, int y)
{
	int width = output->wlr_output->width;
	int height = output->wlr_output->height;
	struct wlr_box box;
	wlr_output_layout_get_box(server.output_layout, output->wlr_output, &box);
	if (box.width > 0 && box.height > 0) {
		width = box.width;
		height = box.height;
	}
	int logo_x = x + (width - SINGULARITY_SPLASH_LOGO_SIZE) / 2;
	int logo_y = y + (height - SINGULARITY_SPLASH_LOGO_SIZE) / 2;
	struct lab_data_buffer *pixels = NULL;
	const char *logo_path = getenv("SINGULARITY_SPLASH_LOGO");
	if (!logo_path || !*logo_path) {
		logo_path = SINGULARITY_SPLASH_LOGO;
	}
	struct lab_img *logo = lab_img_load(LAB_IMG_PNG, logo_path, NULL);
	if (logo) {
		pixels = lab_img_render(logo, SINGULARITY_SPLASH_LOGO_SIZE,
			SINGULARITY_SPLASH_LOGO_SIZE, output->wlr_output->scale);
		lab_img_destroy(logo);
	} else {
		wlr_log(WLR_INFO, "singularity splash: no logo at %s, brand fill only",
			logo_path);
	}
	if (!pixels) {
		struct wlr_scene_rect *fill =
			lab_wlr_scene_rect_create(tree, width, height, splash_bg);
		wlr_scene_node_set_position(&fill->node, x, y);
		return;
	}

	cairo_t *cairo = cairo_create(pixels->surface);
	cairo_set_operator(cairo, CAIRO_OPERATOR_DEST_OVER);
	cairo_set_source_rgba(cairo, splash_bg[0], splash_bg[1], splash_bg[2],
		splash_bg[3]);
	cairo_paint(cairo);
	cairo_destroy(cairo);
	cairo_surface_flush(pixels->surface);

	int size = SINGULARITY_SPLASH_LOGO_SIZE;
	struct wlr_box bands[4] = {
		{ x, y, width, logo_y - y },
		{ x, logo_y + size, width, y + height - logo_y - size },
		{ x, logo_y, logo_x - x, size },
		{ logo_x + size, logo_y, x + width - logo_x - size, size },
	};
	for (int i = 0; i < 4; i++) {
		if (bands[i].width < 1 || bands[i].height < 1) {
			continue;
		}
		struct wlr_scene_rect *band = lab_wlr_scene_rect_create(tree,
			bands[i].width, bands[i].height, splash_bg);
		wlr_scene_node_set_position(&band->node, bands[i].x, bands[i].y);
	}
	struct wlr_scene_buffer *sb = wlr_scene_buffer_create(tree, &pixels->base);
	wlr_buffer_drop(&pixels->base);
	if (sb) {
		wlr_scene_buffer_set_dest_size(sb, size, size);
		wlr_scene_node_set_position(&sb->node, logo_x, logo_y);
	}
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
	if (output->wlr_output->width <= 0 || output->wlr_output->height <= 0) {
		return;
	}

	splash_tree = lab_wlr_scene_tree_create(&server.scene->tree);
	wlr_scene_node_set_position(&splash_tree->node, 0, 0);
	build_brand_frame(splash_tree, output, 0, 0);

	wlr_scene_node_raise_to_top(&splash_tree->node);
	splash_shown = true;
	clock_gettime(CLOCK_MONOTONIC, &splash_shown_at);
	write_first_frame_marker();
}

static struct motion_timed splash_fade;

static void
set_splash_opacity(float opacity)
{
	const float *bg = splash_bg;
	struct wlr_scene_node *node;
	wl_list_for_each(node, &splash_tree->children, link) {
		if (node->type == WLR_SCENE_NODE_RECT) {
			float color[4] = { bg[0] * opacity, bg[1] * opacity,
				bg[2] * opacity, bg[3] * opacity };
			wlr_scene_rect_set_color(wlr_scene_rect_from_node(node), color);
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
	}
}

static void
destroy_splash(void)
{
	splash_dismissed = true;
	if (dismiss_timer) {
		wl_event_source_remove(dismiss_timer);
		dismiss_timer = NULL;
	}
	if (!splash_tree || motion_timed_running(&splash_fade)) {
		return;
	}
	splash_fade.update = handle_splash_fade_update;
	splash_fade.done = handle_splash_fade_done;
	motion_timed_start_custom(&splash_fade, MOTION_SPLASH_FADE,
		motion_enabled() ? motion_duration_ms(MOTION_SPLASH_FADE) : 0,
		rc.motion.timing[MOTION_SPLASH_FADE].curve);
}

static int
handle_dismiss_timer(void *data)
{
	destroy_splash();
	return 0;
}

static int
splash_elapsed_ms(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - splash_shown_at.tv_sec) * 1000
		+ (int)((now.tv_nsec - splash_shown_at.tv_nsec) / 1000000);
}

void
singularity_splash_dismiss(void)
{
	if (splash_held) {
		splash_held = false;
		destroy_splash();
		return;
	}
	if (splash_dismissed) {
		return;
	}
	if (!splash_shown) {
		/* A client mapped before a single splash frame was presented;
		 * mark dismissed so the splash is never raised after the fact. */
		splash_dismissed = true;
		return;
	}

	/* Hold the logo for a minimum so it is an actual splash rather than a
	 * one-frame flash, even when the first client maps immediately after the
	 * compositor's first frame. A timer drives the late dismiss so it fires
	 * even if the scene goes idle and stops producing frames. */
	int elapsed = splash_elapsed_ms();
	if (elapsed >= SINGULARITY_SPLASH_MIN_SHOW_MS) {
		destroy_splash();
		return;
	}
	if (!dismiss_timer) {
		dismiss_timer = wl_event_loop_add_timer(server.wl_event_loop,
			handle_dismiss_timer, NULL);
		if (!dismiss_timer) {
			destroy_splash();
			return;
		}
		wl_event_source_timer_update(dismiss_timer,
			SINGULARITY_SPLASH_MIN_SHOW_MS - elapsed);
	}
}

void
singularity_splash_hold(void)
{
	splash_dismissed = true;
	if (dismiss_timer) {
		wl_event_source_remove(dismiss_timer);
		dismiss_timer = NULL;
	}
	motion_timed_cancel(&splash_fade);
	if (splash_tree) {
		wlr_scene_node_destroy(&splash_tree->node);
		splash_tree = NULL;
	}
	splash_tree = lab_wlr_scene_tree_create(&server.scene->tree);
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
	wlr_log(WLR_INFO, "singularity splash: holding the brand frame for the session handoff");
}
