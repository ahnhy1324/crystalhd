// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef CRYSTALHD_COUPLED_FILTER_BRIDGE_H
#define CRYSTALHD_COUPLED_FILTER_BRIDGE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct CoupledFrame {
  void *owner;
  uint8_t *planes[2];
  int pitches[2],width,height,flags;
  int64_t pts,best;
};
int coupled_original_oracle(int argc,char **argv);
void coupled_reset(void);
struct CoupledFrame coupled_input(unsigned pattern,int64_t pts,int bottom_first);
int coupled_add(struct CoupledFrame *frame);
int coupled_get(struct CoupledFrame *frame);
void coupled_release(void);
void coupled_close(void);
#ifdef __cplusplus
}
#endif
#endif
