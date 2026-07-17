#include "gb_gamegenie.h"
#include "gb_cartridge.h"
#include "gb_log.h"
#include <cctype>
#include <cstring>

static int hex_nibbles(const char* code, u8* n, int want) {
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

bool gg_decode(const char* code, u16* addr, u8* val, u8* cmp, bool* has_cmp) {
    u8 n[9];
    int total = 0;
    for (const char* p = code; *p; p++) {
        char c = *p;
        if (c == '-' || c == ' ') continue;
        if (!isxdigit((unsigned char)c)) return false;
        total++;
    }
    if (total != 6 && total != 9) return false;
    if (hex_nibbles(code, n, total) != total) return false;

    *val  = (u8)((n[0] << 4) | n[1]);
    *addr = (u16)(((n[5] ^ 0xF) << 12) | (n[2] << 8) | (n[3] << 4) | n[4]);

    if (total == 9) {
        u8 t = (u8)((n[6] << 4) | n[8]);
        t = (u8)((t >> 2) | (t << 6));
        t ^= 0xBA;
        *cmp = t;
        *has_cmp = true;
    } else {
        *cmp = 0;
        *has_cmp = false;
    }
    return true;
}

int gg_apply(Cartridge& cart, const char* code, GGPatch* out, int maxpatches) {
    u16 addr; u8 val, cmp; bool has_cmp;
    if (!gg_decode(code, &addr, &val, &cmp, &has_cmp)) return -1;

    int np = 0;
    auto patch_one = [&](u32 off) {
        if (off >= cart.rom.size() || np >= maxpatches) return;
        out[np].offset = off;
        out[np].orig   = cart.rom[off];
        cart.rom[off]  = val;
        np++;
    };

    if (addr < 0x4000) {
        if (!has_cmp || cart.rom[addr] == cmp) patch_one(addr);
    } else if (addr < 0x8000) {
        u32 slot = (u32)(addr - 0x4000);
        int banks = cart.num_rom_banks > 0 ? cart.num_rom_banks : 1;
        for (int b = 0; b < banks; b++) {
            u32 off = (u32)b * 0x4000 + slot;
            if (off >= cart.rom.size()) break;
            if (!has_cmp || cart.rom[off] == cmp) patch_one(off);
        }
    } else {
        LOGW(LOG_SYS, "Game Genie: %s targets 0x%04X (not ROM) - can't patch, ignored", code, addr);
        return 0;
    }

    LOGI(LOG_SYS, "Game Genie: %s -> addr %04X = %02X%s (%d byte%s patched)",
         code, addr, val, has_cmp ? " (compare)" : "", np, np == 1 ? "" : "s");
    return np;
}

void gg_unapply(Cartridge& cart, const GGPatch* patches, int n) {
    for (int i = 0; i < n; i++)
        if (patches[i].offset < cart.rom.size())
            cart.rom[patches[i].offset] = patches[i].orig;
}
