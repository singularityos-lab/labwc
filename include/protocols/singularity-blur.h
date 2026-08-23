// SPDX-License-Identifier: GPL-2.0-only
#ifndef SINGULARITY_BLUR_H
#define SINGULARITY_BLUR_H

#include <pixman.h>
#include <stdbool.h>

struct output;
struct wlr_output_state;

void singularity_blur_init(void);
bool singularity_blur_has_effects(void);
bool singularity_blur_output_has_animations(struct output *output);
void singularity_blur_output_damage(struct output *output,
		pixman_region32_t *damage);
void singularity_blur_render(struct output *output, struct wlr_output_state *state);
void singularity_blur_finish(void);

#endif /* SINGULARITY_BLUR_H */
