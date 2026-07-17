#include "gb_cartridge.h"
#include "gb_log.h"
#include "unzip.h"
#include <cstring>
#include <ctime>

static void rtc_sync(Cartridge& c) {
    if (!c.has_rtc) return;
    if (!c.rtc_use_system_time) { c.rtc_base_secs = (long)time(nullptr); return; }
    long now = (long)time(nullptr);
    long elapsed = now - c.rtc_base_secs;
    if (elapsed <= 0) { c.rtc_base_secs = now; return; }
    c.rtc_base_secs = now;
    if (c.rtc[4] & 0x40) return;
    long s = c.rtc[0] + elapsed;
    long m = c.rtc[1] + s / 60;  c.rtc[0] = (u8)(s % 60);
    long h = c.rtc[2] + m / 60;  c.rtc[1] = (u8)(m % 60);
    long d = ((c.rtc[4] & 1) << 8 | c.rtc[3]) + h / 24; c.rtc[2] = (u8)(h % 24);
    c.rtc[3] = (u8)(d & 0xFF);
    c.rtc[4] = (u8)((c.rtc[4] & 0xFE) | ((d >> 8) & 1) | (d > 0x1FF ? 0x80 : 0));
}

Cartridge::Cartridge() {
    rom.resize(0x8000);
    ram.resize(0x8000);
}

static u8 read_file(const std::string& path, std::vector<u8>& dst) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    dst.resize(sz < 0x8000 ? 0x8000 : (size_t)sz);
    fread(dst.data(), 1, dst.size(), f);
    fclose(f);
    return 0;
}

bool Cartridge::load(const std::string& path) {
    if (path_is_zip(path)) {
        if (!unzip_extract_rom(path, rom)) return false;
        if (rom.size() < 0x8000) rom.resize(0x8000);
    } else {
        if (read_file(path, rom) != 0) return false;
    }

    rom_bank = 1;
    ram_bank = 0;
    ram_enable = false;
    ram_enable_inv = false;
    mbc1_mode = 0;
    mbc1_shift = 0;
    dirty = false;

    u8 cart_type = rom[0x147];
    rom_size = rom[0x148];
    u8 ram_size = rom[0x149];

    u8 cgb_byte = rom[0x143];
    if (cgb_byte == 0xC0) cgb_support = CGB_ONLY;
    else if (cgb_byte == 0x80) cgb_support = CGB_ENHANCED;
    else cgb_support = CGB_NONE;

    has_rtc = (cart_type == 0x0F || cart_type == 0x10);
    has_camera = (cart_type == 0xFC);
    cam_regs_mapped = false; cam_capture_requested = false;
    cam_busy_cycles = 0; cam_shots = 0; cam_pictures = 0; cam_logged_map = false;
    cam_last_matrix_set = false;
    memset(cam_shade_hist, 0, sizeof(cam_shade_hist));
    memset(cam_regs, 0, sizeof(cam_regs));
    memset(cam_sensor, 0x80, sizeof(cam_sensor));
    if (has_camera) LOGI(LOG_CART, "[CAM EMU] Detected camera tryna connect, pipeline started");
    rtc_select = 0; rtc_mapped = false; rtc_latch_last = 0xFF;
    rtc_base_secs = (long)time(nullptr);

    num_rom_banks = rom_size <= 8 ? (2 << rom_size) : 128;

    switch (cart_type) {
        case 0x00: mbc_type = MBC_NONE; break;
        case 0x01: case 0x02: case 0x03: mbc_type = MBC1; break;
        case 0x05: case 0x06: mbc_type = MBC2; break;
        case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: mbc_type = MBC3; break;
        case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: mbc_type = MBC5; break;
        case 0xFC: mbc_type = MBC_CAMERA; break;
        default:
            mbc_type = MBC_NONE;
            LOGW(LOG_CART, "unrecognized cartridge type header 0x%02X - falling back to no mapper, bank switching will not work", cart_type);
            break;
    }

    switch (ram_size) {
        case 0: num_ram_banks = 0; break;
        case 1: num_ram_banks = 1; break;
        case 2: num_ram_banks = 1; break;
        case 3: num_ram_banks = 4; break;
        case 4: num_ram_banks = 16; break;
        case 5: num_ram_banks = 8; break;
    }

    if (num_ram_banks > 0) ram.assign(num_ram_banks * 0x2000, 0);
    else ram.assign(0x2000, 0);

    static const char* mbc_names[] = {"NONE", "MBC1", "MBC2", "MBC3", "?", "MBC5"};
    static const char* cgb_names[] = {"DMG-only", "CGB-enhanced", "CGB-only"};
    LOGI(LOG_CART, "Loaded %s", path.c_str());
    LOGI(LOG_CART, "  type=%s (header 0x%02X) romBanks=%d ramBanks=%d %s",
        mbc_type <= 5 ? mbc_names[mbc_type] : "?", cart_type, num_rom_banks, num_ram_banks,
        cgb_names[cgb_support]);

    return true;
}

bool Cartridge::save_ram(const std::string& path) const {
    if (ram.empty()) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        LOGW(LOG_CART, "failed to open %s for writing - save NOT written", path.c_str());
        return false;
    }
    size_t sz = ram_size();
    fwrite(ram.data(), 1, sz, f);
    fclose(f);
    LOGI(LOG_CART, "saved %zu bytes to %s", sz, path.c_str());
    return true;
}

bool Cartridge::load_ram(const std::string& path) {
    if (ram.empty()) return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        LOGD(LOG_CART, "no save file at %s (new game, or first run)", path.c_str());
        return false;
    }
    size_t sz = ram_size();
    fread(ram.data(), 1, sz, f);
    fclose(f);
    LOGI(LOG_CART, "loaded %zu bytes from %s", sz, path.c_str());
    return true;
}

const u8* Cartridge::rom_ptr(u16 addr) const {
    if (addr < 0x4000) {
        if (mbc_type == MBC1 && mbc1_mode == 1) {
            u32 bank = rom_bank & 0x60;
            u32 offset = bank * 0x4000 + addr;
            if (offset >= rom.size()) offset %= rom.size();
            return &rom[offset];
        }
        return &rom[addr];
    }
    if (addr < 0x8000) {
        u32 bank = rom_bank;
        u32 real_bank = bank < (u32)num_rom_banks ? bank : bank % num_rom_banks;
        if (real_bank == 0 && mbc_type != MBC5) real_bank = 1;
        u32 offset = (u32)(addr - 0x4000) + real_bank * 0x4000;
        if (offset >= rom.size()) offset %= rom.size();
        return &rom[offset];
    }
    return nullptr;
}

u8 Cartridge::read(u16 addr) {
    if (addr < 0x8000) {
        u8 base = *rom_ptr(addr);
        if (gg_code_count) {
            for (int i = 0; i < gg_code_count; i++)
                if (gg_codes[i].addr == addr && (!gg_codes[i].has_cmp || base == gg_codes[i].cmp))
                    return gg_codes[i].val;
        }
        return base;
    }
    if (addr >= 0xA000 && addr < 0xC000) {
        if (mbc_type == MBC_CAMERA && cam_regs_mapped) {
            u8 reg = addr & 0x7F;
            if (reg != 0) return 0x00;
            return (u8)((cam_regs[0] & 0x06) | (cam_busy_cycles > 0 ? 1 : 0));
        }
        if (!ram_enable && mbc_type != MBC_CAMERA) return 0xFF;
        if (mbc_type == MBC3 && rtc_mapped) {
            int idx = rtc_select - 0x08;
            return (idx >= 0 && idx < 5) ? rtc_latched[idx] : 0xFF;
        }
        u32 offset = (u32)(addr - 0xA000);
        offset += ram_offset_base();
        return offset < ram.size() ? ram[offset] : 0xFF;
    }
    return 0xFF;
}

u32 Cartridge::ram_offset_base() const {
    if (num_ram_banks <= 1) return 0;
    u32 bank = ram_bank;
    if (mbc_type == MBC1) {
        if (mbc1_mode != 1) return 0;
        bank &= 3;
    } else if (mbc_type == MBC3) {
        bank &= 0x03;
    } else if (mbc_type == MBC5) {
        bank &= 0x0F;
    } else if (mbc_type == MBC_CAMERA) {
        bank &= 0x0F;
    } else {
        return 0;
    }
    if (bank >= (u32)num_ram_banks) bank %= num_ram_banks;
    return bank * 0x2000;
}

void Cartridge::camera_tick(int cycles) {
    if (cam_busy_cycles <= 0) return;
    cam_busy_cycles -= cycles;
    if (cam_busy_cycles <= 0) {
        cam_busy_cycles = 0;
        camera_capture();
    }
}

void Cartridge::camera_capture() {
    cam_regs[0] &= ~0x01;
    if (ram.size() < 0x2000) {
        LOGW(LOG_CART, "[CAM EMU] no cartridge RAM - picture has nowhere to land");
        return;
    }

    u32 hist[256] = {};
    for (int i = 0; i < 128 * 112; ++i) hist[cam_sensor[i]]++;
    const int total = 128 * 112;
    int lo = 0, hi = 255, acc = 0;
    for (int v = 0; v < 256; ++v) { acc += hist[v]; if (acc >= total / 50) { lo = v; break; } }
    acc = 0;
    for (int v = 255; v >= 0; --v) { acc += hist[v]; if (acc >= total / 50) { hi = v; break; } }
    float lscale = (hi - lo >= 24) ? 255.0f / (float)(hi - lo) : 1.0f;
    if (hi - lo < 24) lo = 0;

    u32 exposure = ((u32)cam_regs[2] << 8) | cam_regs[3];
    if (exposure == 0) exposure = 0x1000;
    float trim = (float)exposure / 4096.0f;
    if (trim < 0.6f) trim = 0.6f;
    if (trim > 1.6f) trim = 1.6f;
    const bool invert = (cam_regs[4] & 0x08) != 0;

    bool matrix_set = false;
    for (int i = 6; i < 6 + 48; ++i) if (cam_regs[i]) { matrix_set = true; break; }
    static const u8 kBayer[16] = { 1, 9, 3, 11, 13, 5, 15, 7, 4, 12, 2, 10, 16, 8, 14, 6 };

    cam_shade_hist[0] = cam_shade_hist[1] = cam_shade_hist[2] = cam_shade_hist[3] = 0;
    u8* dst = ram.data() + 0x0100;
    for (int y = 0; y < 112; ++y) {
        for (int x = 0; x < 128; ++x) {
            int v = (int)((cam_sensor[y * 128 + x] - lo) * lscale * trim);
            if (v < 0)   v = 0;
            if (v > 255) v = 255;
            if (invert) v = 255 - v;

            int cellidx = (y & 3) * 4 + (x & 3);
            u8 t0, t1, t2;
            if (matrix_set) {
                const u8* cell = &cam_regs[6 + cellidx * 3];
                t0 = cell[0]; t1 = cell[1]; t2 = cell[2];
            } else {
                int bias = kBayer[cellidx] * 3;
                t0 = (u8)(40 + bias); t1 = (u8)(104 + bias); t2 = (u8)(168 + bias);
            }
            u8 shade;
            if      (v < t0) shade = 3;
            else if (v < t1) shade = 2;
            else if (v < t2) shade = 1;
            else             shade = 0;
            cam_shade_hist[shade]++;

            u8* tile = dst + ((y >> 3) * 16 + (x >> 3)) * 16 + (y & 7) * 2;
            u8 mask = (u8)(0x80 >> (x & 7));
            if (shade & 1) tile[0] |= mask; else tile[0] &= ~mask;
            if (shade & 2) tile[1] |= mask; else tile[1] &= ~mask;
        }
    }
    dirty = true;
    cam_last_matrix_set = matrix_set;
    cam_level_lo = lo; cam_level_hi = hi;
    cam_pictures++;
}

bool g_mbc_trace = false;
u32 g_mbc_anomaly = 0;

void Cartridge::write(u16 addr, u8 val) {
    if (g_mbc_trace && addr < 0x8000)
        fprintf(stderr, "[MBC] w %04X=%02X (rom_bank %d ram_bank %d mode %d)\n",
                addr, val, rom_bank, ram_bank, mbc1_mode);
    if (g_mbc_anomaly == 0 && addr >= 0x3000 && addr < 0x4000) g_mbc_anomaly = 1;
    if (addr < 0x2000) {
        if (mbc_type == MBC1) {
            ram_enable = ((val & 0x0F) == 0x0A);
        } else if (mbc_type == MBC3) {
            ram_enable = ((val & 0x0F) == 0x0A);
        } else if (mbc_type == MBC5) {
            ram_enable = ((val & 0x0F) == 0x0A);
        } else if (mbc_type == MBC2) {
            if ((addr & 0x0100) == 0) ram_enable = ((val & 0x0F) == 0x0A);
        } else if (mbc_type == MBC_CAMERA) {
            ram_enable = ((val & 0x0F) == 0x0A);
        }
    } else if (addr < 0x4000) {
        if (mbc_type == MBC1) {
            u8 bank = val & 0x1F;
            if (bank == 0) bank = 1;
            rom_bank = (rom_bank & 0x60) | bank;
        } else if (mbc_type == MBC3) {
            u8 bank = val & 0x7F;
            if (bank == 0) bank = 1;
            rom_bank = bank;
        } else if (mbc_type == MBC5) {
            if (addr < 0x3000) rom_bank = (rom_bank & 0x100) | val;
            else rom_bank = (rom_bank & 0xFF) | ((val & 1) << 8);
        } else if (mbc_type == MBC2) {
            if ((addr & 0x0100) != 0) {
                u8 bank = val & 0x0F;
                if (bank == 0) bank = 1;
                rom_bank = bank;
            }
        } else if (mbc_type == MBC_CAMERA) {
            u8 bank = val & 0x3F;
            if (bank == 0) bank = 1;
            rom_bank = bank;
        }
    } else if (addr < 0x6000) {
        if (mbc_type == MBC1) {
            u8 bank2 = val & 3;
            rom_bank = (rom_bank & 0x1F) | (bank2 << 5);
            ram_bank = (mbc1_mode == 1) ? bank2 : 0;
        } else if (mbc_type == MBC3) {
            if (val >= 0x08 && val <= 0x0C) {
                rtc_mapped = true; rtc_select = val;
            } else {
                rtc_mapped = false; ram_bank = val & 0x03;
            }
        } else if (mbc_type == MBC5) {
            ram_bank = val & 0x0F;
        } else if (mbc_type == MBC_CAMERA) {
            if (val & 0x10) {
                if (!cam_logged_map) {
                    cam_logged_map = true;
                    LOGI(LOG_CART, "[CAM EMU] ROM mapped the sensor registers at A000-BFFF");
                }
                cam_regs_mapped = true;
            } else { cam_regs_mapped = false; ram_bank = val & 0x0F; }
        }
    } else if (addr < 0x8000) {
        if (mbc_type == MBC1) {
            mbc1_mode = val & 1;
            ram_bank = (mbc1_mode == 1) ? ((rom_bank >> 5) & 3) : 0;
        } else if (mbc_type == MBC3 && has_rtc) {
            if (rtc_latch_last == 0x00 && val == 0x01) {
                rtc_sync(*this);
                memcpy(rtc_latched, rtc, sizeof(rtc));
            }
            rtc_latch_last = val;
        }
    } else if (addr >= 0xA000 && addr < 0xC000) {
        if (mbc_type == MBC_CAMERA && cam_regs_mapped) {
            u8 reg = addr & 0x7F;
            cam_regs[reg] = val;
            if (reg == 0 && (val & 0x01) && cam_busy_cycles == 0) {
                u32 exposure = ((u32)cam_regs[2] << 8) | cam_regs[3];
                cam_busy_cycles = 32446 + ((cam_regs[1] & 0x80) ? 0 : 512) + (int)(16 * exposure);
                cam_capture_requested = true;

                if (cam_shots == 0)
                    LOGI(LOG_CART, "[CAM EMU] First shutter - game is taking pictures");
                cam_shots++;
            }
            return;
        }
        if (!ram_enable) return;
        if (mbc_type == MBC3 && rtc_mapped) {
            int idx = rtc_select - 0x08;
            if (idx >= 0 && idx < 5) {
                rtc_sync(*this);
                rtc[idx] = val;
                memcpy(rtc_latched, rtc, sizeof(rtc));
                dirty = true;
            }
            return;
        }
        u32 offset = (u32)(addr - 0xA000) + ram_offset_base();
        if (offset < ram.size()) { ram[offset] = val; dirty = true; }
    }
}
