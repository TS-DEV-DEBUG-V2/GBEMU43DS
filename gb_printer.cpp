#include "gb_printer.h"
#include "gb_log.h"

enum { PS_MAGIC1, PS_MAGIC2, PS_CMD, PS_COMPRESS, PS_LEN_LO, PS_LEN_HI,
       PS_DATA, PS_CHK_LO, PS_CHK_HI, PS_ALIVE, PS_STATUS };

void GBPrinter::reset() {
    state = PS_MAGIC1; engaged = false;
    cmd = compress = 0; len = recv = 0;
    checksum_in = checksum_calc = 0; status = 0;
    print_polls = 0;
    print_palette = 0xE4;
    packet.clear(); tiles.clear();
    print_ready = false; img_w = img_h = 0; image.clear();
}

u8 GBPrinter::exchange(u8 in) {
    u8 out = 0x00;

    switch (state) {
        case PS_MAGIC1:
            if (in == 0x88) state = PS_MAGIC2;
            break;
        case PS_MAGIC2:
            if (in == 0x33) { state = PS_CMD; engaged = true; }
            else            { state = PS_MAGIC1; }
            break;
        case PS_CMD:
            cmd = in; checksum_calc = in; state = PS_COMPRESS;
            break;
        case PS_COMPRESS:
            compress = in; checksum_calc += in; state = PS_LEN_LO;
            break;
        case PS_LEN_LO:
            len = in; checksum_calc += in; state = PS_LEN_HI;
            break;
        case PS_LEN_HI:
            len |= (u16)in << 8; checksum_calc += in;
            recv = 0; packet.clear();
            state = len ? PS_DATA : PS_CHK_LO;
            break;
        case PS_DATA:
            packet.push_back(in); checksum_calc += in;
            if (++recv >= len) state = PS_CHK_LO;
            break;
        case PS_CHK_LO:
            checksum_in = in; state = PS_CHK_HI;
            break;
        case PS_CHK_HI:
            checksum_in |= (u16)in << 8;
            run_command();
            state = PS_ALIVE;
            break;
        case PS_ALIVE:
            out = 0x81;
            state = PS_STATUS;
            break;
        case PS_STATUS:
            out = status;
            if (print_polls > 0) {
                print_polls--;
                status = (print_polls > 0) ? 0x06 : 0x04;
            } else if (status == 0x04) {
                status = 0x00;
            }
            state = PS_MAGIC1; engaged = false;
            break;
    }
    return out;
}

void GBPrinter::run_command() {
    switch (cmd) {
        case 0x01:
            tiles.clear(); status = 0x00;
            print_polls = 0;
            break;
        case 0x04:
            if (compress) {
                size_t i = 0;
                while (i < packet.size()) {
                    u8 b = packet[i++];
                    if (b & 0x80) {
                        int count = (b & 0x7F) + 2;
                        if (i >= packet.size()) break;
                        u8 v = packet[i++];
                        for (int k = 0; k < count; k++) tiles.push_back(v);
                    } else {
                        int count = b + 1;
                        for (int k = 0; k < count && i < packet.size(); k++) tiles.push_back(packet[i++]);
                    }
                }
            } else {
                tiles.insert(tiles.end(), packet.begin(), packet.end());
            }
            status = 0x00;
            break;
        case 0x02: {
            u8 sheets = packet.size() >= 1 ? packet[0] : 1;
            if (packet.size() >= 3) print_palette = packet[2];
            status = 0x08;
            print_polls = 4;

            if (sheets == 0) {
                LOGI(LOG_SERIAL, "[PRINT] feed-only PRINT (0 sheets), no image");
                break;
            }
            build_image();
            print_ready = true;
            LOGI(LOG_SERIAL, "[PRINT EMU] detected gameboy tryna print (%dx%d, %u tiles, palette %02X)",
                 img_w, img_h, (unsigned)(tiles.size() / 16), print_palette);
            break;
        }
        case 0x0F:
            break;
        default:
            break;
    }

    static const char* kName[16] = {0,"INIT","PRINT",0,"DATA",0,0,0,"BREAK",0,0,0,0,0,0,"NUL"};
    log_msg(cmd == 0x0F ? LOG_DEBUG : LOG_INFO, LOG_SERIAL,
         "[PRINT] %s cmd=%02X len=%u comp=%u chk=%s -> %u tiles, status %02X",
         (cmd < 16 && kName[cmd]) ? kName[cmd] : "?", cmd, (unsigned)len, (unsigned)compress,
         (checksum_in == checksum_calc) ? "ok" : "BAD", (unsigned)(tiles.size() / 16), status);
}

void GBPrinter::build_image() {
    int num_tiles = (int)(tiles.size() / 16);
    if (num_tiles <= 0) { img_w = img_h = 0; image.clear(); return; }
    const int tiles_w = 20;
    int rows = (num_tiles + tiles_w - 1) / tiles_w;
    img_w = tiles_w * 8;
    img_h = rows * 8;
    image.assign((size_t)img_w * img_h, 0);

    for (int t = 0; t < num_tiles; t++) {
        int tx = (t % tiles_w) * 8;
        int ty = (t / tiles_w) * 8;
        const u8* td = &tiles[(size_t)t * 16];
        for (int row = 0; row < 8; row++) {
            u8 b1 = td[row * 2], b2 = td[row * 2 + 1];
            for (int px = 0; px < 8; px++) {
                int bit = 7 - px;
                int val = ((b1 >> bit) & 1) | (((b2 >> bit) & 1) << 1);
                int shade = (print_palette >> (val * 2)) & 3;
                image[(size_t)(ty + row) * img_w + (tx + px)] = (u8)shade;
            }
        }
    }
}
