/* Copyright (c) 2013, Philipp T�lke
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the <organization> nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Original: https://github.com/toelke/lwip-ftpd
 */

/*
 * 2021-05-22: Modified by Terje Io for grblHAL networking
 */

#include "vfs.h"

#if FTP_ENABLE

#include <string.h>
#include <stdlib.h>

static FIL guard_for_the_whole_fs;

int vfs_read (void *buffer, int dummy, int len, vfs_file_t *file)
{
    unsigned int bytesread;
    FRESULT r = f_read(file, buffer, len, &bytesread);

    return r == FR_OK ? bytesread : -r;
}

int vfs_size (vfs_file_t *file)
{
    return f_size(file);
}

vfs_dirent_t *vfs_readdir(vfs_dir_t *dir)
{
    FILINFO fi;

    /* dirent that will be given to callers;
     * note: both APIs assume that only one dirent ever exists
     */
    static vfs_dirent_t dir_ent;

#if _USE_LFN
    fi.lfname = NULL;
#endif

    if (f_readdir(dir, &fi) != FR_OK || *fi.fname == '\0')
        return NULL;

    if(!strcmp(fi.fname, "System Volume Information") && (f_readdir(dir, &fi) != FR_OK || *fi.fname == '\0'))
        return NULL;

    memcpy(dir_ent.name, fi.fname, sizeof(fi.fname));

    return &dir_ent;
}

int vfs_stat (vfs_t *vfs, const char *filename, vfs_stat_t *st)
{
    FILINFO f;
#if _USE_LFN
    f.lfname = NULL;
#endif

    if (f_stat(filename, &f) == FR_OK) {
        st->st_size = f.fsize;
        st->st_mode = f.fattrib;
        struct tm tm  = {
            .tm_sec  = (f.ftime & 0x1f) << 1,
            .tm_min  = (f.ftime >> 5) & 0x3f,
            .tm_hour = (f.ftime >> 11) & 0x1f,
            .tm_mday = f.fdate & 0x1f,
            .tm_mon  = (f.fdate >> 5) & 0xf,
            .tm_year = 80 + ((f.fdate >> 9) & 0x7f),
        };
        st->st_mtime = mktime(&tm);
    } else
        return 1;

    return 0;
}

void vfs_close (vfs_t* vfs)
{
    if (vfs != &guard_for_the_whole_fs) {
        /* Close a file */
        f_close(vfs);
        free(vfs);
    }
}

int vfs_write (void *buffer, int dummy, int len, vfs_file_t* file)
{
    unsigned int byteswritten;

    if (f_write(file, buffer, len, &byteswritten) != FR_OK)
        byteswritten = 0;

    return byteswritten;
}

vfs_t* vfs_openfs (void)
{
    return &guard_for_the_whole_fs;
}

vfs_file_t* vfs_open (vfs_t *vfs, const char* filename, const char* mode)
{
    BYTE flags = 0;
    vfs_file_t *f = malloc(sizeof(vfs_file_t));

    while (*mode != '\0') {
        if (*mode == 'r')
            flags |= FA_READ;
        else if (*mode == 'w')
            flags |= FA_WRITE | FA_CREATE_ALWAYS;
        mode++;
    }

    if (f_open(f, filename, flags) != FR_OK) {
        free(f);
        f = NULL;
    }

    return f;
}

char *vfs_getcwd (vfs_t *vfs, void *dummy1, int dummy2)
{
    char *cwd = malloc(255);

    if (f_getcwd(cwd, 255) == FR_OK) {
        char *s1, *s2;
        // Strip drive information
        if((s2 = strchr(cwd, ':'))) {
            s1 = cwd;
            s2++;
            while(*s2)
                *s1++ = *s2++;
            *s1 = '\0';
        }
    } else {
        free(cwd);
        cwd = NULL;
    }

    return cwd;
}

vfs_dir_t* vfs_opendir (vfs_t *vfs, const char* path)
{
    vfs_dir_t* dir = malloc(sizeof *dir);

    if (f_opendir(dir, path) != FR_OK)
    {
        free(dir);
        dir = NULL;
    }

    return dir;
}

void vfs_closedir (vfs_dir_t *dir)
{
    if (dir) {
        f_closedir(dir);
        free(dir);
    }
}

// Strip trailing directory separator, FatFS dont't like it (WinSCP adds it)
char *vfs_fixpath (char *path)
{
    char *s = path + strlen(path) - 1;
    if(s > path && *s == '/' && s != strchr(path, '/'))
        *s = '\0';

    return path;
}

struct tm *gmtime (const time_t *c_t)
{
    static struct tm dummy = {
        .tm_year = 70,
        .tm_mon  = 0,
        .tm_mday = 1,
        .tm_hour = 0,
        .tm_min  = 0
    };

    return &dummy;
}

#endif
