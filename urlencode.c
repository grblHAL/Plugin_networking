//
// urlencode.c - urlencode string, public domain
//
// Part of grblHAL
//

#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>

#ifdef __MSP432E401Y__
#include <string.h>
#include "grbl/nuts_bolts.h"
#else
#include <stdlib.h>
#endif

int urlencode (const char *uri, const char *encoded, size_t size)
{
    static bool initok = false;
    static char encode[256];

    if(!initok) {
        uint_fast16_t idx = 256;

        do {
            idx--;
            encode[idx] = idx > 126 || !(isalnum(idx) || idx == '~' || idx == '-' || idx == '.' || idx == '_');
        } while(idx);

        initok = true;
    }

    char *s1 = (char *)uri, *s2 = (char *)encoded;

    while(*s1 && size) {

        if(encode[(uint_fast8_t)*s1]) {
            *s2++ = '%';
#ifdef __MSP432E401Y__
            strcpy(s2++, uitoa(*s1++));
#else
            itoa(*s1++, s2++, 16);
#endif
            s2++;
            size -= 2;
        } else
            *s2++ = *s1++;
        size--;
    }

    if(size)
        *s2 = '\0';

    return size ? 0 : -1;
}
