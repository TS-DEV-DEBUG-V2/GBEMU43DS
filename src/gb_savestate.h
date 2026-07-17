#pragma once
#include "gb_types.h"

struct CPU;
struct Memory;
struct PPU;
struct Timer;
struct APU;
struct Cartridge;
struct Input;

bool savestate_write(const char* path, CPU& cpu, Memory& mem, PPU& ppu,
                     Timer& timer, APU& apu, Cartridge& cart);
bool savestate_read (const char* path, CPU& cpu, Memory& mem, PPU& ppu,
                     Timer& timer, APU& apu, Cartridge& cart, Input& input);

bool savestate_valid_for(const char* path, Cartridge& cart);
