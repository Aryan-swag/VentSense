/*
 * Editable settings (smart feature P1.7): one table of the vs_config_t fields the dashboard may change,
 * each with hard limits. SPEC values can only be made stricter (heater cutoff never above 55 C, heater
 * duty never above 50 %, correlation window kept inside 60-120 s).
 */
#ifndef VS_CONFIG_TABLE_H
#define VS_CONFIG_TABLE_H

#include <stddef.h>

#include "safety_core.h"

#ifdef __cplusplus
extern "C" {
#endif

int vs_config_count(void);
const char *vs_config_key(int i);
/* 1 = set; 0 = rejected (unknown key or out of range), reason written to err */
int vs_config_set(vs_config_t *c, const char *key, float value, char *err, size_t n);
int vs_config_get(const vs_config_t *c, const char *key, float *value);
/* {"window_s":{"v":90,"min":60,"max":120,"unit":"s","label":"..."}, ...}; returns length written */
size_t vs_config_json(const vs_config_t *c, char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif
