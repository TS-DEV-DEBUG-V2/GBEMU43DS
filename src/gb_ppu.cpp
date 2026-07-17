#include "gb_ppu.h"
#include "gb_log.h"
#include <cstring>
#include <algorithm>

static const u32 colors[4] = {
    0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000
};

static const int OAM_CYCLES = 80;
static const int MODE3_BASE = 172;
static const int MODE3_PER_SPRITE = 8;
static const int CYCLES_PER_LINE = 456;

static void init_tile_lut();

void PPU::reset() {
    init_tile_lut();
    framebuffer.fill(0xFFFFFFFF);
    pixels.fill(0xFFFFFFFF);
    dot_counter = 0;
    mode = PPU_MODE_OAM_SCAN;
    frame_count = 0;
    lcd_was_enabled = false;
    num_sprites = 0;
    mode3_duration = 0;
    window_line_counter = 0;
    window_wy_latched = false;
    stat_signal = false;
    if (!fb_external) {
        fb_draw = framebuffer.data();
        fb_done = pixels.data();
        fb_stride = SCREEN_W;
    }
    render_latch = true;
}

void PPU::note_bgp_write(u8 val, int extra_dots) {
    if (mode != PPU_MODE_DRAW || !lcd_was_enabled) return;
    if (bgp_seg_n >= BGP_SEG_MAX) return;
    int px = (int)dot_counter + extra_dots - 12;
    if (px < 0) px = 0;
    if (px > 159) px = 159;
    if (bgp_seg_n && bgp_seg_px[bgp_seg_n - 1] == (u8)px) {
        bgp_seg_val[bgp_seg_n - 1] = val;
        return;
    }
    bgp_seg_px[bgp_seg_n]  = (u8)px;
    bgp_seg_val[bgp_seg_n] = val;
    bgp_seg_n++;
}

static inline int line_pos_of(int mode, u32 dot_counter, int mode3_duration) {
    switch (mode) {
        case PPU_MODE_OAM_SCAN: return (int)dot_counter;
        case PPU_MODE_DRAW:     return 80 + (int)dot_counter;
        case PPU_MODE_HBLANK:   return 80 + mode3_duration + (int)dot_counter;
        default:                return (int)dot_counter;
    }
}

static inline void peek_advance(int cur_line, int pos, int& line, int& in_line) {
    line = cur_line;
    while (pos >= 456) { pos -= 456; line++; }
    if (line >= 154) line -= 154;
    in_line = pos;
}

u8 PPU::peek_ly(int ahead, u8 io_ly) const {
    if (!lcd_was_enabled) return 0;
    int cur_line = (mode == PPU_MODE_VBLANK) ? vblank_line : io_ly;
    int pos = line_pos_of(mode, dot_counter, mode3_duration) + ahead;
    int line, in_line;
    peek_advance(cur_line, pos, line, in_line);
    return (line == 153) ? 0 : (u8)line;
}

u8 PPU::peek_stat_bits(int ahead, u8 io_ly, u8 lyc) const {
    if (!lcd_was_enabled) return 0;
    int cur_line = (mode == PPU_MODE_VBLANK) ? vblank_line : io_ly;
    int pos = line_pos_of(mode, dot_counter, mode3_duration) + ahead;
    int line, in_line;
    peek_advance(cur_line, pos, line, in_line);
    u8 m;
    if (line >= 144) m = 1;
    else if (in_line < 80) m = 2;
    else if (in_line < 80 + mode3_duration) m = 3;
    else m = 0;
    u8 ly_now = (line == 153) ? 0 : (u8)line;
    return (u8)(m | ((ly_now == lyc) ? 0x04 : 0));
}

u32 PPU::get_color(u8 palette, u8 color_idx) const {
    u8 shift = color_idx * 2;
    u8 idx = (palette >> shift) & 3;
    u32 c = colors[idx];
    return output_bswap ? __builtin_bswap32(c) : c;
}

u32 PPU::get_cgb_color(const std::array<u8, 64>& pal_ram, u8 palette_num, u8 color_idx) const {
    int off = (palette_num & 7) * 8 + (color_idx & 3) * 2;
    u16 c = pal_ram[off] | ((u16)pal_ram[off + 1] << 8);
    u8 r5 = c & 0x1F;
    u8 g5 = (c >> 5) & 0x1F;
    u8 b5 = (c >> 10) & 0x1F;
    int r = (r5 << 3) | (r5 >> 2);
    int g = (g5 << 3) | (g5 >> 2);
    int b = (b5 << 3) | (b5 >> 2);
    if (cgb_vivid) {
        int lum = (r * 77 + g * 150 + b * 29) >> 8;
        r = lum + (((r - lum) * 3) >> 1);
        g = lum + (((g - lum) * 3) >> 1);
        b = lum + (((b - lum) * 3) >> 1);
        r = 128 + (((r - 128) * 9) >> 3);
        g = 128 + (((g - 128) * 9) >> 3);
        b = 128 + (((b - 128) * 9) >> 3);
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
    }
    u32 out = 0xFF000000u | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
    return output_bswap ? __builtin_bswap32(out) : out;
}

static u64 g_tile_lo[256];
static u64 g_tile_hi[256];

static void init_tile_lut() {
    for (int b = 0; b < 256; b++) {
        u64 lo = 0, hi = 0;
        for (int i = 0; i < 8; i++) {
            u64 bit = (u64)((b >> (7 - i)) & 1);
            lo |= bit << (i * 8);
            hi |= (bit << 1) << (i * 8);
        }
        g_tile_lo[b] = lo;
        g_tile_hi[b] = hi;
    }
}

int PPU::decode_tile_row(u8 byte1, u8 byte2, u8* out) {
    u64 v = g_tile_lo[byte1] | g_tile_hi[byte2];
    std::memcpy(out, &v, 8);
    return 0;
}

void PPU::scan_oam(Memory& mem, u8 ly, int sprite_h) {
    num_sprites = 0;
    int qualifying = 0;
    for (int i = 0; i < 40; i++) {
        int base = i * 4;
        u8 sy = mem.oam[base];
        u8 sx = mem.oam[base + 1];
        u8 stile = mem.oam[base + 2];
        u8 sflags = mem.oam[base + 3];
        if (sy == 0 || sy >= 160) continue;
        int spr_y = (int)sy - 16;
        if (ly >= spr_y && ly < spr_y + sprite_h) {
            qualifying++;
            if (num_sprites >= 10) continue;
            SpriteInfo si;
            si.x = sx; si.tile = stile; si.flags = sflags;
            si.y = spr_y; si.index = (u8)i;
            visible_sprites[num_sprites++] = si;
        }
    }
    if (qualifying > 10) {
        LOGT(LOG_PPU, "sprite overflow at ly=%d: %d qualify, hardware caps at 10", ly, qualifying);
    }

    if (!mem.cgb_mode) {
        std::stable_sort(visible_sprites, visible_sprites + num_sprites,
            [](const SpriteInfo& a, const SpriteInfo& b) {
                if (a.x != b.x) return a.x < b.x;
                return a.index < b.index;
            });
    }
}

void PPU::render_line(Memory& mem, u8 ly) {
    u8 lcdc = mem.io_regs[LCDC - 0xFF00];
    u8 scx  = mem.io_regs[SCX - 0xFF00];
    u8 scy  = mem.io_regs[SCY - 0xFF00];
    u8 bgp  = mem.io_regs[BGP - 0xFF00];
    u8 obp0 = mem.io_regs[OBP0 - 0xFF00];
    u8 obp1 = mem.io_regs[OBP1 - 0xFF00];
    u8 wy   = mem.io_regs[WY - 0xFF00];
    u8 wx   = mem.io_regs[WX - 0xFF00];

    bool cgb = mem.cgb_mode;

    bool bg_enable = lcdc & 0x01;
    bool obj_enable = lcdc & 0x02;
    bool obj_size = lcdc & 0x04;
    bool bg_tile_map = lcdc & 0x08;
    bool bg_tile_data = lcdc & 0x10;
    bool win_enable = lcdc & 0x20;
    bool win_tile_map = lcdc & 0x40;
    bool lcd_enable = lcdc & 0x80;

    if (!lcd_enable) return;

    bool bg_draw = cgb || bg_enable;
    bool master_priority = !cgb || bg_enable;

    u16 bg_map_base = bg_tile_map ? 0x9C00 : 0x9800;
    u16 win_map_base = win_tile_map ? 0x9C00 : 0x9800;
    u16 tile_data_base = bg_tile_data ? 0x8000 : 0x8800;
    bool signed_tiles = !bg_tile_data;
    int sprite_h = obj_size ? 16 : 8;

    scan_oam(mem, ly, sprite_h);

    if (!render_latch) return;

    u32 bg_lut[4], obj_lut[2][4];
    if (cgb) {
        if (mem.cgb_pal_dirty) {
            for (int p = 0; p < 8; p++) {
                for (int c = 0; c < 4; c++) {
                    cgb_bg_lut[p][c] = get_cgb_color(mem.bg_palette_ram, p, c);
                    cgb_obj_lut[p][c] = get_cgb_color(mem.obj_palette_ram, p, c);
                }
            }
            mem.cgb_pal_dirty = false;
        }
    } else {
        for (int c = 0; c < 4; c++) {
            bg_lut[c] = get_color(bgp, c);
            obj_lut[0][c] = get_color(obp0, c);
            obj_lut[1][c] = get_color(obp1, c);
        }
    }

    u32* fb = &fb_draw[ly * fb_stride];

    u8 bg_pixels[SCREEN_W];
    u8 bg_attr_px[SCREEN_W];
    if (!bg_draw) {
        std::memset(bg_pixels, 0, SCREEN_W);
        u32 c0 = bg_lut[0];
        for (int x = 0; x < SCREEN_W; x++) fb[x] = c0;
    }

    int bg_tile_y = ((int)ly + (int)scy) & 255;
    int bg_tile_row = bg_tile_y / 8;
    int bg_pixel_row_base = bg_tile_y % 8;

    int win_x_offset = (int)wx - 7;

    u16 bg_map_row = bg_map_base + bg_tile_row * 32;

    if (bg_draw) {
        int first_tile_x = (scx >> 3) & 31;
        for (int t = 0; t < 21; t++) {
            int map_col = (first_tile_x + t) & 31;
            u16 map_addr = bg_map_row + map_col;
            u8 tile_num = mem.vram[map_addr & 0x1FFF];
            u8 attr = cgb ? mem.vram_bank1[map_addr & 0x1FFF] : 0;
            bool tile_bank1 = attr & 0x08;
            bool x_flip = attr & 0x20;
            bool y_flip = attr & 0x40;
            bg_tile_attr_cache[t] = attr & 0x87;

            int pixel_row = y_flip ? (7 - bg_pixel_row_base) : bg_pixel_row_base;

            u16 tile_addr;
            if (signed_tiles)
                tile_addr = tile_data_base + ((s8)tile_num + 128) * 16 + pixel_row * 2;
            else
                tile_addr = tile_data_base + (u16)tile_num * 16 + pixel_row * 2;

            const std::array<u8, 0x2000>& bank = tile_bank1 ? mem.vram_bank1 : mem.vram;
            u8 byte1 = bank[tile_addr & 0x1FFF];
            u8 byte2 = bank[(tile_addr + 1) & 0x1FFF];
            decode_tile_row(byte1, byte2, bg_tile_cache[t]);
            if (x_flip) {
                for (int i = 0; i < 4; i++) std::swap(bg_tile_cache[t][i], bg_tile_cache[t][7 - i]);
            }
        }

        int x = 0;
        int sub = scx & 7;
        for (int t = 0; t < 21 && x < SCREEN_W; t++) {
            const u8* tc = bg_tile_cache[t];
            u8 attr = bg_tile_attr_cache[t];
            const u32* lut = cgb ? cgb_bg_lut[attr & 7] : bg_lut;
            for (int px = (t == 0 ? sub : 0); px < 8 && x < SCREEN_W; px++, x++) {
                u8 ci = tc[px];
                bg_pixels[x] = ci;
                bg_attr_px[x] = attr;
                fb[x] = lut[ci];
            }
        }
    }

    if (ly == wy) window_wy_latched = true;

    if (bg_draw && win_enable && window_wy_latched) {
        int win_tile_row = window_line_counter / 8;
        int win_pixel_row_base = window_line_counter % 8;
        window_line_counter++;
        if (wx <= 166) {
        if (win_x_offset < 0) win_x_offset = 0;
        u16 win_map_row = win_map_base + win_tile_row * 32;

        for (int t = 0; t < 21; t++) {
            int map_col = t & 31;
            u16 map_addr_w = win_map_row + map_col;
            u8 tile_num_w = mem.vram[map_addr_w & 0x1FFF];
            u8 attr = cgb ? mem.vram_bank1[map_addr_w & 0x1FFF] : 0;
            bool tile_bank1 = attr & 0x08;
            bool x_flip = attr & 0x20;
            bool y_flip = attr & 0x40;
            win_tile_attr_cache[t] = attr & 0x87;

            int pixel_row = y_flip ? (7 - win_pixel_row_base) : win_pixel_row_base;

            u16 tile_addr_w;
            if (signed_tiles)
                tile_addr_w = tile_data_base + ((s8)tile_num_w + 128) * 16 + pixel_row * 2;
            else
                tile_addr_w = tile_data_base + (u16)tile_num_w * 16 + pixel_row * 2;

            const std::array<u8, 0x2000>& bank = tile_bank1 ? mem.vram_bank1 : mem.vram;
            u8 byte1w = bank[tile_addr_w & 0x1FFF];
            u8 byte2w = bank[(tile_addr_w + 1) & 0x1FFF];
            decode_tile_row(byte1w, byte2w, win_tile_cache[t]);
            if (x_flip) {
                for (int i = 0; i < 4; i++) std::swap(win_tile_cache[t][i], win_tile_cache[t][7 - i]);
            }
        }

        for (int x = 0; x < SCREEN_W; x++) {
            int win_draw_x = x - win_x_offset;
            if (win_draw_x >= 0) {
                int tile_x_w = win_draw_x >> 3;
                int pixel_x_w = win_draw_x & 7;
                u8 ci = win_tile_cache[tile_x_w][pixel_x_w];
                u8 a = win_tile_attr_cache[tile_x_w];
                bg_pixels[x] = ci;
                bg_attr_px[x] = a;
                fb[x] = (cgb ? cgb_bg_lut[a & 7] : bg_lut)[ci];
            }
        }
        }
    }

    u8 spr_ci[SCREEN_W];
    u8 spr_attr[SCREEN_W];
    int spr_lo = SCREEN_W, spr_hi = -1;
    bool have_sprites = obj_enable && num_sprites > 0;
    if (have_sprites) {
        u8 sprite_pixels[10][8];
        for (int si = 0; si < num_sprites; si++) {
            auto& s = visible_sprites[si];
            int pixel_y_s = (int)ly - s.y;
            bool y_flip = s.flags & 0x40;
            if (y_flip) pixel_y_s = sprite_h - 1 - pixel_y_s;

            u8 tile_num_s = s.tile;
            if (obj_size) tile_num_s &= 0xFE;

            bool tile_bank1 = cgb && (s.flags & 0x08);
            u16 tile_addr = (u16)tile_num_s * 16 + pixel_y_s * 2;
            const std::array<u8, 0x2000>& bank = tile_bank1 ? mem.vram_bank1 : mem.vram;
            u8 byte1s = bank[tile_addr & 0x1FFF];
            u8 byte2s = bank[(tile_addr + 1) & 0x1FFF];
            decode_tile_row(byte1s, byte2s, sprite_pixels[si]);

            bool x_flip = s.flags & 0x20;
            if (x_flip) {
                for (int i = 0; i < 4; i++) {
                    std::swap(sprite_pixels[si][i], sprite_pixels[si][7 - i]);
                }
            }
        }

        std::memset(spr_ci, 0, SCREEN_W);
        for (int si = num_sprites - 1; si >= 0; si--) {
            SpriteInfo& s = visible_sprites[si];
            int spr_x = (int)s.x - 8;
            for (int px = 0; px < 8; px++) {
                int x = spr_x + px;
                if ((unsigned)x >= (unsigned)SCREEN_W) continue;
                u8 ci = sprite_pixels[si][px];
                if (ci == 0) continue;
                spr_ci[x] = ci;
                spr_attr[x] = s.flags;
                if (x < spr_lo) spr_lo = x;
                if (x > spr_hi) spr_hi = x;
            }
        }
    }

    if (!cgb && bgp_seg_n > 0) {
        int seg = 0;
        u8 cur = bgp_line_start;
        u32 slut[4];
        for (int c2 = 0; c2 < 4; c2++) slut[c2] = get_color(cur, c2);
        for (int x = 0; x < SCREEN_W; x++) {
            while (seg < bgp_seg_n && bgp_seg_px[seg] <= x) {
                cur = bgp_seg_val[seg++];
                for (int c2 = 0; c2 < 4; c2++) slut[c2] = get_color(cur, c2);
            }
            fb[x] = slut[bg_pixels[x]];
        }
    }

    if (have_sprites && spr_hi >= spr_lo) {
        if (!cgb) {
            for (int x = spr_lo; x <= spr_hi; x++) {
                u8 ci = spr_ci[x];
                if (!ci) continue;
                u8 fl = spr_attr[x];
                if (!((fl & 0x80) && bg_pixels[x] != 0))
                    fb[x] = obj_lut[(fl & 0x10) ? 1 : 0][ci];
            }
        } else {
            for (int x = spr_lo; x <= spr_hi; x++) {
                u8 ci = spr_ci[x];
                if (!ci) continue;
                u8 fl = spr_attr[x];
                bool bg_tile_wins = master_priority && bg_pixels[x] != 0 && (bg_attr_px[x] & 0x80);
                bool obj_behind   = master_priority && (fl & 0x80) && bg_pixels[x] != 0;
                if (!bg_tile_wins && !obj_behind)
                    fb[x] = cgb_obj_lut[fl & 0x07][ci];
            }
        }
    }
}

void PPU::update_stat(Memory& mem, u8* iflag) {
    u8 stat = mem.io_regs[STAT - 0xFF00];
    stat = (stat & 0xFC) | (mode & 3);

    u8 ly = mem.io_regs[LY - 0xFF00];
    u8 lyc = mem.io_regs[LYC - 0xFF00];
    bool lyc_eq = (ly == lyc);
    if (lyc_eq) stat |= 0x04;
    else stat &= ~0x04;

    u8 stat_interrupt = 0;
    if ((stat & 0x10) && mode == PPU_MODE_VBLANK) stat_interrupt = 1;
    if ((stat & 0x20) && mode == PPU_MODE_OAM_SCAN) stat_interrupt = 1;
    if ((stat & 0x08) && mode == PPU_MODE_HBLANK) stat_interrupt = 1;
    if ((stat & 0x40) && lyc_eq) stat_interrupt = 1;

    mem.io_regs[STAT - 0xFF00] = stat;

    bool signal = stat_interrupt != 0;
    if (signal && !stat_signal) *iflag |= INT_STAT;
    stat_signal = signal;
    mem.stat_irq_line = signal;
}

int PPU::cycles_to_next_event() const {
    if (!lcd_was_enabled) return CYCLES_PER_LINE;
    int dur;
    switch (mode) {
        case PPU_MODE_OAM_SCAN: dur = OAM_CYCLES; break;
        case PPU_MODE_DRAW:     dur = mode3_duration; break;
        case PPU_MODE_HBLANK:   dur = CYCLES_PER_LINE - OAM_CYCLES - mode3_duration; break;
        default:                dur = CYCLES_PER_LINE; break;
    }
    int rem = dur - (int)dot_counter;
    return rem > 0 ? rem : 1;
}

int PPU::cycles_to_next_line() const {
    if (!lcd_was_enabled) return CYCLES_PER_LINE;
    int used;
    switch (mode) {
        case PPU_MODE_OAM_SCAN: used = (int)dot_counter; break;
        case PPU_MODE_DRAW:     used = OAM_CYCLES + (int)dot_counter; break;
        case PPU_MODE_HBLANK:   used = OAM_CYCLES + mode3_duration + (int)dot_counter; break;
        default:                used = (int)dot_counter; break;
    }
    int rem = CYCLES_PER_LINE - used;
    return rem > 0 ? rem : 1;
}

void PPU::step(int cycles, Memory& mem, u8* iflag) {
    u8 lcdc = mem.io_regs[LCDC - 0xFF00];
    bool lcd_ena = lcdc & 0x80;

    {
        static u8 prev_lcdc = 0xFF;
        u8 changed = lcdc ^ prev_lcdc;
        if (changed & 0x7F) {
            if (changed & 0x01) LOGD(LOG_PPU, "LCDC: BG %s", (lcdc & 0x01) ? "ENABLED" : "disabled (blank BG on DMG)");
            if (changed & 0x02) LOGD(LOG_PPU, "LCDC: sprites %s", (lcdc & 0x02) ? "ENABLED" : "disabled");
            if (changed & 0x20) LOGD(LOG_PPU, "LCDC: window %s", (lcdc & 0x20) ? "ENABLED" : "disabled");
            if (changed & 0x10) LOGD(LOG_PPU, "LCDC: BG tile data = %s", (lcdc & 0x10) ? "0x8000 (unsigned)" : "0x8800 (signed)");
            if (changed & 0x08) LOGD(LOG_PPU, "LCDC: BG tile map = %s", (lcdc & 0x08) ? "0x9C00" : "0x9800");
            if (changed & 0x40) LOGD(LOG_PPU, "LCDC: window tile map = %s", (lcdc & 0x40) ? "0x9C00" : "0x9800");
            if (changed & 0x04) LOGD(LOG_PPU, "LCDC: sprite size = %s", (lcdc & 0x04) ? "8x16" : "8x8");
        }
        prev_lcdc = lcdc;
    }

    if (!lcd_ena) {
        if (lcd_was_enabled) {
            lcd_was_enabled = false;
            mem.io_regs[LY - 0xFF00] = 0;
            mode = PPU_MODE_HBLANK;
            dot_counter = 0;
            num_sprites = 0;
            stat_signal = false;
            std::fill(fb_draw, fb_draw + fb_stride * SCREEN_H, 0xFFFFFFFFu);
            LOGD(LOG_PPU, "LCD disabled (LCDC.7=0) at frame %u", frame_count);
        }
        return;
    }

    if (!lcd_was_enabled) {
        lcd_was_enabled = true;
        mode = PPU_MODE_OAM_SCAN;
        dot_counter = 0;
        mem.io_regs[LY - 0xFF00] = 0;
        window_line_counter = 0;
        window_wy_latched = false;
        render_latch = render_enabled;
        LOGD(LOG_PPU, "LCD enabled (LCDC.7=1) at frame %u", frame_count);
    }

    dot_counter += cycles;

    while (true) {
        switch (mode) {
            case PPU_MODE_OAM_SCAN: {
                if (dot_counter < OAM_CYCLES) return;
                dot_counter -= OAM_CYCLES;
                mode = PPU_MODE_DRAW;
                bgp_line_start = mem.io_regs[BGP - 0xFF00];
                bgp_seg_n = 0;
                wlc_line_start = window_line_counter;
                render_line(mem, mem.io_regs[LY - 0xFF00]);
                int ns = num_sprites > 10 ? 10 : num_sprites;
                mode3_duration = MODE3_BASE + ns * MODE3_PER_SPRITE;
                if (mode3_duration > 289) mode3_duration = 289;
                update_stat(mem, iflag);
                continue;
            }
            case PPU_MODE_DRAW: {
                if (dot_counter < (u32)mode3_duration) return;
                if (bgp_seg_n > 0 && !mem.cgb_mode) {
#ifdef GB_SEG_DEBUG
                    static int dbg = 0;
                    if (bgp_seg_n >= 4 && dbg < 200) { dbg++;
                        fprintf(stderr, "[SEG] frame %u ly %u segs %d\n",
                            frame_count, mem.io_regs[LY - 0xFF00], bgp_seg_n); }
#endif
                    window_line_counter = wlc_line_start;
                    render_line(mem, mem.io_regs[LY - 0xFF00]);
                }
                dot_counter -= mode3_duration;
                mode = PPU_MODE_HBLANK;
                mem.hdma_hblank_step();
                update_stat(mem, iflag);
                continue;
            }
            case PPU_MODE_HBLANK: {
                int hblank_dur = CYCLES_PER_LINE - OAM_CYCLES - mode3_duration;
                if (dot_counter < (u32)hblank_dur) return;
                dot_counter -= hblank_dur;
                u8 ly = mem.io_regs[LY - 0xFF00] + 1;
                mem.io_regs[LY - 0xFF00] = ly;
                if (ly >= SCREEN_H) {
                    mode = PPU_MODE_VBLANK;
                    vblank_line = 144;
                    *iflag |= INT_VBLANK;
                    frame_count++;
                    if (render_latch) std::swap(fb_draw, fb_done);
                    if (frame_count % 300 == 0) {
                        LOGI(LOG_PPU, "%u frames rendered", frame_count);
                    }
                } else {
                    mode = PPU_MODE_OAM_SCAN;
                }
                update_stat(mem, iflag);
                continue;
            }
            case PPU_MODE_VBLANK: {
                if (dot_counter < CYCLES_PER_LINE) return;
                dot_counter -= CYCLES_PER_LINE;
                vblank_line++;
                if (vblank_line > 153) {
                    mem.io_regs[LY - 0xFF00] = 0;
                    mode = PPU_MODE_OAM_SCAN;
                    window_line_counter = 0;
                    window_wy_latched = false;
                    render_latch = render_enabled;
                } else if (vblank_line == 153) {
                    mem.io_regs[LY - 0xFF00] = 153;
                    update_stat(mem, iflag);
                    mem.io_regs[LY - 0xFF00] = 0;
                } else {
                    mem.io_regs[LY - 0xFF00] = (u8)vblank_line;
                }
                update_stat(mem, iflag);
                continue;
            }
        }
    }
}
