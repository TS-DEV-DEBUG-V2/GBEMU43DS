#pragma once
#include "gb_types.h"
#include <vector>
#include <string>
#include <cstdio>

struct Cartridge {
    std::vector<u8> rom;
    std::vector<u8> ram;
    MBCType mbc_type = MBC_NONE;
    u8 rom_bank = 1;
    u8 ram_bank = 0;
    bool ram_enable = false;
    bool ram_enable_inv = false;
    int num_rom_banks = 2;
    int num_ram_banks = 0;
    u8 mbc1_mode = 0;
    u8 mbc1_shift = 0;
    u8 rom_size = 0;
    mutable bool dirty = false;

    bool has_rtc = false;
    bool rtc_use_system_time = true;
    u8   rtc_select = 0;
    bool rtc_mapped = false;
    u8   rtc[5] = {0,0,0,0,0};
    u8   rtc_latched[5] = {0,0,0,0,0};
    u8   rtc_latch_last = 0xFF;
    long rtc_base_secs = 0;

    enum CGBSupport { CGB_NONE = 0, CGB_ENHANCED = 1, CGB_ONLY = 2 };
    CGBSupport cgb_support = CGB_NONE;
    bool is_cgb() const { return cgb_support != CGB_NONE; }

    Cartridge();
    bool load(const std::string& path);
    bool save_ram(const std::string& path) const;
    bool load_ram(const std::string& path);
    u8 read(u16 addr);
    void write(u16 addr, u8 val);
    bool has_camera = false;
    bool cam_regs_mapped = false;
    u8   cam_regs[0x80] = {};
    u8   cam_sensor[128 * 112] = {};
    bool cam_capture_requested = false;
    int  cam_busy_cycles = 0;
    u32  cam_shots = 0;
    u32  cam_pictures = 0;
    u32  cam_shade_hist[4] = {};
    int  cam_level_lo = 0, cam_level_hi = 255;
    bool cam_last_matrix_set = false;
    bool cam_logged_map = false;
    void camera_tick(int cycles);
    void camera_capture();

    struct GGCode { u16 addr; u8 val; u8 cmp; bool has_cmp; };
    static const int GG_MAX = 32;
    GGCode gg_codes[GG_MAX];
    int gg_code_count = 0;
    void gg_clear() { gg_code_count = 0; }
    bool gg_add(u16 addr, u8 val, u8 cmp, bool has_cmp) {
        if (gg_code_count >= GG_MAX) return false;
        gg_codes[gg_code_count].addr = addr; gg_codes[gg_code_count].val = val;
        gg_codes[gg_code_count].cmp = cmp;   gg_codes[gg_code_count].has_cmp = has_cmp;
        gg_code_count++;
        return true;
    }

    const u8* rom_ptr(u16 addr) const;
    u32 ram_offset_base() const;
    size_t ram_size() const { return num_ram_banks > 0 ? (size_t)num_ram_banks * 0x2000 : 0x2000; }
    const u8* ram_data() const { return ram.data(); }
    u8* ram_data() { return ram.data(); }
};
