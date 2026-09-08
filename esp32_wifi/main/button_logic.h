#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool candidate, stable, released;
    int64_t since_ms;
} button_logic_t;

// Require a debounced release after boot, then one event per debounced press.
static inline bool button_sample(button_logic_t *s, bool down, int64_t now_ms)
{
    if (down != s->candidate) {
        s->candidate = down;
        s->since_ms = now_ms;
    }
    if (now_ms - s->since_ms < 60) return false;
    if (!down) s->released = true;
    if (s->stable == down) return false;
    s->stable = down;
    if (down && s->released) { s->released = false; return true; }
    return false;
}
