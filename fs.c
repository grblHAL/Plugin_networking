//
// fs.c - wrapper for vfs.c for use by httpd
//
// v0.2 / 2021-10-04 / Io Engineering / Terje
//

/*

Copyright (c) 2021, Terje Io
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

ï¿½ Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

ï¿½ Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

ï¿½ Neither the name of the copyright holder nor the names of its contributors may
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

#if HTTP_ENABLE

#include "vfs.h"
#include "httpd.h"

static const embedded_file_t **ro_files = NULL;
static stream_write_ptr wrptr;
static stream_block_tx_buffer_t txbuf = {0};

static struct fs_file v_file = {0};

static bool vf_write (void)
{
    char *data = realloc((void *)v_file.data, v_file.len + txbuf.length);

    if(data == NULL) {

        if(v_file.data != NULL)
            free((void *)v_file.data);

        v_file.len = 0;
        v_file.data = NULL;

        return false;
    }

    v_file.data = data;

    memcpy((void *)(v_file.data + v_file.len), txbuf.data, txbuf.length);

    v_file.len += txbuf.length;
    txbuf.s = txbuf.data;
    txbuf.length = 0;

    return true;
}

static void fs_write (const char *s)
{
    size_t length = strlen(s);

    if(length == 0)
        return;

    if(txbuf.length && (txbuf.length + length) > txbuf.max_length) {
        if(!vf_write())
            return;
    }

    while(length > txbuf.max_length) {
        txbuf.length = txbuf.max_length;
        memcpy(txbuf.s, s, txbuf.length);
        if(!vf_write())
            return;
        length -= txbuf.max_length;
        s += txbuf.max_length;
    }

    if(length) {
        memcpy(txbuf.s, s, length);
        txbuf.length += length;
        txbuf.s += length;
    }
}

struct fs_file *fs_create (void)
{
    if(hal.stream.write == fs_write)
        return NULL;

//    if(v_file.data)
//        return NULL;

    wrptr = hal.stream.write;
    hal.stream.write = fs_write;

    txbuf.s = txbuf.data;
    txbuf.length = 0;
    txbuf.max_length = sizeof(txbuf.data);

    v_file.len = 0;
    v_file.is_custom_file = true;
    if(v_file.data) {
        free((void *)v_file.data);
        v_file.data = NULL;
    }

    return &v_file;
}

static const embedded_file_t *file_is_embedded (const char *name)
{
    uint_fast8_t idx = 0;
    const embedded_file_t *file = NULL;

    if(*name == '/')
        name++;

    if(ro_files) do {
        if(!strcmp(ro_files[idx]->name, name))
            file = ro_files[idx];
    } while(file == NULL && ro_files[++idx] != NULL);

    return file;
}

err_t fs_open (struct fs_file *file, const char *name)
{
    const embedded_file_t *ro_file;

    if(name == NULL)
        return ERR_ARG;

    if(!strncmp(name, "cgi:", 4)) {
        file->pextension = &v_file;
        file->is_custom_file = true;
        file->len = v_file.len;
    } else {

        char fname[255];

        if(*name == ':')
            strcpy(fname, name + 1);
        else {
            strcpy(fname, *name == '/' ? "/www" : "/www/");
            strcat(fname, name);
        }

        if((file->pextension = vfs_open(NULL, fname, "r"))) {
            file->len = vfs_size((vfs_file_t *)file->pextension);
            file->is_custom_file = 0;
        } else if((ro_file = file_is_embedded(name))) {
            file->len = ro_file->size;
            file->data = (char *)ro_file->data;
            file->is_custom_file = true;
            file->pextension = (void *)ro_file;
        } else
            file->pextension = NULL;
    }

    file->flags |= FS_FILE_FLAGS_HEADER_PERSISTENT;

    return file->pextension ? ERR_OK : ERR_ARG;
}

void fs_close (struct fs_file *file)
{
    if(file->is_custom_file == 0)
        vfs_close((vfs_file_t *)file->pextension);
    else {
        if(hal.stream.write == fs_write) {
            hal.stream.write = wrptr;
            wrptr = NULL;
            if(txbuf.length)
                vf_write();
        }

/*        if(file->pextension) {
            file = (struct fs_file *)file->pextension;

            if(file->data) {
                free((void *)file->data);
                file->data = NULL;
            }
        }*/
    }
}

int fs_read (struct fs_file *file, char *buffer, int count)
{
    if(file->is_custom_file == 0)
        count = vfs_read(buffer, 1, count, (vfs_file_t *)file->pextension);
    else {
        if(!file->data)
            file->data = v_file.data;
        if(file->data) {
            count = count > file->len ? file->len : count;
            memcpy(buffer, file->data, count);
            file->data += count;
        } else
            file->len = count = 0;
    }

    file->len -= count;

    return count;
}

int fs_bytes_left (struct fs_file *file)
{
    return file->len;
}

void fs_reset (void)
{
    if(hal.stream.write == fs_write) {
        hal.stream.write = wrptr;
        wrptr = NULL;
    }

    if(v_file.data != NULL) {
        free((void *)v_file.data);
        v_file.data = NULL;
    }
}

void fs_register_embedded_files (const embedded_file_t **files)
{
    ro_files = files;
}

#endif
