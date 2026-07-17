#pragma once
#include "gb_types.h"

struct Input {
    bool buttons[8] = {};
    u8 p1_state = 0xFF;

    enum Button {
        RIGHT  = 0,
        LEFT   = 1,
        UP     = 2,
        DOWN   = 3,
        A      = 4,
        B      = 5,
        SELECT = 6,
        START  = 7,
    };

    void key_down(Button b);
    void key_up(Button b);
    u8 read_p1();
    void write_p1(u8 val);
};
