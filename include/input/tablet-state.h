/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_TABLET_STATE_H
#define LABWC_TABLET_STATE_H

struct seat;

void tablet_state_set_socket(const char *socket);
char *tablet_state_path(void);
void tablet_state_remove(void);
void tablet_state_write(struct seat *seat);

#endif /* LABWC_TABLET_STATE_H */
