/* timers_wasm.cc -- WASM stub for the timer subsystem.
 * POSIX signals (SIGALRM, SIGVTALRM) are unavailable in WASM/Emscripten.
 * All timer operations are no-ops or return dummy values.
 */

#include "timers.h"

static Timer_ID next_id = 1;

Timer_ID
set_timer(unsigned seconds, Timer_Proc callback, Timer_Data data)
{
    /* In WASM we cannot schedule signal-based timers.
     * Return a dummy ID. The callback will never fire. */
    return next_id++;
}

Timer_ID
set_virtual_timer(unsigned seconds, Timer_Proc callback, Timer_Data data)
{
    return set_timer(seconds, callback, data);
}

int
cancel_timer(Timer_ID id)
{
    return 0;
}

void
reenable_timers(void)
{
    /* no-op */
}

unsigned
timer_wakeup_interval(Timer_ID id)
{
    return 0;
}

void
timer_sleep(unsigned seconds)
{
    /* no-op -- could call emscripten_sleep() in the future */
}

int
virtual_timer_available()
{
    return 0;
}
