#include "gb_input.h"

void Input::key_down(Button b) {
    buttons[b] = true;
}

void Input::key_up(Button b) {
    buttons[b] = false;
}

u8 Input::read_p1() {
    u8 result = 0xFF;
    u8 select = p1_state & 0x30;

    if (!(select & 0x10)) {
        if (buttons[RIGHT])  result &= ~(1 << 0);
        if (buttons[LEFT])   result &= ~(1 << 1);
        if (buttons[UP])     result &= ~(1 << 2);
        if (buttons[DOWN])   result &= ~(1 << 3);
    }
    if (!(select & 0x20)) {
        if (buttons[A])      result &= ~(1 << 0);
        if (buttons[B])      result &= ~(1 << 1);
        if (buttons[SELECT]) result &= ~(1 << 2);
        if (buttons[START])  result &= ~(1 << 3);
    }

    return (result & 0x0F) | (p1_state & 0x30) | 0xC0;
}

void Input::write_p1(u8 val) {
    p1_state = (p1_state & 0xCF) | (val & 0x30);
}
