/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_FRAME_STATS_H
#define LABWC_FRAME_STATS_H

#include <stdbool.h>
#include <stdint.h>

struct wlr_output;

/*
 * Frame timing log, enabled by LABWC_FRAME_LOG=<path>. Each line is one of
 *   C <output> <start_ns> <end_ns> <commit_seq>   a frame was rendered
 *   P <output> <when_ns> <refresh_ns> <commit_seq> <presented>
 * with CLOCK_MONOTONIC times. When the variable is unset nothing is hooked.
 */
void frame_stats_init(void);
void frame_stats_finish(void);
void frame_stats_add_output(struct wlr_output *wlr_output);
bool frame_stats_enabled(void);
int64_t frame_stats_now(void);
void frame_stats_commit(struct wlr_output *wlr_output, int64_t start_ns);

#endif /* LABWC_FRAME_STATS_H */
