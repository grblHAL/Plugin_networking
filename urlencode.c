//
// urlencode.c - urlencode string, public domain
//
// Part of grblHAL
//

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <ctype.h>

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
            itoa(*s1++, s2++, 16);
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
