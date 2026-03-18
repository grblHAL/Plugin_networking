//
// utils.c - Assorted utilities for networking plugin - some sourced from internet and possibly modified (public domain)
//
// Part of grblHAL
//

#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#include "grbl/plugins.h"
#include "grbl/nuts_bolts.h"

#include "utils.h"

bool is_valid_port (uint16_t port)
{
    return port > 0;
}

bool is_valid_hostname (const char *hostname)
{
    bool ok = true;
    char *s = (char *)hostname;
    int c;
    size_t len = 0;

    while(ok) {
        if((c = *s++) == '\0')
            break;
        len++;
        ok = c == '-' || isdigit(c) || (isalpha(c) && c != ' ');
    }

    return ok && len >= HOSTNAME_LENGTH_MIN && len <= HOSTNAME_LENGTH_MAX;
}

bool is_valid_ssid (const char *ssid)
{
    bool ok = true;
    char *s = (char *)ssid;
    int c;
    size_t len = 0;

    while(ok) {
        if((c = *s++) == '\0')
            break;
        len++;
        ok = isprint(c);
    }

    return ok && len >= SSID_LENGTH_MIN && len <= SSID_LENGTH_MAX;
}

bool is_valid_password (const char *password)
{
    size_t len = strlen(password);

    if(len == strlen(HIDDEN_PASSWORD) && memcmp(password, HIDDEN_PASSWORD, len) == 0)
        len = PASSWORD_LENGTH_MAX + 1;

    return len >= PASSWORD_LENGTH_MIN && len <= PASSWORD_LENGTH_MAX;
}
