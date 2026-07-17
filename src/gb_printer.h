#pragma once
#include "gb_types.h"
#include <vector>

struct GBPrinter {
    int  state = 0;
    bool engaged = false;
    u8   cmd = 0, compress = 0;
    u16  len = 0, recv = 0;
    u16  checksum_in = 0, checksum_calc = 0;
    u8   status = 0;
    int  print_polls = 0;
    u8   print_palette = 0xE4;
    std::vector<u8> packet;
    std::vector<u8> tiles;

    bool print_ready = false;
    int  img_w = 0, img_h = 0;
    std::vector<u8> image;

    void reset();
    u8   exchange(u8 in);
    void run_command();
    void build_image();
};
