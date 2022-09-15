//
// webdav.c - WebDAV plugin for lwIP "raw" http daemon
//
// v0.1 / 2022-08-28 / Io Engineering / Terje
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

#ifdef ARDUINO
#include "../driver.h"
#else
#include "driver.h"
#endif

#if WEBDAV_ENABLE && HTTP_ENABLE

#include <stdio.h>

#ifdef ARDUINO
#include "../grbl/hal.h"
#include "../grbl/vfs.h"
#else
#include "grbl/hal.h"
#include "grbl/vfs.h"
#endif

#include "httpd.h"
#include "strutils.h"
#include "urlencode.h"
#include "urldecode.h"
#include "fs_ram.h"

typedef enum {
    Resource_NotExist = 0,
    Resource_Directory,
    Resource_File
} http_resource_t;

typedef struct {
    u32_t content_len;
    int depth;
    char uri[100];
    http_resource_t type;
    vfs_file_t *vfsh;
    char *rcvptr;
    char payload[];
} webdav_data_t;

static void dav_request_completed (void *data)
{
    webdav_data_t *dav = (webdav_data_t *)data;

    if(dav->vfsh)
        vfs_close(dav->vfsh);

    free(data);
}

static err_t dav_receive_payload (http_request_t *request, struct pbuf *p)
{
    struct pbuf *q = p;
    webdav_data_t *dav = (webdav_data_t *)request->private_data;

    if(dav) {
        memcpy(dav->rcvptr, p->payload, p->len);
        dav->rcvptr += p->len;
        while((q = q->next)) {
            memcpy(dav->rcvptr, p->payload, p->len);
            dav->rcvptr += p->len;
        }
    }

    httpd_free_pbuf(request, p);

    return ERR_OK;
}

static err_t dav_init_request (http_request_t *request, http_method_t method, char *uri)
{
    webdav_data_t *dav;
    char *value = NULL;
    int content_len = 0, vlen = http_get_header_value_len(request, "Content-Length");

    if(vlen > 0 && (value = malloc(vlen + 1))) {

        http_get_header_value(request, "Content-Length", value, vlen);
        content_len = atoi(value); // use strtol? https://www.cplusplus.com/reference/cstdlib/strtol/
        if (content_len == 0) {
            /* if atoi returns 0 on error, fix this */
            if ((value[0] != '0') || (value[1] != '\r'))
                content_len = 0;
        }

        free(value);
    }

    if((request->private_data = dav = malloc(sizeof(webdav_data_t) + (method == HTTP_Put ? 0 : content_len))) == NULL)
        return ERR_MEM;

    dav->depth = -1;
    dav->content_len = content_len;
    dav->type = Resource_NotExist;
    dav->vfsh = NULL;
    dav->rcvptr = dav->payload;
    strcpy(dav->uri, uri);

    /* If URI contains parameters NULL-terminate the base URI */
    if ((value = strchr(uri, '?')))
        *value = '\0';

    *uri = '\0';

    vfs_stat_t st;

    if (vfs_stat(vfs_fixpath(dav->uri), &st) == 0)
        dav->type = st.st_mode.directory ? Resource_Directory : Resource_File;

    request->on_request_completed = dav_request_completed;

    if((vlen = http_get_header_value_len(request, "Depth")) > 0 && (value = malloc(vlen + 1))) {
        http_get_header_value(request, "Depth", value, vlen);
        dav->depth = strcmp(value, "infinity") ? atoi(value) : -1;

        free(value);
    }

    return ERR_OK;
}

static void propfind_add_properties (char *fname, u32_t size, struct tm *created, struct tm *modified, bool is_dir, vfs_file_t *file)
{
    char buffer[LWIP_HTTPD_MAX_REQUEST_URI_LEN + 1];

    if(strlen(fname) > 1 && strrchr(fname, '/'))
        fname = strrchr(fname, '/') + 1;

    urlencode(fname, buffer, sizeof(buffer) - 1);

    vfs_puts("<D:response><D:href>", file);
    vfs_puts(buffer, file);
    vfs_puts("</D:href><D:propstat>", file);

    vfs_puts("<D:status>HTTP/1.1 200 OK</D:status><D:prop>", file);

    vfs_puts("<D:displayname>", file);
    vfs_puts(strcmp(fname, "/") ? fname : "root", file);
    vfs_puts("</D:displayname>", file);

    vfs_puts("<D:creationdate>", file);
    vfs_puts(strtointernetdt(created), file);
    vfs_puts("</D:creationdate>", file);

    vfs_puts("<D:getlastmodified>", file);
    vfs_puts(strtointernetdt(modified), file);
    vfs_puts("</D:getlastmodified>", file);

    if (!is_dir) {
        vfs_puts("<D:getcontentlength>", file);
        vfs_puts(uitoa(size), file);
        vfs_puts("</D:getcontentlength><D:getcontenttype>text/plain</D:getcontenttype><D:resourcetype/>", file);
    } else
        vfs_puts("<D:resourcetype><D:collection/></D:resourcetype>", file);

#if WEBDAV_ENABLE_LOCK

    vfs_puts("<D:supportedlock>");

    vfs_puts("<D:lockentry>");
    vfs_puts("<D:lockscope>");
    vfs_puts("<D:exclusive/>");
    vfs_puts("</D:lockscope>");
    vfs_puts("<D:locktype>");
    vfs_puts("<D:write/>");
    vfs_puts("</D:locktype>");
    vfs_puts("</D:lockentry>");

    vfs_puts("</D:supportedlock>");

#endif

    vfs_puts("</D:prop></D:propstat></D:response>", file);
}

static void propfind_scan (char *uri, int depth, vfs_file_t *file)
{
    char path[50];
    bool has_subdirs = false;

    vfs_stat_t st;
    vfs_dir_t *vfs_dir;
    vfs_dirent_t *dir_ent;
    struct tm *c_time, *m_time;
    time_t current_time = (time_t)-1;
#ifndef __IMXRT1062__
    time(&current_time);
#endif
    c_time = gmtime(&current_time);

    if((vfs_dir = vfs_opendir(uri))) {

        while((dir_ent = vfs_readdir(vfs_dir))) {

            strcpy(path, uri);
            if(strlen(uri) > 1 && uri[strlen(uri) - 1] != '/')
                strcat(path, "/");
            strcat(path, dir_ent->name);

            vfs_stat(path, &st);

            has_subdirs |= st.st_mode.directory;

            if(!st.st_mode.directory) {
#ifdef ESP_PLATFORM
                m_time = gmtime(&st.st_mtim);
#else
                m_time = gmtime(&st.st_mtime);
#endif
                propfind_add_properties(path, st.st_size, c_time, m_time, st.st_mode.directory, file);
            }
        }

        vfs_closedir(vfs_dir);
    }

    if(has_subdirs && (vfs_dir = vfs_opendir(uri))) {

        while((dir_ent = vfs_readdir(vfs_dir))) {

            strcpy(path, uri);
            if(strlen(uri) > 1 && uri[strlen(uri) - 1] != '/')
                strcat(path, "/");
            strcat(path, dir_ent->name);

            vfs_stat(path, &st);

            if(st.st_mode.directory) {

                propfind_add_properties(path, st.st_size, c_time, c_time, st.st_mode.directory, file);

                if(depth != 0)
                    propfind_scan(path, depth - 1, file);
            }
        }

        vfs_closedir(vfs_dir);
    }
}

static void propfind_receive_finished (http_request_t *request, char *response_uri, u16_t response_uri_len)
{
    vfs_stat_t st;
    webdav_data_t *dav = (webdav_data_t *)request->private_data;

    vfs_fixpath(dav->uri);

    dav->vfsh = vfs_open("/ram/data.xml", "w");

    http_set_response_status(request, "207 Multi-Status");

    vfs_puts("<?xml version=\"1.0\" encoding=\"utf-8\"?>", dav->vfsh);
    vfs_puts("<D:multistatus xmlns:D=\"DAV:\">", dav->vfsh);

    if(vfs_stat(dav->uri, &st) == 0 || !strcmp(dav->uri, "/")) {

        struct tm *c_time, *m_time;
        time_t current_time = (time_t)-1;
    #ifndef __IMXRT1062__
        time(&current_time);
    #endif
        c_time = gmtime(&current_time);

        if(!strcmp(dav->uri, "/")) {
            m_time = c_time;
            st.st_mode.directory = true;
        } else {
    #ifdef ESP_PLATFORM
            m_time = gmtime(&st.st_mtim);
    #else
            m_time = gmtime(&st.st_mtime);
    #endif
        }

        if(st.st_mode.directory) {

            if(dav->depth == 0)
                propfind_add_properties(dav->uri, 0, c_time, m_time, true, dav->vfsh);

            if(dav->depth != 0)
                propfind_scan(dav->uri, dav->depth - 1, dav->vfsh);

        } else
            propfind_add_properties(dav->uri, st.st_size, c_time, m_time, false, dav->vfsh);

    } else {
        char uri[LWIP_HTTPD_MAX_REQUEST_URI_LEN + 1];

        http_set_response_status(request, "404 Not found");
        urlencode(dav->uri, uri, LWIP_HTTPD_MAX_REQUEST_URI_LEN);
        vfs_puts("<D:response><D:href>", dav->vfsh);
        vfs_puts(uri, dav->vfsh);
        vfs_puts("</D:href><D:propstat><D:status>HTTP/1.1 404 Not found</D:status></D:propstat></D:response>", dav->vfsh);
    }

    vfs_puts("</D:multistatus>", dav->vfsh);

    vfs_close(dav->vfsh);
    dav->vfsh = NULL;

    strcpy(response_uri, "/ram/data.xml");
}

static void proppatch_receive_finished (http_request_t *request, char *response_uri, u16_t response_uri_len)
{
    webdav_data_t *dav = (webdav_data_t *)request->private_data;

    *dav->rcvptr = '\0';
/*
    hal.stream.write(ASCII_EOL);
    hal.stream.write(dav->payload);
    hal.stream.write(ASCII_EOL);
*/
    char *tstamp;

    if(dav->payload && (tstamp = strstr(dav->payload, "getlastmodified"))) {

        struct tm modified = {0};
        if((tstamp = strchr(tstamp, '>') + 1)) {
            if(strtotime(tstamp, &modified))
                vfs_utime(dav->uri, &modified);
        }
    }

    propfind_receive_finished(request, response_uri, response_uri_len);
}

static err_t put_receive_data (http_request_t *request, struct pbuf *p)
{
    struct pbuf *q = p;

    vfs_write(q->payload, 1, q->len, ((webdav_data_t *)request->private_data)->vfsh);

    while((q = q->next))
        vfs_write(q->payload, 1, q->len, ((webdav_data_t *)request->private_data)->vfsh);

    httpd_free_pbuf(request, p);

    return ERR_OK;
}

static void put_receive_finished (http_request_t *request, char *response_uri, u16_t response_uri_len)
{
    webdav_data_t *dav = (webdav_data_t *)request->private_data;

    vfs_close(dav->vfsh);
    dav->vfsh = NULL;

    if(dav->type == Resource_File)
        http_set_response_status(request, "200 OK");
    else
        http_set_response_status(request, "201 Created");

    *response_uri = '\0';
}

static err_t dav_process_request (http_request_t *request, http_method_t method, char *uri, u16_t uri_len)
{
    err_t ret = ERR_OK;

    switch(method) {

        case HTTP_Put:
// PUT on existing directory -> fail... 405 Method Not Allowed
            if((ret = dav_init_request(request, method, uri)) == ERR_OK) {

                webdav_data_t *dav = (webdav_data_t *)request->private_data;

                if((dav->vfsh = vfs_open(dav->uri, "w"))) {
                    if(dav->content_len) {
                        request->post_receive_data = put_receive_data;
                        request->post_finished = put_receive_finished;

                        return http_get_payload(request, dav->content_len);
                    } else {
                        vfs_close(dav->vfsh);
                        dav->vfsh = NULL;
                        if(dav->type == Resource_File)
                            http_set_response_status(request, "200 OK");
                        else
                            http_set_response_status(request, "201 Created");
                    }
                } else {
                    uri = "404.html";
                    http_set_response_status(request, "404 Not found");
                }
            }
            break;

        case HTTP_Move:
            if((ret = dav_init_request(request, method, uri)) == ERR_OK) {

                webdav_data_t *dav = (webdav_data_t *)request->private_data;

                if (dav->type == Resource_NotExist) {
                    uri = "404.html";
                } else {

                    char *destination = NULL, *host = NULL, *renameto;
                    int clen = http_get_header_value_len(request, "Destination");

                    if(clen > 0 && (destination = malloc(clen + 1))) {

                        http_get_header_value(request, "Destination", destination, clen + 1);
                        urldecode(destination, destination);

                        clen = http_get_header_value_len(request, "Host");
                        if(clen > 0 && (host = malloc(clen + 1))) {
                            http_get_header_value(request, "Host", host, clen + 1);
                            if((renameto = strstr(destination, host))) {
                                renameto += clen;
                                vfs_rename(dav->uri, renameto);
                            }
                        }
                    }

                    if(destination)
                        free(destination);
                    if(host)
                        free(host);
                }
            }
            break;

        case HTTP_Delete:
            if((ret = dav_init_request(request, method, uri)) == ERR_OK) {

                webdav_data_t *dav = (webdav_data_t *)request->private_data;

                if(dav->type == Resource_NotExist) {
                    uri = "404.html";
                } else {

                    if(dav->type == Resource_Directory)
                        vfs_rmdir(vfs_fixpath(dav->uri));
                    else
                        vfs_unlink(vfs_fixpath(dav->uri));
        //            http_set_response_status(request, "500 Internal Server Error");
                }
            }
            break;

        case HTTP_MkCol:
            if((ret = dav_init_request(request, method, uri)) == ERR_OK) {

                webdav_data_t *dav = (webdav_data_t *)request->private_data;

                if (dav->type == Resource_NotExist) {

                    vfs_mkdir(vfs_fixpath(dav->uri)); //, VFS_IRWXU|VFS_IRWXG|VFS_IRWXO);
        //            http_set_response_status(request, "500 Internal Server Error");
                }
            }
            break;

        case HTTP_PropFind:
            if((ret = dav_init_request(request, method, uri)) == ERR_OK) {

                webdav_data_t *dav = (webdav_data_t *)request->private_data;

                if(dav->content_len) {

                    request->post_receive_data = dav_receive_payload;
                    request->post_finished = propfind_receive_finished;

                    return http_get_payload(request, dav->content_len);

                } else
                    propfind_receive_finished(request, uri, uri_len);
            }
            break;

        case HTTP_PropPatch:
            if((ret = dav_init_request(request, method, uri)) == ERR_OK) {

                webdav_data_t *dav = (webdav_data_t *)request->private_data;

                if(dav->content_len) {

                    request->post_receive_data = dav_receive_payload;
                    request->post_finished = proppatch_receive_finished;

                    return http_get_payload(request, dav->content_len);

                } else
                    propfind_receive_finished(request, uri, uri_len);
            }
            break;

        default:
            ret = ERR_ARG;
            break;
    }

    return ret;
}

static void dav_on_options_report (http_request_t *request)
{
#if WEBDAV_ENABLE_LOCK
    http_set_response_header(request, "DAV", "1,2");
#else
    http_set_response_header(request, "DAV", "1");
#endif
}

bool webdav_init (void)
{
#if WEBDAV_ENABLE_LOCK
    http_set_allowed_methods("HEAD,GET,PUT,POST,DELETE,OPTIONS,COPY,MKCOL,MOVE,PROPFIND,PROPPATCH,LOCK,UNLOCK");
#else
    http_set_allowed_methods("HEAD,GET,PUT,POST,DELETE,OPTIONS,COPY,MKCOL,MOVE,PROPFIND,PROPPATCH");
#endif

    httpd.on_options_report = dav_on_options_report;
    httpd.on_unknown_method_process = dav_process_request;

    fs_ram_mount();

    return true;
}

#endif
