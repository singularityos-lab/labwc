/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_SINGULARITY_GESTURE_H
#define LABWC_SINGULARITY_GESTURE_H

#include <stdbool.h>
#include <stdint.h>
#include "config/mousebind.h"

void singularity_gesture_init(void);
bool singularity_gesture_has_clients(void);
void singularity_gesture_send_begin(uint32_t fingers, enum direction direction);
void singularity_gesture_send_update(double dx, double dy);
void singularity_gesture_send_end(bool cancelled, bool committed);

#endif /* LABWC_SINGULARITY_GESTURE_H */
