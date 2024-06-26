#include "driver.h"

#include "grbl/platform.h"

#if defined(_WIZCHIP_) && !(defined(RP2040) || defined(STM32_H7_PLATFORM))

#include "grbl/hal.h"

/* Returns the current time in mS. This is needed for the LWIP timers */
uint32_t sys_now (void)
{
    return hal.get_elapsed_ticks();
}

#endif
