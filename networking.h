//
// networking.h - some shared networking code
//
// v2.0 / 2025-04-30 / Io Engineering / Terje
//

/*

Copyright (c) 2019-2025, Terje Io
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

#pragma once

#include "driver.h"

#if ETHERNET_ENABLE || WIFI_ENABLE

//*****************************************************************************
//
// lwIP Options
//
//*****************************************************************************

#include "lwipopts.h"

// If no OS increase TX buffer size to hold the largest message generated and then some.
// The list settings $$ command is currently the big one.
#if NO_SYS > 0 && !defined(TX_BUFFER_SIZE)
#define TX_BUFFER_SIZE 1024 // must be a power of 2
#endif

#define SOCKET_TIMEOUT 0
#ifndef TCP_SLOW_INTERVAL
#define TCP_SLOW_INTERVAL 500
#endif

//*****************************************************************************

#ifndef LINK_CHECK_INTERVAL
#define LINK_CHECK_INTERVAL 200
#endif

#if TELNET_ENABLE
#include "telnetd.h"
#endif

#if WEBSOCKET_ENABLE
#include "websocketd.h"
#endif

#if FTP_ENABLE
#include "ftpd.h"
#endif

#if MDNS_ENABLE
#include "lwip/apps/mdns.h"
#endif

#if MQTT_ENABLE
#include "lwip/apps/mqtt.h"
#include "./mqtt.h"
#endif

#if HTTP_ENABLE
#include "httpd.h"
#if WEBDAV_ENABLE
#include "webdav.h"
#endif

#if SSDP_ENABLE
#include "networking/ssdp.h"
#endif
#endif

#if MODBUS_ENABLE & MODBUS_TCP_ENABLED
#include "modbus/client.h"
#endif

//*****************************************************************************
//
// Ensure that AUTOIP COOP option is configured correctly.
//
//*****************************************************************************
#undef LWIP_DHCP_AUTOIP_COOP
#define LWIP_DHCP_AUTOIP_COOP   ((LWIP_DHCP) && (LWIP_AUTOIP))

//*****************************************************************************
//
// lwIP API Header Files
//
//*****************************************************************************
#include <stdint.h>
#include "lwip/api.h"
#include "lwip/netifapi.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include "lwip/sockets.h"
#include "lwip/mem.h"
#include "lwip/stats.h"
#include "lwip/def.h"
#include "lwip/ip_addr.h"

#if NO_SYS
#include "lwip/sys.h"
typedef uint32_t TickType_t;
#define configTICK_RATE_HZ 1000
#define xTaskGetTickCount() sys_now()
#define portMUX_TYPE void*
#define SemaphoreHandle_t void*
#define portMUX_INITIALIZER_UNLOCKED NULL
#define taskENTER_CRITICAL()
#define taskEXIT_CRITICAL()
#define pdTRUE true
#define portMAX_DELAY 0
#define xSemaphoreCreateMutex() ((void *)1)
#define xSemaphoreTake(mutex, delay) pdTRUE
#define xSemaphoreGive(mutex)
#endif

#ifndef SYS_ARCH_PROTECT
#define lev 1
#define SYS_ARCH_PROTECT(lev)
#define SYS_ARCH_UNPROTECT(lev)
#define SYS_ARCH_DECL_PROTECT(lev)
#endif

#define NETWORK_SERVICES_LEN 50
#define MAC_FORMAT_STRING "%02x:%02x:%02x:%02x:%02x:%02x"

typedef struct
{
    uint16_t port;
    bool link_lost;
    struct tcp_pcb *pcb;
} tcp_server_t;

#pragma pack(push, 1)

typedef union {
    uint16_t value;
    struct {
        uint16_t interface_up      :1,
                 link_up           :1,
                 ip_aquired        :1,
                 ap_started        :1,
                 ap_scan_completed :1,
                 unassigned        :11;
    };
} network_flags_t;

typedef union {
    uint32_t value;
    struct {
        network_flags_t changed;
        network_flags_t flags;
    };
} network_status_t;

#pragma pack(pop)

typedef void (*on_network_event_ptr)(const char *interface, network_status_t status);
typedef network_info_t *(*networking_get_info)(const char *interface);
typedef bool networking_enumerate_interfaces_callback_ptr (network_info_t *info, network_flags_t flags, void *data);

typedef struct {
    on_network_event_ptr event;
    networking_get_info get_info;
} networking_t;

void networking_init (void);
bool networking_ismemnull (void *data, size_t len);
char *networking_mac_to_string (uint8_t mac[6]);
bool networking_string_to_mac (char *s, uint8_t mac[6]);
bool bmac_eth_get (uint8_t mac[6]);
bool bmac_wifi_get (uint8_t mac[6]);
network_services_t networking_get_services_list (char *list);
bool networking_enumerate_interfaces (networking_enumerate_interfaces_callback_ptr callback, void *data);
#if MQTT_ENABLE
void networking_make_mqtt_clientid (const char *mac, char *client_id);
#endif

extern networking_t networking;

#endif // ETHERNET_ENABLE || WIFI_ENABLE
