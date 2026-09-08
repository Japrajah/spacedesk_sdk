#include "connection_timers.h"
#include <windows.h>
namespace sd::viewer {
uint64_t ConnectionUptime() {
    ULONGLONG value=0;
    // Android Handler uses uptime, unlike the watcher's Unix wall clock.
    return QueryUnbiasedInterruptTime(&value)?value/10000:GetTickCount64();
}
}
