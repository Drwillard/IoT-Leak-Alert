#include "water_logic.h"

void water_logic_sample(water_logic_t *s, int raw, int dry, int wet, int64_t now_ms)
{
    if (raw < 0) { s->tracking = false; return; }
    bool target = s->wet;
    if (raw >= wet) target = true;
    else if (raw <= dry) target = false;
    if (target == s->wet) { s->tracking = false; return; }
    if (!s->tracking || s->candidate != target) {
        s->tracking = true;
        s->candidate = target;
        s->since_ms = now_ms;
    }
    if (now_ms - s->since_ms >= (target ? 2000 : 5000)) {
        s->wet = target;
        s->tracking = false;
        if (target) s->generation++;
    }
}

bool water_logic_due(const water_logic_t *s, uint32_t acknowledged, int64_t now, int64_t next)
{
    return now >= next && (s->wet || s->generation != acknowledged);
}
