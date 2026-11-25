/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_PRIORITY_H
#define LABWC_PRIORITY_H

/*
 * Ask RealtimeKit for a raised nice level on the calling thread, which runs
 * rendering and input. Child processes start at the default priority.
 */
void priority_raise_main_thread(void);

#endif /* LABWC_PRIORITY_H */
