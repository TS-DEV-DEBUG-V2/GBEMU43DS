#include "gb_cpu.h"
#include "gb_log.h"
#include <cstring>

static inline void alu_add(CPU& c, u8 val) {
    u16 r = (u16)c.a + val;
    u8 f = 0;
    if ((u8)r == 0) f |= 0x80;
    if (((c.a & 0xF) + (val & 0xF)) > 0xF) f |= 0x20;
    if (r > 0xFF) f |= 0x10;
    c.a = (u8)r; c.f = f;
}
static inline void alu_adc(CPU& c, u8 val) {
    u8 cy = (c.f & 0x10) ? 1 : 0;
    u16 r = (u16)c.a + val + cy;
    u8 f = 0;
    if ((u8)r == 0) f |= 0x80;
    if (((c.a & 0xF) + (val & 0xF) + cy) > 0xF) f |= 0x20;
    if (r > 0xFF) f |= 0x10;
    c.a = (u8)r; c.f = f;
}
static inline void alu_sub(CPU& c, u8 val) {
    u8 f = 0x40;
    if (c.a == val) f |= 0x80;
    if ((c.a & 0xF) < (val & 0xF)) f |= 0x20;
    if (c.a < val) f |= 0x10;
    c.a = (u8)(c.a - val); c.f = f;
}
static inline void alu_sbc(CPU& c, u8 val) {
    u8 cy = (c.f & 0x10) ? 1 : 0;
    int r = (int)c.a - val - cy;
    u8 f = 0x40;
    if ((u8)r == 0) f |= 0x80;
    if ((c.a & 0xF) < ((val & 0xF) + cy)) f |= 0x20;
    if (r < 0) f |= 0x10;
    c.a = (u8)r; c.f = f;
}
static inline void alu_cp(CPU& c, u8 val) {
    u8 f = 0x40;
    if (c.a == val) f |= 0x80;
    if ((c.a & 0xF) < (val & 0xF)) f |= 0x20;
    if (c.a < val) f |= 0x10;
    c.f = f;
}
static inline void alu_and(CPU& c, u8 val) { c.a &= val; c.f = c.a ? 0x20 : 0xA0; }
static inline void alu_or (CPU& c, u8 val) { c.a |= val; c.f = c.a ? 0x00 : 0x80; }
static inline void alu_xor(CPU& c, u8 val) { c.a ^= val; c.f = c.a ? 0x00 : 0x80; }
static inline void alu_inc(CPU& c, u8& r) {
    r++;
    u8 f = c.f & 0x10;
    if (r == 0) f |= 0x80;
    if ((r & 0x0F) == 0) f |= 0x20;
    c.f = f;
}
static inline void alu_dec(CPU& c, u8& r) {
    r--;
    u8 f = (c.f & 0x10) | 0x40;
    if (r == 0) f |= 0x80;
    if ((r & 0x0F) == 0x0F) f |= 0x20;
    c.f = f;
}
static inline void alu_add16(CPU& c, u16 val) {
    u32 tmp = (u32)c.hl + val;
    u8 f = c.f & 0x80;
    if (((c.hl & 0xFFF) + (val & 0xFFF)) > 0xFFF) f |= 0x20;
    if (tmp > 0xFFFF) f |= 0x10;
    c.hl = (u16)tmp; c.f = f;
}

static inline __attribute__((always_inline)) void push(CPU& cpu, Memory& mem, u16 val) {
    cpu.sp -= 2;
    mem.write(cpu.sp, val & 0xFF);
    mem.write(cpu.sp + 1, (val >> 8) & 0xFF);
}

static inline __attribute__((always_inline)) u16 pop(CPU& cpu, Memory& mem) {
    u16 lo = mem.read(cpu.sp++);
    u16 hi = mem.read(cpu.sp++);
    return lo | (hi << 8);
}

static inline __attribute__((always_inline)) u8 exec_cb(CPU& cpu, Memory& mem) {
    u8 op = read_imm8(cpu, mem);
    u8 r = op & 7;
    u8 bit = (op >> 3) & 7;
    u8 x = (op >> 6) & 3;

    u8* reg8 = nullptr;
    switch (r) {
        case 0: reg8 = &cpu.b; break;
        case 1: reg8 = &cpu.c; break;
        case 2: reg8 = &cpu.d; break;
        case 3: reg8 = &cpu.e; break;
        case 4: reg8 = &cpu.h; break;
        case 5: reg8 = &cpu.l; break;
        case 6: reg8 = nullptr; break;
        case 7: reg8 = &cpu.a; break;
    }

    u8 val = reg8 ? *reg8 : mem.read(cpu.hl);

    switch (x) {
        case 0: {
            u8 result = val;
            u8 old_c = get_c(cpu) ? 1 : 0;
            u8 new_c = 0;
            u8 new_h = 0;

            switch (bit) {
                case 0: {
                    new_c = (val >> 7) & 1;
                    result = (val << 1) | new_c;
                    set_z(cpu, result == 0);
                    set_n(cpu, false);
                    set_h(cpu, false);
                    set_c(cpu, new_c);
                    break;
                }
                case 1: {
                    new_c = val & 1;
                    result = (val >> 1) | (new_c << 7);
                    set_z(cpu, result == 0);
                    set_n(cpu, false);
                    set_h(cpu, false);
                    set_c(cpu, new_c);
                    break;
                }
                case 2: {
                    new_c = (val >> 7) & 1;
                    result = (val << 1) | old_c;
                    set_z(cpu, result == 0);
                    set_n(cpu, false);
                    set_h(cpu, false);
                    set_c(cpu, new_c);
                    break;
                }
                case 3: {
                    new_c = val & 1;
                    result = (val >> 1) | (old_c << 7);
                    set_z(cpu, result == 0);
                    set_n(cpu, false);
                    set_h(cpu, false);
                    set_c(cpu, new_c);
                    break;
                }
                case 4: {
                    new_c = (val >> 7) & 1;
                    result = val << 1;
                    set_z(cpu, result == 0);
                    set_n(cpu, false);
                    set_h(cpu, false);
                    set_c(cpu, new_c);
                    break;
                }
                case 5: {
                    new_c = val & 1;
                    result = (val >> 1) | (val & 0x80);
                    set_z(cpu, result == 0);
                    set_n(cpu, false);
                    set_h(cpu, false);
                    set_c(cpu, new_c);
                    break;
                }
                case 6: {
                    result = ((val & 0x0F) << 4) | ((val >> 4) & 0x0F);
                    set_z(cpu, result == 0);
                    set_n(cpu, false);
                    set_h(cpu, false);
                    set_c(cpu, false);
                    break;
                }
                case 7: {
                    new_c = val & 1;
                    result = val >> 1;
                    set_z(cpu, result == 0);
                    set_n(cpu, false);
                    set_h(cpu, false);
                    set_c(cpu, new_c);
                    break;
                }
            }

            if (reg8) *reg8 = result;
            else mem.write(cpu.hl, result);
            return r == 6 ? 16 : 8;
        }
        case 1: {
            u8 test = (val >> bit) & 1;
            set_z(cpu, test == 0);
            set_n(cpu, false);
            set_h(cpu, true);
            return r == 6 ? 16 : 8;
        }
        case 2: {
            u8 result = val & ~(1 << bit);
            if (reg8) *reg8 = result;
            else mem.write(cpu.hl, result);
            return r == 6 ? 16 : 8;
        }
        case 3: {
            u8 result = val | (1 << bit);
            if (reg8) *reg8 = result;
            else mem.write(cpu.hl, result);
            return r == 6 ? 16 : 8;
        }
    }
    return 0;
}

void CPU::reset(bool cgb) {
    if (cgb) {
        af = 0x1180;
        bc = 0x0000;
        de = 0xFF56;
        hl = 0x000D;
    } else {
        af = 0x01B0;
        bc = 0x0013;
        de = 0x00D8;
        hl = 0x014D;
    }
    sp = 0xFFFE;
    pc = 0x0000;
    halted = false;
    stop_mode = false;
    ime = false;
    ime_scheduled = 0;
    double_speed = false;
    pc_ptr = nullptr;
    pc_end = nullptr;
    pc_gen = 0;
}

inline __attribute__((always_inline)) u8 CPU::step_core(Memory& mem) {
    if (mem.hdma_stall_cycles > 0) {
        u8 chunk = mem.hdma_stall_cycles > 255 ? 255 : (u8)mem.hdma_stall_cycles;
        mem.hdma_stall_cycles -= chunk;
        return chunk;
    }

    u8 iflag = mem.io_regs[0x0F];
    u8 ie_reg = mem.ie_register;

    if (halted) {
        if ((iflag & ie_reg) != 0) {
            halted = false;
        } else {
            if (ie_reg == 0) {
                static int cooldown = 0;
                if (cooldown <= 0) {
                    cooldown = 4000000;
                    LOGW(LOG_SYS, "FREEZE? HALT with IE=0 at pc=%04X - CPU cannot wake (no interrupt enabled)", (u16)(pc - 1));
                } else cooldown -= 4;
            }
            return 4;
        }
    }

    if (ime && (iflag & ie_reg) != 0) {
        ime = false;
        halted = false;
        static const char* kIntName[5] = { "VBLANK", "STAT", "TIMER", "SERIAL", "JOYPAD" };
        for (int i = 0; i < 5; i++) {
            if (iflag & (1 << i) & ie_reg) {
                iflag &= ~(1 << i);
                mem.io_regs[0x0F] = iflag;
                LOGT(LOG_CPU, "interrupt dispatch: %s (from pc=%04X)", kIntName[i], pc);
                push(*this, mem, pc);
                jump(0x40 + i * 8);
                return 20;
            }
        }
    }

    if (ime_scheduled > 0) {
        ime_scheduled--;
        if (ime_scheduled == 0) ime = true;
    }

    u8 op = read_imm8(*this, mem);
    u8 cycles = 4;

    switch (op) {
        case 0x00: cycles = 4; break;
        case 0x01: bc = read_imm16(*this, mem); cycles = 12; break;
        case 0x02: mem.write(bc, a); cycles = 8; break;
        case 0x03: bc++; cycles = 8; break;
        case 0x04: alu_inc(*this, b); cycles = 4; break;
        case 0x05: alu_dec(*this, b); cycles = 4; break;
        case 0x06: b = read_imm8(*this, mem); cycles = 8; break;
        case 0x07: { u8 c_bit = (a >> 7) & 1; a = (a << 1) | c_bit; set_z(*this, false); set_n(*this, false); set_h(*this, false); set_c(*this, c_bit); cycles = 4; break; }
        case 0x08: { u16 addr = read_imm16(*this, mem); mem.write(addr, sp & 0xFF); mem.write(addr + 1, (sp >> 8) & 0xFF); cycles = 20; break; }
        case 0x09: alu_add16(*this, bc); cycles = 8; break;
        case 0x0A: a = mem.read(bc); cycles = 8; break;
        case 0x0B: bc--; cycles = 8; break;
        case 0x0C: alu_inc(*this, c); cycles = 4; break;
        case 0x0D: alu_dec(*this, c); cycles = 4; break;
        case 0x0E: c = read_imm8(*this, mem); cycles = 8; break;
        case 0x0F: { u8 c_bit = a & 1; a = (a >> 1) | (c_bit << 7); set_z(*this, false); set_n(*this, false); set_h(*this, false); set_c(*this, c_bit); cycles = 4; break; }

        case 0x10: {
            read_imm8(*this, mem);
            u8& key1 = mem.io_regs[0x4D];
            if (mem.cgb_mode && (key1 & 0x01)) {
                double_speed = !double_speed;
                key1 = (key1 & ~0x01) | (double_speed ? 0x80 : 0x00);
                LOGI(LOG_CPU, "CGB speed switch -> %s speed", double_speed ? "double" : "normal");
            } else {
                halted = true;
                LOGD(LOG_CPU, "STOP (pc=%04X)", pc);
            }
            cycles = 4;
            break;
        }
        case 0x11: de = read_imm16(*this, mem); cycles = 12; break;
        case 0x12: mem.write(de, a); cycles = 8; break;
        case 0x13: de++; cycles = 8; break;
        case 0x14: alu_inc(*this, d); cycles = 4; break;
        case 0x15: alu_dec(*this, d); cycles = 4; break;
        case 0x16: d = read_imm8(*this, mem); cycles = 8; break;
        case 0x17: { u8 c_bit = get_c(*this) ? 1 : 0; u8 new_c = (a >> 7) & 1; a = (a << 1) | c_bit; set_z(*this, false); set_n(*this, false); set_h(*this, false); set_c(*this, new_c); cycles = 4; break; }
        case 0x18: { s8 offset = (s8)read_imm8(*this, mem); jump(pc + offset); cycles = 12; break; }
        case 0x19: alu_add16(*this, de); cycles = 8; break;
        case 0x1A: a = mem.read(de); cycles = 8; break;
        case 0x1B: de--; cycles = 8; break;
        case 0x1C: alu_inc(*this, e); cycles = 4; break;
        case 0x1D: alu_dec(*this, e); cycles = 4; break;
        case 0x1E: e = read_imm8(*this, mem); cycles = 8; break;
        case 0x1F: { u8 c_bit = get_c(*this) ? 1 : 0; u8 new_c = a & 1; a = (a >> 1) | (c_bit << 7); set_z(*this, false); set_n(*this, false); set_h(*this, false); set_c(*this, new_c); cycles = 4; break; }

        case 0x20: { s8 offset = (s8)read_imm8(*this, mem); if (!get_z(*this)) { jump(pc + offset); cycles = 12; } else cycles = 8; break; }
        case 0x21: hl = read_imm16(*this, mem); cycles = 12; break;
        case 0x22: { mem.write(hl++, a); cycles = 8; break; }
        case 0x23: hl++; cycles = 8; break;
        case 0x24: alu_inc(*this, h); cycles = 4; break;
        case 0x25: alu_dec(*this, h); cycles = 4; break;
        case 0x26: h = read_imm8(*this, mem); cycles = 8; break;
        case 0x27: {
            u16 correction = 0;
            if (get_h(*this) || (!get_n(*this) && (a & 0x0F) > 9)) correction |= 0x06;
            if (get_c(*this) || (!get_n(*this) && a > 0x99)) { correction |= 0x60; set_c(*this, true); }
            a = get_n(*this) ? (a - correction) : (a + correction);
            set_z(*this, a == 0); set_h(*this, false);
            cycles = 4; break;
        }
        case 0x28: { s8 offset = (s8)read_imm8(*this, mem); if (get_z(*this)) { jump(pc + offset); cycles = 12; } else cycles = 8; break; }
        case 0x29: alu_add16(*this, hl); cycles = 8; break;
        case 0x2A: { a = mem.read(hl++); cycles = 8; break; }
        case 0x2B: hl--; cycles = 8; break;
        case 0x2C: alu_inc(*this, l); cycles = 4; break;
        case 0x2D: alu_dec(*this, l); cycles = 4; break;
        case 0x2E: l = read_imm8(*this, mem); cycles = 8; break;
        case 0x2F: { a = ~a; set_n(*this, true); set_h(*this, true); cycles = 4; break; }

        case 0x30: { s8 offset = (s8)read_imm8(*this, mem); if (!get_c(*this)) { jump(pc + offset); cycles = 12; } else cycles = 8; break; }
        case 0x31: sp = read_imm16(*this, mem); cycles = 12; break;
        case 0x32: { mem.write(hl--, a); cycles = 8; break; }
        case 0x33: sp++; cycles = 8; break;
        case 0x34: { u8 val = mem.read(hl); alu_inc(*this, val); mem.write(hl, val); cycles = 12; break; }
        case 0x35: { u8 val = mem.read(hl); alu_dec(*this, val); mem.write(hl, val); cycles = 12; break; }
        case 0x36: mem.write(hl, read_imm8(*this, mem)); cycles = 12; break;
        case 0x37: set_c(*this, true); set_n(*this, false); set_h(*this, false); cycles = 4; break;
        case 0x38: { s8 offset = (s8)read_imm8(*this, mem); if (get_c(*this)) { jump(pc + offset); cycles = 12; } else cycles = 8; break; }
        case 0x39: alu_add16(*this, sp); cycles = 8; break;
        case 0x3A: { a = mem.read(hl--); cycles = 8; break; }
        case 0x3B: sp--; cycles = 8; break;
        case 0x3C: alu_inc(*this, a); cycles = 4; break;
        case 0x3D: alu_dec(*this, a); cycles = 4; break;
        case 0x3E: a = read_imm8(*this, mem); cycles = 8; break;
        case 0x3F: { set_c(*this, !get_c(*this)); set_n(*this, false); set_h(*this, false); cycles = 4; break; }

        case 0x40: b = b; cycles = 4; break;
        case 0x41: b = c; cycles = 4; break;
        case 0x42: b = d; cycles = 4; break;
        case 0x43: b = e; cycles = 4; break;
        case 0x44: b = h; cycles = 4; break;
        case 0x45: b = l; cycles = 4; break;
        case 0x46: b = mem.read(hl); cycles = 8; break;
        case 0x47: b = a; cycles = 4; break;
        case 0x48: c = b; cycles = 4; break;
        case 0x49: c = c; cycles = 4; break;
        case 0x4A: c = d; cycles = 4; break;
        case 0x4B: c = e; cycles = 4; break;
        case 0x4C: c = h; cycles = 4; break;
        case 0x4D: c = l; cycles = 4; break;
        case 0x4E: c = mem.read(hl); cycles = 8; break;
        case 0x4F: c = a; cycles = 4; break;

        case 0x50: d = b; cycles = 4; break;
        case 0x51: d = c; cycles = 4; break;
        case 0x52: d = d; cycles = 4; break;
        case 0x53: d = e; cycles = 4; break;
        case 0x54: d = h; cycles = 4; break;
        case 0x55: d = l; cycles = 4; break;
        case 0x56: d = mem.read(hl); cycles = 8; break;
        case 0x57: d = a; cycles = 4; break;
        case 0x58: e = b; cycles = 4; break;
        case 0x59: e = c; cycles = 4; break;
        case 0x5A: e = d; cycles = 4; break;
        case 0x5B: e = e; cycles = 4; break;
        case 0x5C: e = h; cycles = 4; break;
        case 0x5D: e = l; cycles = 4; break;
        case 0x5E: e = mem.read(hl); cycles = 8; break;
        case 0x5F: e = a; cycles = 4; break;

        case 0x60: h = b; cycles = 4; break;
        case 0x61: h = c; cycles = 4; break;
        case 0x62: h = d; cycles = 4; break;
        case 0x63: h = e; cycles = 4; break;
        case 0x64: h = h; cycles = 4; break;
        case 0x65: h = l; cycles = 4; break;
        case 0x66: h = mem.read(hl); cycles = 8; break;
        case 0x67: h = a; cycles = 4; break;
        case 0x68: l = b; cycles = 4; break;
        case 0x69: l = c; cycles = 4; break;
        case 0x6A: l = d; cycles = 4; break;
        case 0x6B: l = e; cycles = 4; break;
        case 0x6C: l = h; cycles = 4; break;
        case 0x6D: l = l; cycles = 4; break;
        case 0x6E: l = mem.read(hl); cycles = 8; break;
        case 0x6F: l = a; cycles = 4; break;

        case 0x70: mem.write(hl, b); cycles = 8; break;
        case 0x71: mem.write(hl, c); cycles = 8; break;
        case 0x72: mem.write(hl, d); cycles = 8; break;
        case 0x73: mem.write(hl, e); cycles = 8; break;
        case 0x74: mem.write(hl, h); cycles = 8; break;
        case 0x75: mem.write(hl, l); cycles = 8; break;
        case 0x76: halted = true; LOGT(LOG_CPU, "HALT (pc=%04X)", pc); cycles = 4; break;
        case 0x77: mem.write(hl, a); cycles = 8; break;

        case 0x78: a = b; cycles = 4; break;
        case 0x79: a = c; cycles = 4; break;
        case 0x7A: a = d; cycles = 4; break;
        case 0x7B: a = e; cycles = 4; break;
        case 0x7C: a = h; cycles = 4; break;
        case 0x7D: a = l; cycles = 4; break;
        case 0x7E: a = mem.read(hl); cycles = 8; break;
        case 0x7F: a = a; cycles = 4; break;

        case 0x80: alu_add(*this, b); cycles = 4; break;
        case 0x81: alu_add(*this, c); cycles = 4; break;
        case 0x82: alu_add(*this, d); cycles = 4; break;
        case 0x83: alu_add(*this, e); cycles = 4; break;
        case 0x84: alu_add(*this, h); cycles = 4; break;
        case 0x85: alu_add(*this, l); cycles = 4; break;
        case 0x86: alu_add(*this, mem.read(hl)); cycles = 8; break;
        case 0x87: alu_add(*this, a); cycles = 4; break;

        case 0x88: alu_adc(*this, b); cycles = 4; break;
        case 0x89: alu_adc(*this, c); cycles = 4; break;
        case 0x8A: alu_adc(*this, d); cycles = 4; break;
        case 0x8B: alu_adc(*this, e); cycles = 4; break;
        case 0x8C: alu_adc(*this, h); cycles = 4; break;
        case 0x8D: alu_adc(*this, l); cycles = 4; break;
        case 0x8E: alu_adc(*this, mem.read(hl)); cycles = 8; break;
        case 0x8F: alu_adc(*this, a); cycles = 4; break;

        case 0x90: alu_sub(*this, b); cycles = 4; break;
        case 0x91: alu_sub(*this, c); cycles = 4; break;
        case 0x92: alu_sub(*this, d); cycles = 4; break;
        case 0x93: alu_sub(*this, e); cycles = 4; break;
        case 0x94: alu_sub(*this, h); cycles = 4; break;
        case 0x95: alu_sub(*this, l); cycles = 4; break;
        case 0x96: alu_sub(*this, mem.read(hl)); cycles = 8; break;
        case 0x97: alu_sub(*this, a); cycles = 4; break;

        case 0x98: alu_sbc(*this, b); cycles = 4; break;
        case 0x99: alu_sbc(*this, c); cycles = 4; break;
        case 0x9A: alu_sbc(*this, d); cycles = 4; break;
        case 0x9B: alu_sbc(*this, e); cycles = 4; break;
        case 0x9C: alu_sbc(*this, h); cycles = 4; break;
        case 0x9D: alu_sbc(*this, l); cycles = 4; break;
        case 0x9E: alu_sbc(*this, mem.read(hl)); cycles = 8; break;
        case 0x9F: alu_sbc(*this, a); cycles = 4; break;

        case 0xA0: alu_and(*this, b); cycles = 4; break;
        case 0xA1: alu_and(*this, c); cycles = 4; break;
        case 0xA2: alu_and(*this, d); cycles = 4; break;
        case 0xA3: alu_and(*this, e); cycles = 4; break;
        case 0xA4: alu_and(*this, h); cycles = 4; break;
        case 0xA5: alu_and(*this, l); cycles = 4; break;
        case 0xA6: alu_and(*this, mem.read(hl)); cycles = 8; break;
        case 0xA7: alu_and(*this, a); cycles = 4; break;

        case 0xA8: alu_xor(*this, b); cycles = 4; break;
        case 0xA9: alu_xor(*this, c); cycles = 4; break;
        case 0xAA: alu_xor(*this, d); cycles = 4; break;
        case 0xAB: alu_xor(*this, e); cycles = 4; break;
        case 0xAC: alu_xor(*this, h); cycles = 4; break;
        case 0xAD: alu_xor(*this, l); cycles = 4; break;
        case 0xAE: alu_xor(*this, mem.read(hl)); cycles = 8; break;
        case 0xAF: alu_xor(*this, a); cycles = 4; break;

        case 0xB0: alu_or(*this, b); cycles = 4; break;
        case 0xB1: alu_or(*this, c); cycles = 4; break;
        case 0xB2: alu_or(*this, d); cycles = 4; break;
        case 0xB3: alu_or(*this, e); cycles = 4; break;
        case 0xB4: alu_or(*this, h); cycles = 4; break;
        case 0xB5: alu_or(*this, l); cycles = 4; break;
        case 0xB6: alu_or(*this, mem.read(hl)); cycles = 8; break;
        case 0xB7: alu_or(*this, a); cycles = 4; break;

        case 0xB8: alu_cp(*this, b); cycles = 4; break;
        case 0xB9: alu_cp(*this, c); cycles = 4; break;
        case 0xBA: alu_cp(*this, d); cycles = 4; break;
        case 0xBB: alu_cp(*this, e); cycles = 4; break;
        case 0xBC: alu_cp(*this, h); cycles = 4; break;
        case 0xBD: alu_cp(*this, l); cycles = 4; break;
        case 0xBE: alu_cp(*this, mem.read(hl)); cycles = 8; break;
        case 0xBF: alu_cp(*this, a); cycles = 4; break;

        case 0xC0: { if (!get_z(*this)) { jump(pop(*this, mem)); cycles = 20; } else cycles = 8; break; }
        case 0xC1: bc = pop(*this, mem); cycles = 12; break;
        case 0xC2: { u16 addr = read_imm16(*this, mem); if (!get_z(*this)) { jump(addr); cycles = 16; } else cycles = 12; break; }
        case 0xC3: jump(read_imm16(*this, mem)); cycles = 16; break;
        case 0xC4: { u16 addr = read_imm16(*this, mem); if (!get_z(*this)) { push(*this, mem, pc); jump(addr); cycles = 24; } else cycles = 12; break; }
        case 0xC5: push(*this, mem, bc); cycles = 16; break;
        case 0xC6: alu_add(*this, read_imm8(*this, mem)); cycles = 8; break;
        case 0xC7: push(*this, mem, pc); jump(0x00); cycles = 16; break;
        case 0xC8: { if (get_z(*this)) { jump(pop(*this, mem)); cycles = 20; } else cycles = 8; break; }
        case 0xC9: jump(pop(*this, mem)); cycles = 16; break;
        case 0xCA: { u16 addr = read_imm16(*this, mem); if (get_z(*this)) { jump(addr); cycles = 16; } else cycles = 12; break; }
        case 0xCB: cycles = exec_cb(*this, mem); break;
        case 0xCC: { u16 addr = read_imm16(*this, mem); if (get_z(*this)) { push(*this, mem, pc); jump(addr); cycles = 24; } else cycles = 12; break; }
        case 0xCD: { u16 addr = read_imm16(*this, mem); push(*this, mem, pc); jump(addr); cycles = 24; break; }
        case 0xCE: alu_adc(*this, read_imm8(*this, mem)); cycles = 8; break;
        case 0xCF: push(*this, mem, pc); jump(0x08); cycles = 16; break;

        case 0xD0: { if (!get_c(*this)) { jump(pop(*this, mem)); cycles = 20; } else cycles = 8; break; }
        case 0xD1: de = pop(*this, mem); cycles = 12; break;
        case 0xD2: { u16 addr = read_imm16(*this, mem); if (!get_c(*this)) { jump(addr); cycles = 16; } else cycles = 12; break; }
        case 0xD4: { u16 addr = read_imm16(*this, mem); if (!get_c(*this)) { push(*this, mem, pc); jump(addr); cycles = 24; } else cycles = 12; break; }
        case 0xD5: push(*this, mem, de); cycles = 16; break;
        case 0xD6: alu_sub(*this, read_imm8(*this, mem)); cycles = 8; break;
        case 0xD7: push(*this, mem, pc); jump(0x10); cycles = 16; break;
        case 0xD8: { if (get_c(*this)) { jump(pop(*this, mem)); cycles = 20; } else cycles = 8; break; }
        case 0xD9: { jump(pop(*this, mem)); ime = true; cycles = 16; break; }
        case 0xDA: { u16 addr = read_imm16(*this, mem); if (get_c(*this)) { jump(addr); cycles = 16; } else cycles = 12; break; }
        case 0xDC: { u16 addr = read_imm16(*this, mem); if (get_c(*this)) { push(*this, mem, pc); jump(addr); cycles = 24; } else cycles = 12; break; }
        case 0xDE: alu_sbc(*this, read_imm8(*this, mem)); cycles = 8; break;
        case 0xDF: push(*this, mem, pc); jump(0x18); cycles = 16; break;

        case 0xE0: { u8 offset = read_imm8(*this, mem); mem.write(0xFF00 + offset, a); cycles = 12; break; }
        case 0xE1: hl = pop(*this, mem); cycles = 12; break;
        case 0xE2: mem.write(0xFF00 + c, a); cycles = 8; break;
        case 0xE5: push(*this, mem, hl); cycles = 16; break;
        case 0xE6: alu_and(*this, read_imm8(*this, mem)); cycles = 8; break;
        case 0xE7: push(*this, mem, pc); jump(0x20); cycles = 16; break;
        case 0xE8: { s8 offset = (s8)read_imm8(*this, mem); u32 r = sp + offset; set_z(*this, false); set_n(*this, false); set_h(*this, (sp & 0xF) + (offset & 0xF) > 0xF); set_c(*this, (sp & 0xFF) + (offset & 0xFF) > 0xFF); sp = (u16)r; cycles = 16; break; }
        case 0xE9: jump(hl); cycles = 4; break;
        case 0xEA: { u16 addr = read_imm16(*this, mem); mem.write(addr, a); cycles = 16; break; }
        case 0xEE: alu_xor(*this, read_imm8(*this, mem)); cycles = 8; break;
        case 0xEF: push(*this, mem, pc); jump(0x28); cycles = 16; break;

        case 0xF0: { u8 offset = read_imm8(*this, mem); a = mem.read(0xFF00 + offset); cycles = 12; break; }
        case 0xF1: af = pop(*this, mem) & 0xFFF0; cycles = 12; break;
        case 0xF2: a = mem.read(0xFF00 + c); cycles = 8; break;
        case 0xF3: ime = false; ime_scheduled = 0; cycles = 4; break;
        case 0xF5: push(*this, mem, af); cycles = 16; break;
        case 0xF6: alu_or(*this, read_imm8(*this, mem)); cycles = 8; break;
        case 0xF7: push(*this, mem, pc); jump(0x30); cycles = 16; break;
        case 0xF8: { s8 offset = (s8)read_imm8(*this, mem); u32 r = sp + offset; set_z(*this, false); set_n(*this, false); set_h(*this, (sp & 0xF) + (offset & 0xF) > 0xF); set_c(*this, (sp & 0xFF) + (offset & 0xFF) > 0xFF); hl = (u16)r; cycles = 12; break; }
        case 0xF9: sp = hl; cycles = 8; break;
        case 0xFA: { u16 addr = read_imm16(*this, mem); a = mem.read(addr); cycles = 16; break; }
        case 0xFB: ime_scheduled = 1; cycles = 4; break;
        case 0xFE: alu_cp(*this, read_imm8(*this, mem)); cycles = 8; break;
        case 0xFF: push(*this, mem, pc); jump(0x38); cycles = 16; break;

        default:
            LOGW(LOG_CPU, "illegal opcode %02X at pc=%04X (treated as NOP)", op, (u16)(pc - 1));
            cycles = 4;
            break;
    }

    return cycles;
}

u8 CPU::tick(Memory& mem, PPU&, Timer&, Input&, APU&) {
    mem.batch_dots = 0;
    return step_core(mem);
}

int CPU::run_batch(Memory& mem, int budget, int& out_sys) {
    int done_dots = 0, done_sys = 0;
    while (done_dots < budget) {
        mem.batch_dots = done_dots;
        u8 c = step_core(mem);
        done_sys  += c;
        done_dots += double_speed ? (c >> 1) : c;
    }
    out_sys = done_sys;
    return done_dots;
}
