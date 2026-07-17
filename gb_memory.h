#pragma once
#include "gb_types.h"
#include "gb_cartridge.h"
#include <array>

struct Input;
struct APU;
struct GBPrinter;
struct PPU;

struct Memory {
    Cartridge* cart = nullptr;
    Input* input = nullptr;
    APU* apu = nullptr;
    GBPrinter* printer = nullptr;
    PPU* ppu_ptr = nullptr;
    int  batch_dots = 0;
    void set_ppu(PPU* p) { ppu_ptr = p; }
    std::array<u8, 0x2000> vram;
    std::array<u8, 0x2000> vram_bank1;
    std::array<u8, 0x8000> wram;
    std::array<u8, 0xA0> oam;
    std::array<u8, 0x80> hram;
    std::array<u8, 0x80> io_regs;
    u8 ie_register = 0;
    std::array<u8, 0x900> boot_rom;
    bool boot_rom_active = true;
    bool boot_rom_is_cgb = false;

    bool cgb_mode = false;
    u8 vbk = 0;
    u8 svbk = 1;

    bool stat_irq_line = false;

    std::array<u8, 64> bg_palette_ram = {};
    std::array<u8, 64> obj_palette_ram = {};
    u8 bcps = 0;
    u8 ocps = 0;
    bool cgb_pal_dirty = true;

    bool hdma_active = false;
    u16 hdma_src = 0;
    u16 hdma_dst = 0;
    u8 hdma_blocks_left = 0;
    u8 hdma_dst_vbk = 0;

    u32 hdma_stall_cycles = 0;

    char serial_line[128] = {};
    int serial_line_len = 0;

    int serial_cycles = 0;
    u8  serial_tx = 0;
    void serial_tick(int sys_cycles);

    const u8* fast_read_page[16] = {};
    u8* fast_write_page[16] = {};
    u32 fast_gen = 0;
    void refresh_fast_pages();

    Memory();
    void reset();
    void set_cartridge(Cartridge* c);
    void set_input(Input* i) { input = i; }
    void set_apu(APU* a) { apu = a; }
    void set_printer(GBPrinter* p) { printer = p; }
    void load_boot_rom(const unsigned char* data, int size, bool is_cgb = false);

    u8 read_slow(u16 addr);
    void write_slow(u16 addr, u8 val);
    inline u8 read(u16 addr) {
        const u8* page = fast_read_page[addr >> 12];
        if (page) return page[addr & 0xFFF];
        return read_slow(addr);
    }
    inline void write(u16 addr, u8 val) {
        u8* page = fast_write_page[addr >> 12];
        if (page) { page[addr & 0xFFF] = val; return; }
        write_slow(addr, val);
    }
    u16 read16(u16 addr);
    void write16(u16 addr, u16 val);
    void hdma_hblank_step();

    u8 wram_bank() const { u8 b = cgb_mode ? (svbk & 7) : 1; return b == 0 ? 1 : b; }
    u8 vram_bank() const { return cgb_mode ? (vbk & 1) : 0; }
};