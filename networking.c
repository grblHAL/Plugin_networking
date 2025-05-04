//
// networking.c - some shared networking code
//
// v1.9 / 2025-02-20 / Io Engineering / Terje
//

/*

Copyright (c) 2021-2025, Terje Io
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

typedef struct net_if {
    struct net_if *next;
    const char *name;
    network_flags_t status;
} net_if_t;

static net_if_t net_if = {};

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
#if FTP_ENABLE && FS_ENABLE
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

static void network_event (const char *interface, network_status_t status);

static network_info_t *get_info (const char *interface)
{
    return NULL;
}

networking_t networking = {
    .event = network_event,
    .get_info = get_info
};

static void network_event (const char *interface, network_status_t status)
{
    net_if_t *intf = &net_if;

    if(intf->name == NULL)
        intf->name = interface;

    do {
        if(intf->name == interface)
            break;
        if(intf->next == NULL) {
            if((intf->next = calloc(sizeof(net_if_t), 1))) {
                intf = intf->next;
                intf->name = interface;
                break;
            }
        }
    } while((intf = intf->next));

    if(intf == NULL)
        return;

//    if(!ntf->status.value)
//        report_message(interface, Message_Plain);

    if(status.changed.ap_started && status.flags.ap_started)
        report_message("WIFI AP READY", Message_Plain);

    if(status.changed.ip_aquired) {

        network_info_t *info;

        if((info = networking.get_info(interface))) {

            char buf[30];

            if(info->is_ethernet) {
                sprintf(buf, "ETHERNET IP=%s", info->status.ip);
            } else
                sprintf(buf, status.flags.ap_started ? "WIFI AP IP=%s" : "WIFI STA IP=%s", info->status.ip);

            report_message(buf, Message_Plain);
        }
    }

    if(status.changed.ap_scan_completed && status.flags.ap_scan_completed)
        report_message("WIFI AP SCAN COMPLETED", Message_Plain);

    intf->status.value = status.flags.value;
}

bool networking_enumerate_interfaces (networking_enumerate_interfaces_callback_ptr callback, void *data)
{
    bool ok = false;
    net_if_t *intf = &net_if;

    if(intf->name) do {
        network_info_t *info;
        if((info = networking.get_info(intf->name)))
            ok = callback(info, intf->status, data);
    } while(!ok && (intf = intf->next));

    return ok;
}

static inline bool add_port (char *buf, uint16_t port, bool add_sep)
{
    if(add_sep)
        strcat(buf, ",");

    strcat(buf, uitoa(port));

    return true;
}

static bool if_enumerate (network_info_t *info, network_flags_t flags, void *data)
{
    if(flags.interface_up) {

        char buf[100];
        bool add_sep = false;

        sprintf(buf, "IF=%s IP=%s MAC=%s", info->interface, info->status.ip, info->mac);

        if(info->status.services.mask) {
            strcat(buf, " Listening=");
            if(info->status.services.ftp)
                add_sep = add_port(buf, info->status.ftp_port, add_sep);
            if(info->status.services.telnet)
                add_sep = add_port(buf, info->status.telnet_port, add_sep);
            if(info->status.services.http)
                add_sep = add_port(buf, info->status.http_port, add_sep);
            if(info->status.services.websocket)
                add_sep = add_port(buf, info->status.websocket_port, add_sep);
        }
        report_message(buf, Message_Plain);
    }

    return false;
}

static status_code_t netif (sys_state_t state, char *args)
{
    networking_enumerate_interfaces(if_enumerate, NULL);

    return Status_OK;
}

void networking_init (void)
{
    static bool ok = false;

    static const sys_command_t net_command_list[] = {
        {"NETIF", netif, { .allow_blocking = On, .noargs = On }, { .str = "provides information about network interfaces" } }
    };

    static sys_commands_t net_commands = {
        .n_commands = sizeof(net_command_list) / sizeof(sys_command_t),
        .commands = net_command_list
    };

    if(!ok) {
        ok = true;
        system_register_commands(&net_commands);
    }
}

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

char *networking_mac_to_string (uint8_t mac[6])
{
    static char s[18];

    if(networking_ismemnull(mac, 6))
        *s = '\0';
    else
        sprintf(s, MAC_FORMAT_STRING, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return s;
}

bool networking_string_to_mac (char *s, uint8_t mac[6])
{
    if(*s) {

        uint bmac[6];

        if(sscanf(s,"%2X:%2X:%2X:%2X:%2X:%2X", &bmac[0], &bmac[1], &bmac[2], &bmac[3], &bmac[4], &bmac[5]) == 6) {

            char c = LCAPS(s[strlen(s) - 1]);
            if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;

            uint_fast8_t idx;
            for(idx = 0; idx < 6; idx++)
                mac[idx] = (uint8_t)bmac[idx];
        } else
            return false;
    } else
        memset(mac, 0, 6);

    return true;
}

// Returns default MAC address for $535 (Setting_NetworkMAC)

__attribute__((weak)) bool bmac_eth_get (uint8_t mac[6])
{
    memset(mac, 0, 6);

    return false;
}

__attribute__((weak)) bool bmac_wifi_get (uint8_t mac[6])
{
    memset(mac, 0, 6);

    return false;
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
