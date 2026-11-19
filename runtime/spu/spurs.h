#ifndef SPURS_H
#define SPURS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void spurs_register(uint32_t service_pm, uint32_t taskset_pm, uint32_t jobchain_pm, uint32_t jobchain_plain_pm);

#ifdef __cplusplus
}
#endif

#endif
