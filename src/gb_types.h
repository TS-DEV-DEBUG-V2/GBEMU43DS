#pragma once
#include <cstdint>
#include <cstddef>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8  = int8_t;
using s16 = int16_t;

enum {
    LCDC  = 0xFF40,
    STAT  = 0xFF41,
    SCY   = 0xFF42,
    SCX   = 0xFF43,
    LY    = 0xFF44,
    LYC   = 0xFF45,
    DMA   = 0xFF46,
    BGP   = 0xFF47,
    OBP0  = 0xFF48,
    OBP1  = 0xFF49,
    WY    = 0xFF4A,
    WX    = 0xFF4B,
    P1    = 0xFF00,
    SB    = 0xFF01,
    SC    = 0xFF02,
    DIV   = 0xFF04,
    TIMA  = 0xFF05,
    TMA   = 0xFF06,
    TAC   = 0xFF07,
    IF    = 0xFF0F,
    IE    = 0xFFFF,
    NR10  = 0xFF10,
    NR11  = 0xFF11,
    NR12  = 0xFF12,
    NR13  = 0xFF13,
    NR14  = 0xFF14,
    NR21  = 0xFF16,
    NR22  = 0xFF17,
    NR23  = 0xFF18,
    NR24  = 0xFF19,
    NR30  = 0xFF1A,
    NR31  = 0xFF1B,
    NR32  = 0xFF1C,
    NR33  = 0xFF1D,
    NR34  = 0xFF1E,
    NR41  = 0xFF20,
    NR42  = 0xFF21,
    NR43  = 0xFF22,
    NR44  = 0xFF23,
    NR50  = 0xFF24,
    NR51  = 0xFF25,
    NR52  = 0xFF26,
    BOOT  = 0xFF50,
};

enum Interrupt {
    INT_VBLANK = 1,
    INT_STAT   = 2,
    INT_TIMER  = 4,
    INT_SERIAL = 8,
    INT_JOYPAD = 16,
};

enum PPUMode {
    PPU_MODE_HBLANK   = 0,
    PPU_MODE_VBLANK   = 1,
    PPU_MODE_OAM_SCAN = 2,
    PPU_MODE_DRAW     = 3,
};

enum MBCType {
    MBC_NONE = 0,
    MBC1     = 1,
    MBC2     = 2,
    MBC3     = 3,
    MBC5     = 5,
    MBC_CAMERA = 6,
};

static const int SCREEN_W = 160;
static const int SCREEN_H = 144;
static const int CYCLES_PER_FRAME = 70224;
