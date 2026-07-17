#include "gb_memory.h"
#include "gb_input.h"
#include "gb_apu.h"
#include "gb_printer.h"
#include "gb_ppu.h"
#include "gb_log.h"
#include <cstring>
#include <algorithm>

Memory::Memory() {
    reset();
}

void Memory::set_cartridge(Cartridge* c) {
    cart = c;
    cgb_mode = c && c->is_cgb();
    LOGI(LOG_MEM, "cgb_mode=%s", cgb_mode ? "ON" : "off");
    refresh_fast_pages();
}

void Memory::load_boot_rom(const unsigned char* data, int size, bool is_cgb) {
    int n = std::min(size, (int)boot_rom.size());
    std::copy(data, data + n, boot_rom.begin());
    boot_rom_active = true;
    boot_rom_is_cgb = is_cgb;
    LOGI(LOG_SYS, "boot ROM loaded (%s, %d bytes)", is_cgb ? "CGB" : "DMG", n);
    refresh_fast_pages();
}

void Memory::refresh_fast_pages() {
    fast_gen++;
    for (int p = 0; p < 16; p++) { fast_read_page[p] = nullptr; fast_write_page[p] = nullptr; }

    if (cart) {
        if (!boot_rom_active) fast_read_page[0] = cart->rom_ptr(0x0000);
        for (int p = 1; p < 8; p++) fast_read_page[p] = cart->rom_ptr((u16)(p * 0x1000));
    }

    u8* vram_active = vram_bank() ? vram_bank1.data() : vram.data();
    fast_read_page[8]  = vram_active;
    fast_read_page[9]  = vram_active + 0x1000;
    fast_write_page[8] = vram_active;
    fast_write_page[9] = vram_active + 0x1000;
    extern bool g_vram1_trace;
    if (g_vram1_trace) { fast_write_page[8] = nullptr; fast_write_page[9] = nullptr; }

    fast_read_page[0xC]  = wram.data();
    fast_write_page[0xC] = wram.data();

    u8* wram_active = wram.data() + wram_bank() * 0x1000;
    fast_read_page[0xD]  = wram_active;
    fast_write_page[0xD] = wram_active;

    if (cart) {
        for (int i = 0; i < cart->gg_code_count; i++) {
            u16 pg = cart->gg_codes[i].addr >> 12;
            if (pg < 8) fast_read_page[pg] = nullptr;
        }
    }

    int hot = 0;
    for (int p = 0; p < 16; p++) if (fast_read_page[p]) hot++;
    static int s_last_hot = -1;
    if (hot != s_last_hot) {
        s_last_hot = hot;
        LOGD(LOG_CPU, "Fast memory pages mapped: %d/16 (max ~12; A-B and E-F always slow path)", hot);
    }
}

void Memory::reset() {
    vram.fill(0);
    vram_bank1.fill(0);
    wram.fill(0);
    oam.fill(0);
    hram.fill(0);
    io_regs.fill(0);
    ie_register = 0;

    serial_cycles = 0;
    serial_tx = 0;
    serial_line_len = 0;
    stat_irq_line = false;

    vbk = 0;
    svbk = 1;
    bg_palette_ram.fill(0);
    obj_palette_ram.fill(0);
    bcps = 0;
    ocps = 0;
    cgb_pal_dirty = true;

    hdma_active = false;
    hdma_src = 0;
    hdma_dst = 0;
    hdma_blocks_left = 0;
    hdma_dst_vbk = 0;
    hdma_stall_cycles = 0;

    serial_line_len = 0;
    serial_line[0] = '\0';

    io_regs[NR10 - 0xFF00] = 0x80;
    io_regs[NR11 - 0xFF00] = 0xBF;
    io_regs[NR12 - 0xFF00] = 0xF3;
    io_regs[NR14 - 0xFF00] = 0xBF;
    io_regs[NR21 - 0xFF00] = 0x3F;
    io_regs[NR24 - 0xFF00] = 0xBF;
    io_regs[NR30 - 0xFF00] = 0x7F;
    io_regs[NR31 - 0xFF00] = 0xFF;
    io_regs[NR32 - 0xFF00] = 0x9F;
    io_regs[NR34 - 0xFF00] = 0xBF;
    io_regs[NR41 - 0xFF00] = 0xFF;
    io_regs[NR44 - 0xFF00] = 0xBF;
    io_regs[NR50 - 0xFF00] = 0x77;
    io_regs[NR51 - 0xFF00] = 0xF3;
    io_regs[NR52 - 0xFF00] = 0xF1;

    refresh_fast_pages();
}

u8 Memory::read_slow(u16 addr) {
    if (boot_rom_active) {
        if (addr < 0x0100) return boot_rom[addr];
        if (boot_rom_is_cgb && addr >= 0x0200 && addr < 0x0900) return boot_rom[addr];
    }
    if (addr < 0x8000) {
        if (cart) return cart->read(addr);
        return 0xFF;
    }
    if (addr == 0xFFFF) return ie_register;
    if (addr >= 0xFF80 && addr < 0xFFFF) return hram[addr & 0x7F];
    if (addr == 0xFF0F) return io_regs[0x0F];
    if (addr >= 0xFF04 && addr <= 0xFF07) return io_regs[addr - 0xFF00];
    if (addr < 0xA000) return (vram_bank() ? vram_bank1 : vram)[addr & 0x1FFF];
    if (addr >= 0xFF40 && addr <= 0xFF4B) {
        if (addr == 0xFF44) {
            if (ppu_ptr && batch_dots) return ppu_ptr->peek_ly(batch_dots, io_regs[0x44]);
            return io_regs[0x44];
        }
        if (addr == 0xFF41) {
            u8 s = io_regs[0x41];
            if (ppu_ptr && batch_dots)
                s = (u8)((s & 0x78) | ppu_ptr->peek_stat_bits(batch_dots, io_regs[0x44], io_regs[0x45]));
            return (u8)(0x80 | s);
        }
        return io_regs[addr - 0xFF00];
    }
    if (addr < 0xFEA0 && addr >= 0xFE00) return oam[addr & 0xFF];
    if (addr < 0xC000) {
        if (cart) return cart->read(addr);
        return 0xFF;
    }
    if (addr < 0xFE00) {
        u16 a = (addr < 0xE000) ? addr : (u16)(addr - 0x2000);
        if (a < 0xD000) return wram[a - 0xC000];
        return wram[wram_bank() * 0x1000 + (a - 0xD000)];
    }
    if (addr == 0xFF00) {
        if (input) return input->read_p1();
        return 0xFF;
    }
    if (addr == 0xFF01) return io_regs[0x01];
    if (addr == 0xFF02) return 0x7C | io_regs[0x02];
    if (addr >= 0xFF10 && addr <= 0xFF3F) return io_regs[addr - 0xFF00];
    if (addr == 0xFF4F) return 0xFE | (vbk & 1);
    if (addr == 0xFF68) return bcps;
    if (addr == 0xFF69) return bg_palette_ram[bcps & 0x3F];
    if (addr == 0xFF6A) return ocps;
    if (addr == 0xFF6B) return obj_palette_ram[ocps & 0x3F];
    if (addr == 0xFF70) return 0xF8 | (svbk & 7);
    if (addr == 0xFF4D) return 0x7E | io_regs[0x4D];
    if (addr == 0xFF55) return hdma_active ? (u8)((hdma_blocks_left - 1) & 0x7F) : 0xFF;
    if (addr >= 0xFF4F && addr <= 0xFF54) return io_regs[addr - 0xFF00];
    if (addr >= 0xFF68 && addr <= 0xFF6F) return io_regs[addr - 0xFF00];
    if (addr >= 0xFF70 && addr <= 0xFF7F) return io_regs[addr - 0xFF00];
    return 0xFF;
}

bool g_vram1_trace = false;
extern u16* g_trace_pc;
u32  g_v1_cpu[6] = {};
u32  g_v1_dma[6] = {};
u16* g_trace_pc = nullptr;
static int g_v1_logn = 0;

void Memory::serial_tick(int sys_cycles) {
    if (serial_cycles <= 0) return;
    serial_cycles -= sys_cycles;
    if (serial_cycles > 0) return;
    serial_cycles = 0;

    char c = (char)serial_tx;
    if (c == '\n' || serial_line_len >= (int)sizeof(serial_line) - 1) {
        serial_line[serial_line_len] = '\0';
        bool all_same = serial_line_len > 0;
        for (int i = 1; i < serial_line_len; i++) {
            if (serial_line[i] != serial_line[0]) { all_same = false; break; }
        }
        if (serial_line_len > 0 && !all_same) LOGI(LOG_SERIAL, "%s", serial_line);
        serial_line_len = 0;
    }
    if (c != '\n' && c != '\0') serial_line[serial_line_len++] = c;

    u8 rx = printer ? printer->exchange(serial_tx) : 0xFF;
    io_regs[SB - 0xFF00] = rx;
    io_regs[SC - 0xFF00] &= ~0x80;
    io_regs[0x0F] |= INT_SERIAL;
    LOGD(LOG_SERIAL, "TX %02X ('%c') -> RX %02X",
         serial_tx, (c >= 0x20 && c < 0x7F) ? c : '.', rx);
}

void Memory::write_slow(u16 addr, u8 val) {
    if (addr < 0x8000) {
        if (cart) cart->write(addr, val);
        refresh_fast_pages();
    } else if (addr < 0xA000) {
        if (g_vram1_trace && vram_bank() == 1 && (addr & 0x1FFF) < 0x1800) {
            g_v1_cpu[(addr & 0x1FFF) >> 10]++;
            if (g_v1_logn < 24) {
                g_v1_logn++;
                fprintf(stderr, "[V1] cpu write %04X=%02X pc=%04X\n",
                        addr, val, g_trace_pc ? *g_trace_pc : 0);
            }
        }
        (vram_bank() ? vram_bank1 : vram)[addr & 0x1FFF] = val;
    } else if (addr < 0xC000) {
        if (cart) cart->write(addr, val);
    } else if (addr < 0xFE00) {
        u16 a = (addr < 0xE000) ? addr : (u16)(addr - 0x2000);
        if (a < 0xD000) wram[a - 0xC000] = val;
        else wram[wram_bank() * 0x1000 + (a - 0xD000)] = val;
    } else if (addr < 0xFEA0) {
        oam[addr & 0xFF] = val;
    } else if (addr >= 0xFF80 && addr < 0xFFFF) {
        hram[addr & 0x7F] = val;
    } else if (addr == 0xFFFF) {
        ie_register = val;
    } else if (addr == 0xFF00) {
        if (input) input->write_p1(val);
    } else if (addr == SC) {
        io_regs[addr - 0xFF00] = val;
        if ((val & 0x80) && (val & 0x01)) {
            serial_tx = io_regs[SB - 0xFF00];
            serial_cycles = (val & 0x02) ? 128 : 4096;
        }
    } else if (addr == BGP) {
        io_regs[0x47] = val;
        if (!cgb_mode && ppu_ptr) ppu_ptr->note_bgp_write(val, batch_dots);
    } else if (addr == STAT) {
        io_regs[0x41] = (u8)((io_regs[0x41] & 0x07) | (val & 0x78));
        if (!cgb_mode && (io_regs[0x40] & 0x80) && !stat_irq_line) {
            u8 s = io_regs[0x41];
            u8 ppu_mode = s & 0x03;
            if (ppu_mode == 0 || ppu_mode == 1 || (s & 0x04))
                io_regs[0x0F] |= INT_STAT;
        }
    } else if (addr == 0xFF46) {
        static u8 s_prev_dma = 0xFF;
        if (val != s_prev_dma) { LOGD(LOG_MEM, "OAM DMA source = %02X00", val); s_prev_dma = val; }
        u16 src = (u16)val << 8;
        for (int i = 0; i < 0xA0; i++) {
            oam[i] = read(src + i);
        }
    } else if (addr == 0xFF4F) {
        u8 nv = val & 1;
        if (nv != vbk) LOGD(LOG_MEM, "VRAM bank -> %d", nv);
        vbk = nv;
        refresh_fast_pages();
    } else if (addr == 0xFF50) {
        boot_rom_active = false;
        LOGI(LOG_SYS, "boot ROM disabled, game code running");
        refresh_fast_pages();
    } else if (addr == 0xFF68) {
        bcps = val;
    } else if (addr == 0xFF69) {
        bg_palette_ram[bcps & 0x3F] = val;
        if (bcps & 0x80) bcps = 0x80 | ((bcps + 1) & 0x3F);
        cgb_pal_dirty = true;
    } else if (addr == 0xFF6A) {
        ocps = val;
    } else if (addr == 0xFF6B) {
        obj_palette_ram[ocps & 0x3F] = val;
        if (ocps & 0x80) ocps = 0x80 | ((ocps + 1) & 0x3F);
        cgb_pal_dirty = true;
    } else if (addr == 0xFF70) {
        u8 nv = val & 7;
        if (nv != svbk) LOGD(LOG_MEM, "WRAM bank -> %d", nv == 0 ? 1 : nv);
        svbk = nv;
        refresh_fast_pages();
    } else if (addr == 0xFF55) {
        u16 src = ((u16)io_regs[0x51] << 8) | (io_regs[0x52] & 0xF0);
        u16 dst = 0x8000 | (((u16)io_regs[0x53] & 0x1F) << 8) | (io_regs[0x54] & 0xF0);
        u16 blocks = (u16)(val & 0x7F) + 1;

        if (!(val & 0x80)) {
            if (hdma_active) {
                LOGD(LOG_PPU, "HDMA stop: %d block(s) left unsent (src=%04X dst=%04X)",
                    hdma_blocks_left, hdma_src, hdma_dst);
                hdma_active = false;
            } else {
                LOGD(LOG_PPU, "GDMA: src=%04X dst=%04X blocks=%d bank=%d pc=%04X",
                     src, dst, blocks, vram_bank(), g_trace_pc ? *g_trace_pc : 0);
                std::array<u8, 0x2000>& dst_bank = vram_bank() ? vram_bank1 : vram;
                u16 len = blocks * 0x10;
                for (u16 i = 0; i < len; i++) {
                    if (g_vram1_trace && vram_bank() == 1 && ((dst + i) & 0x1FFF) < 0x1800)
                        g_v1_dma[((dst + i) & 0x1FFF) >> 10]++;
                    dst_bank[(dst + i) & 0x1FFF] = read(src + i);
                }
                hdma_stall_cycles += (u32)blocks * 32;
                LOGT(LOG_PPU, "GDMA stall queued: +%u cycles (total pending %u)", blocks * 32, hdma_stall_cycles);
            }
        } else {
            hdma_active = true;
            hdma_src = src;
            hdma_dst = dst;
            hdma_blocks_left = (u8)blocks;
            hdma_dst_vbk = (u8)vram_bank();
            LOGD(LOG_PPU, "HDMA queued: src=%04X dst=%04X blocks=%d bank=%d", src, dst, blocks, hdma_dst_vbk);
        }
    } else if (addr == DIV) {
        io_regs[addr - 0xFF00] = 0;
    } else if (addr == TAC) {
        static const char* kFreqName[4] = { "4096 Hz", "262144 Hz", "65536 Hz", "16384 Hz" };
        LOGD(LOG_TIMER, "TAC=%02X (timer %s, %s)", val, (val & 0x04) ? "enabled" : "disabled", kFreqName[val & 3]);
        io_regs[addr - 0xFF00] = val;
    } else if (addr >= 0xFF10 && addr <= 0xFF3F) {
        io_regs[addr - 0xFF00] = val;
        if (apu) {
            apu->write_reg(addr, val);
            apu->update_nr52(*this);
        }
    } else if (addr >= 0xFF00 && addr < 0xFF80) {
        io_regs[addr - 0xFF00] = val;
    }
}

void Memory::hdma_hblank_step() {
    if (!hdma_active) return;

    std::array<u8, 0x2000>& dst_bank = hdma_dst_vbk ? vram_bank1 : vram;
    for (int i = 0; i < 0x10; i++) {
        if (g_vram1_trace && hdma_dst_vbk && ((hdma_dst + i) & 0x1FFF) < 0x1800)
            g_v1_dma[((hdma_dst + i) & 0x1FFF) >> 10]++;
        dst_bank[(hdma_dst + i) & 0x1FFF] = read(hdma_src + i);
    }
    hdma_src += 0x10;
    hdma_dst += 0x10;
    hdma_blocks_left--;
    if (hdma_blocks_left == 0) hdma_active = false;
}

u16 Memory::read16(u16 addr) {
    return read(addr) | (u16)(read(addr + 1) << 8);
}

void Memory::write16(u16 addr, u16 val) {
    write(addr, val & 0xFF);
    write(addr + 1, (val >> 8) & 0xFF);
}