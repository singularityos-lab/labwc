/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_SINGULARITY_KEY_HOLD_H
#define LABWC_SINGULARITY_KEY_HOLD_H

#include <stdbool.h>
#include <stdint.h>

void singularity_key_hold_init(void);
bool singularity_key_hold_active(void);
void singularity_key_hold_press(uint32_t keycode, bool super_alone);
bool singularity_key_hold_release(uint32_t keycode);
void singularity_key_hold_pointer_button(void);
void singularity_key_hold_cancel(void);

#endif /* LABWC_SINGULARITY_KEY_HOLD_H */
