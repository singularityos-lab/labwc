/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_SINGULARITY_PROCESS_H
#define LABWC_SINGULARITY_PROCESS_H

#include <stdbool.h>

struct view;

void singularity_process_init(void);
bool singularity_process_request_resume(struct view *view);

#endif /* LABWC_SINGULARITY_PROCESS_H */
