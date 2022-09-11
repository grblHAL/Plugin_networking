/*
  web/upload.h - An embedded CNC Controller with rs274/ngc (g-code) support

  Webserver backend - file upload

  Part of grblHAL

  Copyright (c) 2019-2022 Terje Io

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __WEB_UPLOAD_H__
#define __WEB_UPLOAD_H__

#include <stdio.h>
#include <stdlib.h>

#include "grbl/vfs.h"
#include "networking/httpd.h"

#define HTTP_UPLOAD_MAX_PATHLENGTH 100

typedef enum
{
    Upload_Parsing = 0,
    Upload_GetPath,
    Upload_GetSize,
    Upload_Write,
    Upload_Failed,
    Upload_Complete
} upload_state_t;

typedef union {
    FILE *handle;
#ifdef GRBL_VFS
    vfs_file_t *vfs_handle;
#else
#endif
} file_handle_t;

typedef void (*http_upload_filename_parsed_ptr)(char *name, void *data);

typedef struct {
    upload_state_t state;
    bool to_fatfs;
    char header_name[100];
    char header_value[100];
    char filename[HTTP_UPLOAD_MAX_PATHLENGTH + 1];
    char path[HTTP_UPLOAD_MAX_PATHLENGTH + 1];
    char size_str[15];
    file_handle_t file;
    http_request_t *req;
#ifndef GRBL_VFS
    FIL fatfs_fd;
#endif
    size_t size;
    size_t uploaded;
    http_upload_filename_parsed_ptr on_filename_parsed;
    void *on_filename_parsed_arg;
} file_upload_t;

file_upload_t *http_upload_start (http_request_t *req, const char* boundary, bool to_fatfs);
size_t http_upload_chunk (http_request_t *req, const char* data, size_t size);
void http_upload_on_filename_parsed (file_upload_t *upload, http_upload_filename_parsed_ptr fn, void *data);

#endif

