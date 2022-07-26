/**
 * @file
 * HTTP server
 */

/*
 * Copyright (c) 2001-2003 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 * This version of the file has been modified by Texas Instruments to offer
 * simple server-side-include (SSI) and Common Gateway Interface (CGI)
 * capability.
 */

/*
 * 2022-25-07: Modified by Terje Io for grblHAL networking
 */

#ifndef _HTTPD_H
#define _HTTPD_H

#include "lwip/init.h"
#include "lwip/altcp.h"
#if LWIP_ALTCP
#include "lwip/altcp_tcp.h"
#endif
#include "lwip/apps/fs.h"
#if HTTPD_ENABLE_HTTPS
#include "lwip/altcp_tls.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef LWIP_HTTPD_SUPPORT_POST
#undef LWIP_HTTPD_SUPPORT_POST
#endif
#define LWIP_HTTPD_SUPPORT_POST 1

typedef enum {
    HTTP_Get = 0,
    HTTP_Post,
    HTTP_Delete,
    HTTP_Options
} http_method_t;

typedef struct http_request {
    void *handle;
    void *private_data;
    err_t (*post_receive_data)(struct http_request *request, struct pbuf *p);
    void (*post_finished)(struct http_request *request, char *response_uri, u16_t response_uri_len);
    void (*on_request_completed)(void *private_data);
} http_request_t;

typedef const char *(*uri_handler_fn)(http_request_t *request);

typedef struct {
    const char *uri;
    http_method_t method;
    uri_handler_fn handler;
    void *private_data;
} httpd_uri_handler_t;

typedef struct {
    const char *name;
    size_t size;
    const uint8_t data[];
} embedded_file_t; // TODO: move to new vfs.c

uint8_t http_get_param_count (http_request_t *request);
const char *http_get_uri (http_request_t *request);
ip_addr_t http_get_remote_ip (http_request_t *request);
uint16_t http_get_remote_port (http_request_t *request);
char *http_get_param_value (http_request_t *request, const char *key, char *value, uint32_t size);
int http_get_header_value_len (http_request_t *hs, const char *name);
char *http_get_header_value (http_request_t *hs, const char *name, char *value, uint32_t size);
bool http_set_response_header (http_request_t *request, const char *name, const char *value);
void http_set_response_status (http_request_t *request, const char *status);
void httpd_register_uri_handlers (const httpd_uri_handler_t *httpd_uri_handlers, uint_fast8_t httpd_num_uri_handlers);
void httpd_free_pbuf (http_request_t *request, struct pbuf *p);


#if LWIP_HTTPD_POST_MANUAL_WND
void httpd_post_data_recved(void *connection, u16_t recved_len);
#endif /* LWIP_HTTPD_POST_MANUAL_WND */

bool httpd_init (uint16_t port);

#if HTTPD_ENABLE_HTTPS
struct altcp_tls_config;
void httpd_inits (struct altcp_tls_config *conf);
#endif

#ifdef __cplusplus
}
#endif

#endif /* LWIP_HDR_APPS_HTTPD_H */
