#include "gb_cpu.h"
#include "gb_memory.h"
#include "gb_ppu.h"
#include "gb_timer.h"
#include "gb_input.h"
#include "gb_apu.h"
#include "gb_cartridge.h"
#include "gb_printer.h"
#include "bios.h"
#include "bios4gbc.h"
#include "gb_log.h"

#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <string>
#ifndef __EMSCRIPTEN__
#include <filesystem>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef _WIN32
#include <windows.h>
static void enable_ansi_console() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return;
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#else
static void enable_ansi_console() {}
#endif

static const char* level_color(LogLevel level) {
    switch (level) {
        case LOG_TRACE: return "\x1b[90m";
        case LOG_DEBUG: return "\x1b[36m";
        case LOG_INFO:  return "\x1b[37m";
        case LOG_WARN:  return "\x1b[33m";
        case LOG_ERROR: return "\x1b[91m";
    }
    return "\x1b[37m";
}

static const char* cat_color(LogCat cat) {
    switch (cat) {
        case LOG_SYS:    return "\x1b[35m";
        case LOG_CPU:    return "\x1b[32m";
        case LOG_MEM:    return "\x1b[34m";
        case LOG_TIMER:  return "\x1b[36m";
        case LOG_PPU:    return "\x1b[95m";
        case LOG_APU:    return "\x1b[93m";
        case LOG_CART:   return "\x1b[92m";
        case LOG_SERIAL: return "\x1b[96m";
        default:         return "\x1b[37m";
    }
}

static void desktop_log_sink(LogLevel level, LogCat cat, const char* text) {
    fprintf(stdout, "%s%-5s\x1b[0m %s%-6s\x1b[0m %s%s\x1b[0m\n",
        level_color(level), log_level_name(level),
        cat_color(cat), log_cat_name(cat),
        level >= LOG_WARN ? level_color(level) : "", text);
    fflush(stdout);
}

static CPU cpu;
static Memory mem;
static PPU ppu;
static Timer timer;
static Input input;
static APU apu;
static Cartridge cart;
static GBPrinter printer;

static SDL_Window* window = nullptr;
static SDL_Renderer* renderer = nullptr;
static SDL_Texture* texture = nullptr;
static SDL_AudioDeviceID audio_dev = 0;
static bool running = true;

static const int TARGET_FPS = 60;
static const int FRAME_MS = 1000 / TARGET_FPS;
static u32 frame_start_ms = 0;

static std::string save_path;
static std::string save_key;
static int save_timer = 0;

static const int SAVE_INTERVAL = 300;

static void persist_save_js() {
#ifdef __EMSCRIPTEN__
    if (save_key.empty()) return;
    EM_ASM({
        try {
            var path = UTF8ToString($0);
            var key = UTF8ToString($1);
            var data = FS.readFile(path);
            var bin = '';
            for (var i = 0; i < data.length; i++) bin += String.fromCharCode(data[i]);
            localStorage.setItem(key, btoa(bin));
        } catch(e) {}
    }, save_path.c_str(), save_key.c_str());
#endif
}

static bool save_ram_to_file() {
    if (cart.ram_size() == 0) return false;
    cart.save_ram(save_path);
    persist_save_js();
    return true;
}

static bool load_ram_from_file() {
    if (cart.ram_size() == 0) return false;
    return cart.load_ram(save_path);
}

static Input::Button sdl_to_gb(SDL_Keycode key) {
    switch (key) {
        case SDLK_RIGHT:  return Input::RIGHT;
        case SDLK_LEFT:   return Input::LEFT;
        case SDLK_UP:     return Input::UP;
        case SDLK_DOWN:   return Input::DOWN;
        case SDLK_z:      return Input::A;
        case SDLK_x:      return Input::B;
        case SDLK_RETURN: return Input::START;
        case SDLK_RSHIFT: return Input::SELECT;
        case SDLK_LSHIFT: return Input::SELECT;
        default:          return (Input::Button)-1;
    }
}

static void handle_event(const SDL_Event& e) {
    if (e.type == SDL_QUIT) {
        running = false;
        return;
    }
    if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_RESIZED) {
        int w = e.window.data1;
        int h = e.window.data2;
        int fit_w = h * SCREEN_W / SCREEN_H;
        int fit_h = w * SCREEN_H / SCREEN_W;
        if (fit_w <= w) {
            int vw = fit_w;
            int vh = h;
            int vx = (w - vw) / 2;
            int vy = 0;
            SDL_Rect viewport = { vx, vy, vw, vh };
            SDL_RenderSetViewport(renderer, &viewport);
        } else {
            int vw = w;
            int vh = fit_h;
            int vx = 0;
            int vy = (h - vh) / 2;
            SDL_Rect viewport = { vx, vy, vw, vh };
            SDL_RenderSetViewport(renderer, &viewport);
        }
        return;
    }
    if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
        Input::Button b = sdl_to_gb(e.key.keysym.sym);
        if (b != (Input::Button)-1) {
            if (e.type == SDL_KEYDOWN) {
                input.key_down(b);
                mem.io_regs[0x0F] |= INT_JOYPAD;
            } else {
                input.key_up(b);
            }
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
            running = false;
        }
    }
}

static void run_frame() {
    u64 target_cycles = 70224;
    u64 total = 0;

    while (total < target_cycles) {
        u8 cycles = cpu.tick(mem, ppu, timer, input, apu);
        timer.step(cycles, mem);
        mem.serial_tick(cycles);
        u8 dot_cycles = cpu.double_speed ? (u8)(cycles / 2) : cycles;
        ppu.step(dot_cycles, mem, (u8*)&mem.io_regs[0x0F]);
        apu.step(dot_cycles, mem);
        if (cart.has_camera) cart.camera_tick(dot_cycles);
        total += dot_cycles;
    }

    if (texture) {
        SDL_UpdateTexture(texture, nullptr, ppu.fb_done, SCREEN_W * sizeof(u32));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }

    if (audio_dev && apu.buffer_pos > 0) {
        SDL_QueueAudio(audio_dev, apu.audio_buffer.data(), apu.buffer_pos * sizeof(s16));
        apu.buffer_pos = 0;
    }

    save_timer++;
    if (cart.dirty && save_timer >= SAVE_INTERVAL) {
        save_timer = 0;
        cart.dirty = false;
        save_ram_to_file();
    }

    u32 now = SDL_GetTicks();
    u32 elapsed = now - frame_start_ms;
    if (elapsed < FRAME_MS) {
        SDL_Delay(FRAME_MS - elapsed);
    }
    frame_start_ms = SDL_GetTicks();
}

#ifdef __EMSCRIPTEN__
static void emscripten_loop() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) handle_event(e);
    run_frame();
}
#endif

static bool init_sdl() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    int win_flags = SDL_WINDOW_SHOWN;
#ifndef __EMSCRIPTEN__
    win_flags |= SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#endif
    window = SDL_CreateWindow("gb-emu by TS ",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W * 3, SCREEN_H * 3,
        win_flags);

    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    int render_flags = SDL_RENDERER_ACCELERATED;
#ifndef __EMSCRIPTEN__
    render_flags |= SDL_RENDERER_PRESENTVSYNC;
#endif
    renderer = SDL_CreateRenderer(window, -1, render_flags);

    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }

    texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING,
        SCREEN_W, SCREEN_H);

    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 2048;
    want.callback = nullptr;

    audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio_dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    } else {
        SDL_PauseAudioDevice(audio_dev, 0);
    }

    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "0");
    return true;
}

int main(int argc, char* argv[]) {
    enable_ansi_console();
    log_set_sink(desktop_log_sink);
    log_set_min_level(LOG_INFO);
    log_print_banner("Game Boy Emu by TS");

    if (argc < 2) {
        fprintf(stderr, "Usage: gbemu <rom.gb>\n");
        return 1;
    }

    if (!cart.load(argv[1])) {
        fprintf(stderr, "Failed to load ROM: %s\n", argv[1]);
        return 1;
    }

    if (cart.is_cgb()) {
        mem.load_boot_rom(cgb_boot_data, cgb_boot_size, true);
    } else {
        mem.load_boot_rom(dmg_boot_data, dmg_boot_size, false);
    }

#ifdef __EMSCRIPTEN__
    save_path = argc > 2 ? argv[2] : "save.sav";
    save_key = argc > 3 ? argv[3] : "gb_save_data";
    if (cart.ram_size() > 0) load_ram_from_file();
#else
    std::string rom_path(argv[1]);
    save_path = rom_path + ".sav";
    if (std::filesystem::exists(save_path)) {
        load_ram_from_file();
    }
#endif

    if (const char* dump_dir = getenv("GBDUMP")) {
        cpu.reset(cart.is_cgb());
        mem.reset(); ppu.reset(); timer.reset(); apu.reset();
        mem.set_cartridge(&cart); mem.set_input(&input); mem.set_apu(&apu);
        printer.reset(); mem.set_printer(&printer); mem.set_ppu(&ppu);
        const char* nf = getenv("GBDUMPFRAMES");
        long frames = nf ? atol(nf) : 600;
        static long vram_at2 = getenv("GBVRAMDUMP") ? atol(getenv("GBVRAMDUMP")) : -1;
        long mash = getenv("GBMASH") ? atol(getenv("GBMASH")) : 200;
        extern bool g_mbc_trace;
        extern u32 g_mbc_anomaly;
        long mbc_a = -1, mbc_b = -1;
        if (const char* mt = getenv("GBMBCTRACE")) sscanf(mt, "%ld,%ld", &mbc_a, &mbc_b);
        const int RING = 4096;
        static u32 ring_pc[RING]; static u8 ring_op[RING], ring_bk[RING];
        int rp = 0; bool want_ring = getenv("GBRING") != nullptr;
        long tr_a = -1, tr_b = -1;
        if (const char* tf = getenv("GBTRACEFR")) sscanf(tf, "%ld,%ld", &tr_a, &tr_b);
        extern bool g_vram1_trace; extern u32 g_v1_cpu[6], g_v1_dma[6]; extern u16* g_trace_pc;
        long v1_a = -1, v1_b = -1;
        if (const char* vf = getenv("GBV1TRACE")) sscanf(vf, "%ld,%ld", &v1_a, &v1_b);
        g_trace_pc = &cpu.pc;
        for (long i = 0; i < frames; i++) {
            g_mbc_trace = (i >= mbc_a && i <= mbc_b);
            if (tr_a >= 0) log_set_min_level((i >= tr_a && i <= tr_b) ? LOG_TRACE : LOG_INFO);
            if (v1_a >= 0) {
                bool want = (i >= v1_a && i <= v1_b);
                if (want != g_vram1_trace) {
                    g_vram1_trace = want;
                    mem.refresh_fast_pages();
                    if (!want) {
                        fprintf(stderr, "[V1] block writes 8000/8400/8800/8C00/9000/9400 (bank1):\n");
                        fprintf(stderr, "[V1] cpu: %lu %lu %lu %lu %lu %lu\n",
                            (unsigned long)g_v1_cpu[0],(unsigned long)g_v1_cpu[1],(unsigned long)g_v1_cpu[2],
                            (unsigned long)g_v1_cpu[3],(unsigned long)g_v1_cpu[4],(unsigned long)g_v1_cpu[5]);
                        fprintf(stderr, "[V1] dma: %lu %lu %lu %lu %lu %lu\n",
                            (unsigned long)g_v1_dma[0],(unsigned long)g_v1_dma[1],(unsigned long)g_v1_dma[2],
                            (unsigned long)g_v1_dma[3],(unsigned long)g_v1_dma[4],(unsigned long)g_v1_dma[5]);
                    }
                }
            }
            input.key_up(Input::START); input.key_up(Input::A);
            input.key_up(Input::RIGHT);
            if (mash < 0) {
            } else if (i < mash) {
                if ((i / 10) & 1) input.key_down(Input::START);
                else if ((i / 5) & 1) input.key_down(Input::A);
            } else {
                input.key_down(Input::RIGHT);
                if ((i % 40) < 6) input.key_down(Input::A);
            }
            static int use_batch = getenv("GBBATCH") ? atoi(getenv("GBBATCH")) : 0;
            if (use_batch) {
                u8* iflag = (u8*)&mem.io_regs[0x0F];
                int total = 0;
                while (total < 70224) {
                    int budget = ppu.cycles_to_next_event();
                    if (mem.io_regs[0x07] & 0x04) {
                        int tmr_sys  = timer.cycles_to_next_event(mem);
                        int tmr_dots = cpu.double_speed ? (tmr_sys >> 1) : tmr_sys;
                        if (tmr_dots < budget) budget = tmr_dots;
                    }
                    int rem = 70224 - total;
                    if (budget > rem) budget = rem;
                    if (budget < 1) budget = 1;
                    int done_sys = 0;
                    int done_dots = cpu.run_batch(mem, budget, done_sys);
                    timer.step(done_sys, mem);
                    mem.serial_tick(done_sys);
                    ppu.step(done_dots, mem, iflag);
                    apu.step(done_dots, mem);
                    if (cart.has_camera) cart.camera_tick(done_dots);
                    total += done_dots;
                }
            } else {
            u64 t = 0;
            u8 prev_ly = 0xFF, prev_lcdc = 0xFF;
            while (t < 70224) {
                if (want_ring) {
                    ring_pc[rp] = cpu.pc;
                    ring_op[rp] = mem.read(cpu.pc);
                    ring_bk[rp] = (u8)cart.rom_bank;
                    rp = (rp + 1) % RING;
                }
                static bool sblog = getenv("GBSBLOG") != nullptr;
                static int sbn = 0;
                if (sblog) {
                    if (cpu.pc == 0x39B8) {
                        fprintf(stderr, "[SB] f%05ld DROP! queue full (src=%04X)\n", i, cpu.de);
                    } else if (cpu.pc == 0x3A52 && sbn < 400) {
                        sbn++;
                        fprintf(stderr, "[SB] f%05ld BAIL LY=%u queue=%u\n",
                            i, mem.io_regs[0x44], mem.read(0xC996));
                    } else if (cpu.pc == 0x39B1 && sbn < 400) {
                        u16 ret = mem.read(cpu.sp) | (mem.read(cpu.sp + 1) << 8);
                        int vbk1 = (cpu.de >> 15) & 1;
                        if (vbk1 || i >= 1300) {
                            sbn++;
                            fprintf(stderr, "[SB] f%05ld QADD ret=%04X src=%04X dst=%04X rbank=%u blocks=%u vbk=%d\n",
                                i, ret, cpu.hl, (u16)(cpu.de | 0x8000), cpu.b, cpu.c, vbk1);
                        }
                    }
                }
                static u32 waitpc = getenv("GBWAITLOG") ? strtoul(getenv("GBWAITLOG"), 0, 16) : 0;
                static int waitlog_n = 0;
                static u16 last_ret = 0xFFFF; static u8 last_b = 0xFF;
                if (waitpc && cpu.pc == waitpc && waitlog_n < 60) {
                    u16 ret = mem.read(cpu.sp) | (mem.read(cpu.sp + 1) << 8);
                    if (ret != last_ret || cpu.b != last_b) {
                        last_ret = ret; last_b = cpu.b; waitlog_n++;
                        fprintf(stderr, "[WAIT] f%05ld wait LY==%3d caller=%04X LY=%3d LCDC=%02X bank=%d\n",
                            i, cpu.b, ret, mem.io_regs[0x44], mem.io_regs[0x40], cart.rom_bank);
                    }
                }
                u8 cy = cpu.tick(mem, ppu, timer, input, apu);
                static long ring_at = getenv("GBRINGAT") ? atol(getenv("GBRINGAT")) : -1;
                if (want_ring && i == ring_at && t >= 70000) ring_at = -2, g_mbc_anomaly = 1;
                if (want_ring && g_mbc_anomaly == 1) {
                    g_mbc_anomaly = 2;
                    fprintf(stderr, "[RING] anomaly at frame %ld - last %d instructions:\n", i, RING);
                    for (int k = 0; k < RING; k++) {
                        int idx = (rp + k) % RING;
                        fprintf(stderr, "%04X %02X b%d\n", ring_pc[idx], ring_op[idx], ring_bk[idx]);
                    }
                }
                timer.step(cy, mem);
                mem.serial_tick(cy);
                u8 d = cpu.double_speed ? (u8)(cy / 2) : cy;
                ppu.step(d, mem, (u8*)&mem.io_regs[0x0F]);
                apu.step(d, mem);
                if (cart.has_camera) cart.camera_tick(d);
                t += d;
                if (i == vram_at2) {
                    u8 ly = mem.io_regs[0x44], lcdc = mem.io_regs[0x40];
                    static u8 pv[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
                    static const u8 regs[6] = {0x40,0x42,0x43,0x47,0x4A,0x4B};
                    static const char* nm[6] = {"LCDC","SCY","SCX","BGP","WY","WX"};
                    for (int r = 0; r < 6; r++) {
                        u8 v = mem.io_regs[regs[r]];
                        if (v != pv[r]) {
                            fprintf(stderr, "[TRACE] LY=%3u %s %02X->%02X (LYC=%u STAT=%02X)\n",
                                ly, nm[r], pv[r], v, mem.io_regs[0x45], mem.io_regs[0x41]);
                            pv[r] = v;
                        }
                    }
                    (void)lcdc; (void)prev_lcdc;
                    if (ly != prev_ly) prev_ly = ly;
                }
            }
            }
            static long vram_at = getenv("GBVRAMDUMP") ? atol(getenv("GBVRAMDUMP")) : -1;
            if (i == vram_at) {
                char vp[512]; snprintf(vp, sizeof(vp), "%s/vram.bin", dump_dir);
                if (FILE* vf = fopen(vp, "wb")) {
                    fwrite(mem.vram.data(), 1, mem.vram.size(), vf);
                    fclose(vf);
                }
                snprintf(vp, sizeof(vp), "%s/vram1.bin", dump_dir);
                if (FILE* vf = fopen(vp, "wb")) {
                    fwrite(mem.vram_bank1.data(), 1, mem.vram_bank1.size(), vf);
                    fclose(vf);
                }
                fprintf(stderr, "[VRAM] tilemap 0x9800 at frame %ld:\n", i);
                for (int r = 0; r < 32; r++) {
                    char lineb[3*32 + 16]; int p = 0;
                    p += snprintf(lineb + p, sizeof(lineb) - p, "%2d: ", r);
                    for (int c = 0; c < 32; c++)
                        p += snprintf(lineb + p, sizeof(lineb) - p, "%02X ", mem.vram[0x1800 + r*32 + c]);
                    fprintf(stderr, "%s\n", lineb);
                }
            }
            {
                const u32* px = ppu.fb_done;
                bool uniform = true;
                for (int k = 1; k < SCREEN_W * SCREEN_H; k++)
                    if (px[k] != px[0]) { uniform = false; break; }
                if (uniform && i > 60 && (i % 30) == 0)
                    fprintf(stderr, "[BLANK] f%05ld col=%08X PC=%04X halted=%d IME=%d IE=%02X IF=%02X "
                        "LCDC=%02X STAT=%02X LYC=%3u BGP=%02X SCY=%d SCX=%d bank=%d\n",
                        i, px[0], cpu.pc, (int)cpu.halted, (int)cpu.ime,
                        mem.ie_register, mem.io_regs[0x0F], mem.io_regs[0x40], mem.io_regs[0x41],
                        mem.io_regs[0x45], mem.io_regs[0x47], mem.io_regs[0x42], mem.io_regs[0x43],
                        cart.rom_bank);
            }
            if ((i % 5) == 0) {
                fprintf(stderr, "[REG] f%05ld LCDC=%02X SCY=%3d SCX=%3d WY=%3d WX=%3d BGP=%02X\n",
                    i, mem.io_regs[0x40], mem.io_regs[0x42], mem.io_regs[0x43],
                    mem.io_regs[0x4A], mem.io_regs[0x4B], mem.io_regs[0x47]);
                char path[512];
                snprintf(path, sizeof(path), "%s/f%05ld.bmp", dump_dir, i);
                FILE* f = fopen(path, "wb");
                if (f) {
                    const u32* px = ppu.fb_done;
                    int W = SCREEN_W, H = SCREEN_H, row = (W*3+3)&~3, sz = 54 + row*H;
                    u8 hdr[54] = {0}; hdr[0]='B'; hdr[1]='M';
                    hdr[2]=(u8)sz; hdr[3]=(u8)(sz>>8); hdr[4]=(u8)(sz>>16); hdr[5]=(u8)(sz>>24);
                    hdr[10]=54; hdr[14]=40; hdr[18]=(u8)W; hdr[19]=(u8)(W>>8);
                    hdr[22]=(u8)H; hdr[23]=(u8)(H>>8); hdr[26]=1; hdr[28]=24;
                    fwrite(hdr,1,54,f);
                    u8* line = (u8*)malloc(row);
                    for (int y = H-1; y >= 0; y--) {
                        memset(line,0,row);
                        for (int x = 0; x < W; x++) {
                            u32 c = px[y*W+x];
                            line[x*3+0]=(c>>16)&0xFF; line[x*3+1]=(c>>8)&0xFF; line[x*3+2]=c&0xFF;
                        }
                        fwrite(line,1,row,f);
                    }
                    free(line); fclose(f);
                }
            }
        }
        return 0;
    }

    if (!init_sdl()) {
        return 1;
    }

    cpu.reset(cart.is_cgb());
    mem.reset();
    ppu.reset();
    timer.reset();
    apu.reset();
    mem.set_cartridge(&cart);
    mem.set_input(&input);
    mem.set_apu(&apu);
    printer.reset();
    mem.set_printer(&printer); mem.set_ppu(&ppu);

    fprintf(stdout, "Loaded: %s | Type: %02X | ROM Banks: %d | RAM Banks: %d\n",
        argv[1], cart.mbc_type, cart.num_rom_banks, cart.num_ram_banks);

    frame_start_ms = SDL_GetTicks();

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(emscripten_loop, 0, 1);
#else
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(e);
        run_frame();
    }

    save_ram_to_file();

    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
#endif

    return 0;
}
