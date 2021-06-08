#include <string.h>

#include "networking.h"

PROGMEM static char const *const service_names[] = {
    "Telnet,",
    "Websocket,",
    "HTTP,",
    "FTP,",
    "DNS,"
};

PROGMEM static const network_services_t allowed_services = {
#if TELNET_ENABLE
    .telnet = 1,
#endif
#if WEBSOCKET_ENABLE
    .websocket = 1,
#endif
#if FTP_ENABLE && SDCARD_ENABLE
    .ftp = 1,
#endif
#if HTTP_ENABLE
    .http = 1
#endif
#if DNS_ENABLE
    .dns = 1
#endif
};

network_services_t networking_get_services_list (char *list)
{
    uint_fast8_t idx = 0;
    network_services_t services = {allowed_services.mask};

    while(services.mask) {
        if(services.mask == 1)
            strncat(list, service_names[idx], strlen(service_names[idx]) - 1);
        else
            strcat(list, services.mask & 0x1 ? service_names[idx] : "N/A,");
        idx++;
        services.mask >>= 1;
    }

    return *list != '\0' ? allowed_services : (network_services_t){0};
}
