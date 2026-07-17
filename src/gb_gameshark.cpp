#include "gb_gameshark.h"
#include <cctype>

static int gs_hex_nibbles(const char* code, u8* n, int want) {
    int got = 0;
    for (const char* p = code; *p && got < want; p++) {
        char c = *p;
        if (c == '-' || c == ' ') continue;
        if (!isxdigit((unsigned char)c)) return -1;
        c = (char)toupper((unsigned char)c);
        n[got++] = (u8)(c <= '9' ? c - '0' : c - 'A' + 10);
    }
    return got;
}

bool gs_decode(const char* code, u8* type, u16* addr, u8* val) {
    int total = 0;
    for (const char* p = code; *p; p++) {
        char c = *p;
        if (c == '-' || c == ' ') continue;
        if (!isxdigit((unsigned char)c)) return false;
        total++;
    }
    if (total != 8) return false;

    u8 n[8];
    if (gs_hex_nibbles(code, n, 8) != 8) return false;

    *type = (u8)((n[0] << 4) | n[1]);
    *val  = (u8)((n[2] << 4) | n[3]);
    u8 lo = (u8)((n[4] << 4) | n[5]);
    u8 hi = (u8)((n[6] << 4) | n[7]);
    *addr = (u16)(((u16)hi << 8) | lo);
    return true;
}
