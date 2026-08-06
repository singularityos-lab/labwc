/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_SINGULARITY_SPLASH_H
#define LABWC_SINGULARITY_SPLASH_H

struct output;

void singularity_splash_maybe_show(struct output *output);
void singularity_splash_dismiss(void);

#endif /* LABWC_SINGULARITY_SPLASH_H */
