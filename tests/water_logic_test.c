#include <assert.h>
#include <stdio.h>
#include "water_logic.h"

int main(void)
{
    water_logic_t s = {0};
    water_logic_sample(&s, 900, 300, 600, 0);
    water_logic_sample(&s, 100, 300, 600, 1000);
    assert(!s.wet && s.generation == 0); // A spike must not trigger.
    water_logic_sample(&s, 900, 300, 600, 2000);
    water_logic_sample(&s, 900, 300, 600, 3999);
    assert(!s.wet);
    water_logic_sample(&s, 900, 300, 600, 4000);
    assert(s.wet && s.generation == 1);
    assert(water_logic_due(&s, 0, 4000, 0));
    assert(!water_logic_due(&s, 0, 4000, 60000)); // Cooldown applies even before acknowledgement.
    water_logic_sample(&s, 450, 300, 600, 5000);
    assert(s.wet); // Hysteresis preserves state in the middle band.
    water_logic_sample(&s, 100, 300, 600, 6000);
    water_logic_sample(&s, 100, 300, 600, 10999);
    assert(s.wet);
    water_logic_sample(&s, 100, 300, 600, 11000);
    assert(!s.wet);
    assert(water_logic_due(&s, 0, 60000, 60000)); // Delayed event survives drying/offline time.
    assert(!water_logic_due(&s, 1, 60000, 60000));
    water_logic_sample(&s, 900, 300, 600, 12000);
    water_logic_sample(&s, 900, 300, 600, 14000);
    assert(s.generation == 2);
    assert(!water_logic_due(&s, 1, 14000, 60000)); // Dry/wet cycles never bypass cooldown.
    assert(water_logic_due(&s, 2, 60000, 60000)); // Reminder while still wet.
    water_logic_sample(&s, 100, 300, 600, 15000);
    water_logic_sample(&s, -1, 300, 600, 18000);
    water_logic_sample(&s, 100, 300, 600, 20000);
    assert(s.wet); // A failed read interrupts confirmation, not the alarm state.
    puts("Water detection, hysteresis, pending events, and throttle tests passed.");
}
