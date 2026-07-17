#include "gb_savestate.h"
#include "gb_cpu.h"
#include "gb_memory.h"
#include "gb_ppu.h"
#include "gb_timer.h"
#include "gb_apu.h"
#include "gb_cartridge.h"
#include "gb_input.h"
#include "gb_log.h"
#include <cstdio>
#include <cstring>

static const char  MAGIC[4] = { 'G', 'B', 'S', 'T' };
static const u32   VERSION  = 3;

static u32 rom_id(const Cartridge& c) {
    u32 h = 2166136261u;
    for (int i = 0x134; i <= 0x14F; i++) {
        u8 b = (i < (int)c.rom.size()) ? c.rom[i] : 0;
        h = (h ^ b) * 16777619u;
    }
    return h;
}

static size_t ppu_state_off() { return (size_t)((char*)&((PPU*)0)->dot_counter - (char*)(PPU*)0); }

static bool wr(FILE* f, const void* p, size_t n) { return fwrite(p, 1, n, f) == n; }
static bool rd(FILE* f, void* p, size_t n)       { return fread(p, 1, n, f) == n; }

static void write_cart_regs(FILE* f, const Cartridge& c) {
    wr(f, &c.rom_bank, sizeof(c.rom_bank));
    wr(f, &c.ram_bank, sizeof(c.ram_bank));
    wr(f, &c.ram_enable, sizeof(c.ram_enable));
    wr(f, &c.ram_enable_inv, sizeof(c.ram_enable_inv));
    wr(f, &c.mbc1_mode, sizeof(c.mbc1_mode));
    wr(f, &c.mbc1_shift, sizeof(c.mbc1_shift));
    wr(f, &c.has_rtc, sizeof(c.has_rtc));
    wr(f, &c.rtc_select, sizeof(c.rtc_select));
    wr(f, &c.rtc_mapped, sizeof(c.rtc_mapped));
    wr(f, c.rtc, sizeof(c.rtc));
    wr(f, c.rtc_latched, sizeof(c.rtc_latched));
    wr(f, &c.rtc_latch_last, sizeof(c.rtc_latch_last));
    wr(f, &c.rtc_base_secs, sizeof(c.rtc_base_secs));
}
static void read_cart_regs(FILE* f, Cartridge& c) {
    rd(f, &c.rom_bank, sizeof(c.rom_bank));
    rd(f, &c.ram_bank, sizeof(c.ram_bank));
    rd(f, &c.ram_enable, sizeof(c.ram_enable));
    rd(f, &c.ram_enable_inv, sizeof(c.ram_enable_inv));
    rd(f, &c.mbc1_mode, sizeof(c.mbc1_mode));
    rd(f, &c.mbc1_shift, sizeof(c.mbc1_shift));
    rd(f, &c.has_rtc, sizeof(c.has_rtc));
    rd(f, &c.rtc_select, sizeof(c.rtc_select));
    rd(f, &c.rtc_mapped, sizeof(c.rtc_mapped));
    rd(f, c.rtc, sizeof(c.rtc));
    rd(f, c.rtc_latched, sizeof(c.rtc_latched));
    rd(f, &c.rtc_latch_last, sizeof(c.rtc_latch_last));
    rd(f, &c.rtc_base_secs, sizeof(c.rtc_base_secs));
}

bool savestate_write(const char* path, CPU& cpu, Memory& mem, PPU& ppu,
                     Timer& timer, APU& apu, Cartridge& cart) {
    FILE* f = fopen(path, "wb");
    if (!f) { LOGW(LOG_CART, "savestate: cannot open %s for writing", path); return false; }

    u32 ver = VERSION, id = rom_id(cart);
    u8  cgb = cart.is_cgb() ? 1 : 0;
    bool ok = true;
    ok &= wr(f, MAGIC, 4);
    ok &= wr(f, &ver, sizeof(ver));
    ok &= wr(f, &id, sizeof(id));
    ok &= wr(f, &cgb, 1);

    ok &= wr(f, &cpu, sizeof(CPU));
    ok &= wr(f, &timer, sizeof(Timer));
    ok &= wr(f, &apu, sizeof(APU));
    size_t off = ppu_state_off();
    ok &= wr(f, (char*)&ppu + off, sizeof(PPU) - off);
    ok &= wr(f, &mem, sizeof(Memory));

    u32 ramlen = (u32)cart.ram.size();
    ok &= wr(f, &ramlen, sizeof(ramlen));
    if (ramlen) ok &= wr(f, cart.ram.data(), ramlen);
    write_cart_regs(f, cart);

    fclose(f);
    if (!ok) LOGW(LOG_CART, "savestate: short write to %s", path);
    return ok;
}

static bool read_header(FILE* f, Cartridge& cart) {
    char magic[4]; u32 ver = 0, id = 0; u8 cgb = 0;
    if (!rd(f, magic, 4) || memcmp(magic, MAGIC, 4) != 0) return false;
    if (!rd(f, &ver, sizeof(ver)) || ver != VERSION) return false;
    if (!rd(f, &id, sizeof(id))   || id != rom_id(cart)) return false;
    if (!rd(f, &cgb, 1)) return false;
    return true;
}

bool savestate_valid_for(const char* path, Cartridge& cart) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    bool ok = read_header(f, cart);
    fclose(f);
    return ok;
}

bool savestate_read(const char* path, CPU& cpu, Memory& mem, PPU& ppu,
                    Timer& timer, APU& apu, Cartridge& cart, Input& input) {
    FILE* f = fopen(path, "rb");
    if (!f) { LOGW(LOG_CART, "savestate: cannot open %s", path); return false; }
    if (!read_header(f, cart)) {
        LOGW(LOG_CART, "savestate: %s is invalid or from a different game", path);
        fclose(f);
        return false;
    }

    bool ok = true;
    ok &= rd(f, &cpu, sizeof(CPU));
    ok &= rd(f, &timer, sizeof(Timer));
    ok &= rd(f, &apu, sizeof(APU));
    size_t off = ppu_state_off();
    ok &= rd(f, (char*)&ppu + off, sizeof(PPU) - off);
    ok &= rd(f, &mem, sizeof(Memory));

    u32 ramlen = 0;
    ok &= rd(f, &ramlen, sizeof(ramlen));
    if (ramlen && ramlen == cart.ram.size()) ok &= rd(f, cart.ram.data(), ramlen);
    else if (ramlen) {
        fseek(f, ramlen, SEEK_CUR); ok = false;
    }
    read_cart_regs(f, cart);
    fclose(f);

    if (!ok) { LOGW(LOG_CART, "savestate: truncated/mismatched %s", path); return false; }

    mem.cart = &cart;
    mem.input = &input;
    mem.apu = &apu;
    mem.refresh_fast_pages();
    mem.cgb_pal_dirty = true;
    cpu.pc_ptr = nullptr;
    cpu.pc_end = nullptr;

    LOGI(LOG_CART, "savestate: loaded %s", path);
    return true;
}
