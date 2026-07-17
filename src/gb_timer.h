#pragma once
#include "gb_types.h"

struct Memory;

struct Timer {
    u16 div_counter = 0;
    u16 tima_counter = 0;
    u32 prev_div_bit = 0;

    void reset();
    void step(int cycles, Memory& mem);
    int cycles_to_next_event(Memory& mem) const;
};
