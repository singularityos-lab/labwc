/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_SINGULARITY_BRIGHTNESS_H
#define LABWC_SINGULARITY_BRIGHTNESS_H

#include <stdbool.h>
#include <stdint.h>

struct output;

#define SINGULARITY_BRIGHTNESS_MIN 150

void singularity_brightness_init(void);
void singularity_brightness_output_ready(struct output *output);
void singularity_brightness_apply(struct output *output);
void singularity_brightness_reset(void);
uint64_t singularity_brightness_edid_id(const char *connector, bool *found);

#endif
