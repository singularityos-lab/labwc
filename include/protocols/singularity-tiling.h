/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_SINGULARITY_TILING_H
#define LABWC_SINGULARITY_TILING_H

#include <stdbool.h>
#include <stdint.h>
#include <wlr/util/box.h>

struct view;

enum singularity_tiling_interaction_phase {
	SINGULARITY_TILING_INTERACTION_BEGIN,
	SINGULARITY_TILING_INTERACTION_UPDATE,
	SINGULARITY_TILING_INTERACTION_END,
};

enum singularity_tiling_interaction_kind {
	SINGULARITY_TILING_INTERACTION_MOVE,
	SINGULARITY_TILING_INTERACTION_RESIZE,
};

void singularity_tiling_init(void);
bool singularity_tiling_scrolling_mode_enabled(void);
bool singularity_tiling_float_candidate(struct view *view);
bool singularity_tiling_get_float_drop_box(struct view *view,
	struct wlr_box *box);
bool singularity_tiling_get_drop_preview_box(struct wlr_box *box);
void singularity_tiling_send_group_state(struct view *view);
void singularity_tiling_send_interaction(struct view *view,
	enum singularity_tiling_interaction_phase phase,
	enum singularity_tiling_interaction_kind kind,
	const struct wlr_box *geometry, uint32_t edges);

#endif /* LABWC_SINGULARITY_TILING_H */
