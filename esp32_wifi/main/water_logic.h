#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool wet;
    bool candidate;
    bool tracking;
    int64_t since_ms;
    uint32_t generation;
} water_logic_t;

void water_logic_sample(water_logic_t *s, int raw, int dry, int wet, int64_t now_ms);
bool water_logic_due(const water_logic_t *s, uint32_t acknowledged, int64_t now, int64_t next);
