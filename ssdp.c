//
// ssdp.c - Simple Service Discovery Protocol
//
// v0.1 / 2022-10-19 / Io Engineering / Terje
//

/*

Copyright (c) 2022, Terje Io
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

#include "driver.h"

#if SSDP_ENABLE && HTTP_ENABLE

#include "lwip/netif.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#include "lwip/timeouts.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "strutils.h"
#include "networking.h"
#include "ssdp.h"

#if (LWIP_IPV4 && !LWIP_IGMP)
#error "If you want to use SSDP with IPv4, you have to define LWIP_IGMP=1 in your lwipopts.h"
#endif
#if (!LWIP_UDP)
#error "If you want to use SSDP, you have to define LWIP_UDP=1 in your lwipopts.h"
#endif

#if LWIP_IPV4
#include "lwip/igmp.h"
static const ip_addr_t v4group = IPADDR4_INIT_BYTES(239, 255, 255, 250);
#endif

#if LWIP_IPV6
#include "lwip/mld6.h"
static const ip_addr_t v6group = IPADDR6_INIT_HOST(0xFF020000, 0, 0, 0x0C);
#endif

#define CRLF "\r\n"
#define SSDP_TTL 2
#define SSDP_MAX_AGE 1800 // seconds
#define SSDP_ADVERTISE_INTERVAL (SSDP_MAX_AGE / 2)
#define SSDP_DEVICE_TYPE "urn:io-engineering-com:grblHAL:1" // Max. 40 characters!

typedef struct {
    char *host;
    char *st;
    char *mx;
    char *man;
    int mx_max;
    char hdr[400];
} ssdp_msg_t;

typedef struct {
    ip_addr_t addr;
    u16_t port;
    char st[41];
} ssdp_request_t;

typedef enum {
    SSDP_Up = 0,
    SSDP_Down,
    SSDP_SearchReply
} ssdp_response_t;

PROGMEM static const char *ssdp_notify_hdr =
    "NOTIFY * HTTP/1.1" CRLF
    "HOST: 239.255.255.250:1900" CRLF
    "CACHE-CONTROL: max-age=%u" CRLF
    "NTS: %s" CRLF;
PROGMEM static const char *ssdp_search_hdr =
    "HTTP/1.1 200 OK" CRLF
    "CACHE-CONTROL: max-age=%u" CRLF
    "EXT:" CRLF;
PROGMEM static const char *ssdp_common_hdr =
    "SERVER: lwIP/1.0 UPNP/1.1 %s/%s" CRLF
    "USN: uuid:%s::%s" CRLF
    "%s: %s" CRLF;
PROGMEM static const char *ssdp_location_hdr =
    "LOCATION: http://%s/" SSDP_LOCATION_DOC CRLF CRLF;
PROGMEM static const char *xml_doc =
    "<?xml version=\"1.0\"?>"
    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
    "<specVersion>"
    "<major>1</major>"
    "<minor>0</minor>"
    "</specVersion>"
    "<device>"
    "<deviceType>" SSDP_DEVICE_TYPE "</deviceType>"
    "<friendlyName>%s</friendlyName>"
    "<manufacturer>grblHAL</manufacturer>"
    "<manufacturerURL>%s</manufacturerURL>"
    "<modelDescription>%s</modelDescription>"
    "<modelName>%s</modelName>"
    "<modelNumber>%s (%s)</modelNumber>"
    "<modelURL>%s</modelURL>"
    "<serialNumber>%d</serialNumber>"
    "<UDN>uuid:%s</UDN>"
    "<presentationURL>/</presentationURL>"
    "</device>"
    "</root>";

static char uuid[37], location[22] = "";
static ssdp_msg_t request;
static struct udp_pcb *ssdp_pcb = NULL;

const char *ssdp_handler_get (http_request_t *request)
{
    char xml[800];
    vfs_file_t *file = NULL;
    network_info_t *network = networking_get_info();
    char *mfg_url = hal.driver_url &&  hal.board_url ? hal.driver_url : GRBL_URL,
         *model_url = hal.board_url ? hal.board_url : (hal.driver_url ? hal.driver_url : GRBL_URL);

    if(*location && (file = vfs_open("/ram/qry.xml", "w"))) {

        sprintf(xml, xml_doc, network->status.hostname, mfg_url, hal.info, hal.board ? hal.board : "", GRBL_VERSION, hal.info, model_url, GRBL_BUILD, uuid);

        vfs_puts(xml, file);
        vfs_close(file);
    }

    return file ? "/ram/qry.xml" : NULL;
}

static void ssdp_send (ssdp_response_t response, const ip_addr_t *addr, u16_t port, char *st)
{
    struct pbuf *p;
    char msg[500], ntst[3], *add;

    if(response == SSDP_SearchReply)
        add = msg + sprintf(msg, ssdp_search_hdr, SSDP_MAX_AGE);
    else
        add = msg + sprintf(msg, ssdp_notify_hdr, SSDP_MAX_AGE, response == SSDP_Down ? "ssdp:byebye" : "ssdp:alive");

    strcpy(ntst, response == SSDP_SearchReply ? "ST" : "NT");

    add += sprintf(add, ssdp_common_hdr, "grblHAL", GRBL_VERSION, uuid, st, ntst, st);

    if(response == SSDP_Down)
        strcpy(add, CRLF);
    else
        sprintf(add, ssdp_location_hdr, location);

    if((p = pbuf_alloc(PBUF_TRANSPORT, 0, PBUF_REF)))
    {
        p->payload = (void *)msg;
        p->len = p->tot_len = strlen(msg);
        if(response == SSDP_SearchReply)
            udp_sendto(ssdp_pcb, p, addr, port);
        else
            udp_sendto(ssdp_pcb, p, &v4group, 1900);

        p->payload = NULL;
        pbuf_free(p);
    }
}

static void ssdp_advertise_root (void *arg);
/*
static void ssdp_advertise_device (void *arg)
{
    ssdp_send(SSDP_Up, NULL, 0, SSDP_DEVICE_TYPE);
#if SSDP_ADVERTISE_INTERVAL
    sys_timeout(SSDP_ADVERTISE_INTERVAL * 1000UL, ssdp_advertise_root, NULL);
#endif
}
*/
static void ssdp_advertise_root (void *arg)
{
    ssdp_send(SSDP_Up, NULL, 0, "upnp:rootdevice");
#if SSDP_ADVERTISE_INTERVAL
    sys_timeout(SSDP_ADVERTISE_INTERVAL * 1000UL, ssdp_advertise_root, NULL);
    //    sys_timeout(100, ssdp_advertise_device, NULL);
#endif
}

static void ssdp_reply (void *arg)
{
    ssdp_request_t *req = (ssdp_request_t *)arg;

    ssdp_send(SSDP_SearchReply, &req->addr, req->port, req->st);
    free(arg);
}

static void ssdp_recv (void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    u16_t hdr_len;
    char *start, *end, *argp;
    bool unicast = false;

    memset(&request, 0, sizeof(ssdp_msg_t));
    hdr_len = pbuf_copy_partial(p, request.hdr, sizeof(request.hdr) - 1, 0);
    pbuf_free(p);

    if(strncmp(request.hdr, "M-SEARCH ", 9))
        return;

#if LWIP_IPV4
    if (!IP_IS_V6(ip_current_dest_addr()))
        unicast = !ip_addr_cmp(ip_current_dest_addr(), &v4group);
#endif
#if LWIP_IPV6
    if (IP_IS_V6(ip_current_dest_addr()))
        unicast = !ip_addr_cmp_zoneless(ip_current_dest_addr(), &v6group);
#endif

//hal.stream.write(request.hdr);
    if((start = lwip_strnstr(request.hdr, CRLF, hdr_len))) {

        start += 2;

        while((end = lwip_strnstr(start, CRLF, hdr_len))) {

            *end = '\0';

            if((argp = strchr(start, ':'))) {

                *argp++ = '\0';

                while(*argp == ' ')
                    *argp++ = '\0';

                switch(strlookup(strcaps(start), "HOST,MAN,MX,ST", ',')) {

                    case 0:
                        request.host = argp;
                        break;

                    case 1:
                        if(!strcmp(argp, "\"ssdp:discover\""))
                            request.man = argp;
                        break;

                    case 2:
                        request.mx = argp;
                        request.mx_max = atoi(request.mx) * 1000; // ms
                        if(request.mx_max == 0 || (unicast && request.mx_max > 1000))
                            request.mx_max = 1000;
                        break;

                    case 3:
                        request.st = argp;
                        break;
                }
            }
            start = end + 2;
        }

        if(request.man && request.mx && request.st) {

            bool all;

            if((all = strcmp(request.st, "ssdp:all") == 0) || strcmp(request.st, "upnp:rootdevice") == 0 || strcmp(request.st, SSDP_DEVICE_TYPE) == 0) {
                ssdp_request_t *req;
                if((req = malloc(sizeof(ssdp_request_t)))) {
                    SMEMCPY(&req->addr, addr, sizeof(req->addr));
                    req->port = port;
                    strcpy(req->st, all ? "upnp:rootdevice" : request.st);
                    request.mx_max = rand() % (request.mx_max - 100);
                    sys_timeout(max((u32_t)request.mx_max, 20), ssdp_reply, req);
                }
            }
        }
    }
}

void ssdp_stop (void)
{
    if(ssdp_pcb) {

        struct netif *netif = netif_default; // netif_get_by_index(0);

        sys_untimeout(ssdp_advertise_root, NULL);

        ssdp_send(SSDP_Down, NULL, 0, "upnp:rootdevice");

        udp_remove(ssdp_pcb);
        ssdp_pcb = NULL;

#if LWIP_IPV4
        igmp_leavegroup_netif(netif, ip_2_ip4(&v4group));
#endif
#if LWIP_IPV6
        mld6_leavegroup_netif(netif, ip_2_ip6(&v6group));
#endif

    }
}

bool ssdp_init (uint16_t httpd_port)
{
    err_t res = ERR_MEM;
    struct netif *netif = netif_default; // netif_get_by_index(0);

    if((ssdp_pcb = udp_new_ip_type(IPADDR_TYPE_ANY))) {

#if LWIP_MULTICAST_TX_OPTIONS
        udp_set_multicast_ttl(ssdp_pcb, SSDP_TTL);
#else
        ssdp_pcb->ttl = SSDP_TTL;
#endif

        if((res = udp_bind(ssdp_pcb, IP_ANY_TYPE, 1900)) == ERR_OK) {

            udp_recv(ssdp_pcb, ssdp_recv, NULL);

#if LWIP_IPV4
            res = igmp_joingroup_netif(netif, ip_2_ip4(&v4group));
#endif
#if LWIP_IPV6
            if(res == ERR_OK)
                res = mld6_joingroup_netif(netif, ip_2_ip6(&v6group));
#endif

            if(res == ERR_OK) {

                network_info_t *network = networking_get_info();

                srand(hal.get_elapsed_ticks());
                sprintf(location, "%s:%d", network->status.ip, httpd_port);
                sprintf(uuid, "06945d64-43bc-11ed-b878-0242%02x%02x%02x%02x", netif->hwaddr[2], netif->hwaddr[3], netif->hwaddr[4], netif->hwaddr[5]);

                sys_timeout(5 * 1000UL, ssdp_advertise_root, NULL);
            }
        }
    }

    return res == ERR_OK;
}

#endif
