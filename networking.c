//
// networking.c - some shared networking code
//
// v1.7 / 2023-12-25 / Io Engineering / Terje
//

/*

Copyright (c) 2021-2023, Terje Io
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its contributors may
be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "networking.h"

#if ETHERNET_ENABLE || WIFI_ENABLE

#include <stdio.h>
#include <string.h>

// NOTE: increase #define NETWORK_SERVICES_LEN in networking.h when adding to this array!
PROGMEM static char const *const service_names[] = {
    "Telnet,",
    "Websocket,",
    "HTTP,",
    "FTP,",
    "DNS,",
    "mDNS,",
    "SSDP,",
    "WebDAV,"
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
    .http = 1,
  #if WEBDAV_ENABLE
    .webdav = 1,
  #endif
#endif
#if DNS_ENABLE
    .dns = 1,
#endif
#if MDNS_ENABLE
    .mdns = 1,
#endif
#if SSDP_ENABLE
    .ssdp = 1,
#endif
};

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif

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

bool networking_ismemnull (void *data, size_t len)
{
    uint8_t *p = data;

    do {
        if(*p++ != 0)
            return false;
    } while(--len);

    return true;
}

char *networking_mac_to_string (uint8_t *mac)
{
    static char s[18];

    if(networking_ismemnull(mac, 6))
        *s = '\0';
    else
        sprintf(s, MAC_FORMAT_STRING, mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);

    return s;
}

#if MQTT_ENABLE

// Create MQTT client id from last three values of MAC address
void networking_make_mqtt_clientid (const char *mac, char *client_id)
{
    if(*mac) {

        char c, *s1, *s2 = (char *)mac + 9;

        strcpy(client_id, "grblHAL.");
        s1 = strchr(client_id, '\0');

        while((c = *s2++)) {
            if(c != ':')
                *s1++ = c;
        }
        *s1 = '\0';
    } else
        strcpy(client_id, "grblHAL");
}

#endif

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#endif // ETHERNET_ENABLE || WIFI_ENABLE
