#include "gb_timer.h"
#include "gb_memory.h"
#include "gb_log.h"

void Timer::reset() {
    div_counter = 0;
    tima_counter = 0;
    prev_div_bit = 0;
}

int Timer::cycles_to_next_event(Memory& mem) const {
    u8 tac = mem.io_regs[TAC - 0xFF00];
    if (!(tac & 0x04)) return 0x3FFFFFFF;
    static const u8 sel_bit[4] = { 9, 3, 5, 7 };
    u32 period = 1u << (sel_bit[tac & 3] + 1);
    u32 counter = ((u32)mem.io_regs[DIV - 0xFF00] << 8) | (div_counter & 0xFF);
    u32 to_inc = period - (counter & (period - 1));
    u32 incs   = 256u - mem.io_regs[TIMA - 0xFF00];
    return (int)(to_inc + (incs - 1) * period);
}

void Timer::step(int cycles, Memory& mem) {
    u8 tac = mem.io_regs[TAC - 0xFF00];

    u32 before = ((u32)mem.io_regs[DIV - 0xFF00] << 8) | (div_counter & 0xFF);
    u32 after  = before + cycles;

    mem.io_regs[DIV - 0xFF00] = (u8)((after >> 8) & 0xFF);
    div_counter = (u16)(after & 0xFF);

    if (tac & 0x04) {
        static const u8 sel_bit[4] = { 9, 3, 5, 7 };
        u32 shift = sel_bit[tac & 3] + 1;
        u32 edges = (after >> shift) - (before >> shift);

        if (edges) {
            u8 tima = mem.io_regs[TIMA - 0xFF00];
            for (u32 e = 0; e < edges; e++) {
                if (tima == 0xFF) {
                    tima = mem.io_regs[TMA - 0xFF00];
                    mem.io_regs[IF - 0xFF00] |= INT_TIMER;
                    LOGT(LOG_TIMER, "TIMA overflow -> INT_TIMER (reload TMA=%02X)", tima);
                } else {
                    tima++;
                }
            }
            mem.io_regs[TIMA - 0xFF00] = tima;
        }
    }
}
