/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_SINGULARITY_VEIL_H
#define LABWC_SINGULARITY_VEIL_H

#include <stdbool.h>

void singularity_veil_fade(bool faded, int duration_ms);
bool singularity_veil_active(void);
void singularity_veil_finish(void);

#endif /* LABWC_SINGULARITY_VEIL_H */
