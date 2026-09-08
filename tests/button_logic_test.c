#include <assert.h>
#include "button_logic.h"
int main(void)
{
    button_logic_t b = {0};
    assert(!button_sample(&b, true, 0));
    assert(!button_sample(&b, true, 100)); // held at boot never sends
    assert(!button_sample(&b, false, 200));
    assert(!button_sample(&b, false, 270));
    assert(!button_sample(&b, true, 300));
    assert(!button_sample(&b, false, 320)); // bounce
    assert(!button_sample(&b, true, 330));
    assert(!button_sample(&b, true, 380));
    assert(button_sample(&b, true, 390));
    assert(!button_sample(&b, true, 20000)); // no repeat while held
    assert(!button_sample(&b, false, 20100));
    assert(!button_sample(&b, true, 20120)); // release bounce cannot re-arm
    assert(!button_sample(&b, true, 20200));
    assert(!button_sample(&b, false, 20300));
    assert(!button_sample(&b, false, 20360));
    assert(!button_sample(&b, true, 20400));
    assert(button_sample(&b, true, 20460));
    return 0;
}
