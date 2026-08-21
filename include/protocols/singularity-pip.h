/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_SINGULARITY_PIP_H
#define LABWC_SINGULARITY_PIP_H

#include <stdbool.h>
#include <stdint.h>

struct output;
struct timespec;
struct wlr_scene_node;

enum singularity_pip_cursor_area {
	SINGULARITY_PIP_CURSOR_NONE = 0,
	SINGULARITY_PIP_CURSOR_CONTENT,
	SINGULARITY_PIP_CURSOR_CLOSE,
	SINGULARITY_PIP_CURSOR_RESIZE,
};

void singularity_pip_init(void);
void singularity_pip_finish(void);
void singularity_pip_close(void);
void singularity_pip_output_destroy(struct output *output);
void singularity_pip_render_output(struct output *output);
void singularity_pip_send_frame_done(struct output *output,
	const struct timespec *when);

enum singularity_pip_cursor_area singularity_pip_cursor_area(
	struct wlr_scene_node *node);
bool singularity_pip_interactive(void);
bool singularity_pip_button_press(struct wlr_scene_node *node,
	uint32_t button, double x, double y);
bool singularity_pip_button_release(uint32_t button);
void singularity_pip_cursor_motion(double x, double y);

#endif /* LABWC_SINGULARITY_PIP_H */
