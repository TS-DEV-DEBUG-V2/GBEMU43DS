#pragma once
#include "gb_types.h"
#include "gb_memory.h"
#include <array>

struct PPU {
    std::array<u32, SCREEN_W * SCREEN_H> framebuffer;
    std::array<u32, SCREEN_W * SCREEN_H> pixels;

    bool output_bswap = false;

    bool cgb_vivid = false;

    u32* fb_draw = nullptr;
    u32* fb_done = nullptr;
    bool render_latch = true;
    int  fb_stride = SCREEN_W;
    bool fb_external = false;

    u32 dot_counter = 0;
    PPUMode mode = PPU_MODE_OAM_SCAN;
    u32 frame_count = 0;
    bool lcd_was_enabled = false;
    int mode3_duration = 0;

    int window_line_counter = 0;

    static const int BGP_SEG_MAX = 24;
    u8  bgp_seg_px[BGP_SEG_MAX];
    u8  bgp_seg_val[BGP_SEG_MAX];
    int bgp_seg_n = 0;
    u8  bgp_line_start = 0;
    int wlc_line_start = 0;
    void note_bgp_write(u8 val, int extra_dots);

    u8 peek_ly(int ahead, u8 io_ly) const;
    u8 peek_stat_bits(int ahead, u8 io_ly, u8 lyc) const;

    bool window_wy_latched = false;

    bool stat_signal = false;

    int vblank_line = 0;

    bool render_enabled = true;

    struct SpriteInfo {
        u8 x, tile, flags;
        int y;
        u8 index;
    };
    SpriteInfo visible_sprites[10];
    int num_sprites = 0;

    u8 bg_tile_cache[21][8];
    u8 win_tile_cache[21][8];
    u8 bg_tile_attr_cache[21];
    u8 win_tile_attr_cache[21];

    u32 cgb_bg_lut[8][4];
    u32 cgb_obj_lut[8][4];

    void reset();
    void step(int cycles, Memory& mem, u8* iflag);
    int cycles_to_next_event() const;
    int cycles_to_next_line() const;

private:
    u32 get_color(u8 palette, u8 color_idx) const;
    u32 get_cgb_color(const std::array<u8, 64>& pal_ram, u8 palette_num, u8 color_idx) const;
    int decode_tile_row(u8 byte1, u8 byte2, u8* out);
    void render_line(Memory& mem, u8 ly);
    void scan_oam(Memory& mem, u8 ly, int sprite_h);
    void update_stat(Memory& mem, u8* iflag);
};
