/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_SINGULARITY_PACING_H
#define LABWC_SINGULARITY_PACING_H

#include <stdbool.h>

struct output;

void singularity_pacing_init(void);
bool singularity_pacing_hold(struct output *output);
void singularity_pacing_output_destroy(struct output *output);

#endif
