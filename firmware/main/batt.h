#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void  batt_init(void);
float batt_volts(void);      /* pack voltage, 0 if the read fails */
int   batt_percent(void);    /* 0..100, clamped Li-ion 3.30-4.20 V curve */
#ifdef __cplusplus
}
#endif
