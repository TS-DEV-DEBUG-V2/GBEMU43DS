#pragma once
#include "gb_types.h"
#include "gb_memory.h"
#include "gb_ppu.h"
#include "gb_timer.h"
#include "gb_input.h"
#include "gb_apu.h"

struct CPU {
    union {
        struct { u8 f, a; };
        u16 af;
    };
    union {
        struct { u8 c, b; };
        u16 bc;
    };
    union {
        struct { u8 e, d; };
        u16 de;
    };
    union {
        struct { u8 l, h; };
        u16 hl;
    };
    u16 sp;
    u16 pc;
    bool halted = false;
    bool stop_mode = false;
    bool ime = false;
    u8 ime_scheduled = 0;
    bool double_speed = false;

    const u8* pc_ptr = nullptr;
    const u8* pc_end = nullptr;
    u32 pc_gen = 0;
    void jump(u16 target) { pc = target; pc_ptr = nullptr; pc_end = nullptr; }

    void reset(bool cgb = false);
    u8 tick(Memory& mem, PPU& ppu, Timer& timer, Input& input, APU& apu);

    int run_batch(Memory& mem, int budget, int& out_sys);

private:
    u8 step_core(Memory& mem);
};

static inline void set_z(CPU& cpu, bool v) { cpu.f = (cpu.f & ~0x80) | (v ? 0x80 : 0); }
static inline void set_n(CPU& cpu, bool v) { cpu.f = (cpu.f & ~0x40) | (v ? 0x40 : 0); }
static inline void set_h(CPU& cpu, bool v) { cpu.f = (cpu.f & ~0x20) | (v ? 0x20 : 0); }
static inline void set_c(CPU& cpu, bool v) { cpu.f = (cpu.f & ~0x10) | (v ? 0x10 : 0); }
static inline bool get_z(CPU& cpu) { return (cpu.f & 0x80) != 0; }
static inline bool get_n(CPU& cpu) { return (cpu.f & 0x40) != 0; }
static inline bool get_h(CPU& cpu) { return (cpu.f & 0x20) != 0; }
static inline bool get_c(CPU& cpu) { return (cpu.f & 0x10) != 0; }

static inline u8 read_imm8_slow(CPU& cpu, Memory& mem) {
    u16 addr = cpu.pc;
    const u8* page = mem.fast_read_page[addr >> 12];
    cpu.pc++;
    if (page) {
        const u8* p = page + (addr & 0xFFF);
        cpu.pc_ptr = p + 1;
        cpu.pc_end = page + 0x1000;
        cpu.pc_gen = mem.fast_gen;
        return *p;
    }
    cpu.pc_ptr = nullptr;
    cpu.pc_end = nullptr;
    return mem.read(addr);
}

static inline u8 read_imm8(CPU& cpu, Memory& mem) {
    if (cpu.pc_ptr < cpu.pc_end && cpu.pc_gen == mem.fast_gen) {
        cpu.pc++;
        return *cpu.pc_ptr++;
    }
    return read_imm8_slow(cpu, mem);
}

static inline u16 read_imm16(CPU& cpu, Memory& mem) {
    u16 lo = read_imm8(cpu, mem);
    u16 hi = read_imm8(cpu, mem);
    return lo | (hi << 8);
}
