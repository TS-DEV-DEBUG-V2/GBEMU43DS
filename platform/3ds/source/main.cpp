#include <3ds.h>
#include <citro2d.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cmath>
#include <ctime>

#include "gb_cpu.h"
#include "gb_memory.h"
#include "gb_ppu.h"
#include "gb_timer.h"
#include "gb_input.h"
#include "gb_apu.h"
#include "gb_cartridge.h"
#include "gb_savestate.h"
#include "gb_gamegenie.h"
#include "gb_gameshark.h"
#include "gb_printer.h"
#include "bios.h"
#include "bios4gbc.h"
#include "gb_log.h"

#define COL_BG_TOP     C2D_Color32(0x35, 0x35, 0x3a, 0xFF)
#define COL_BG_BOT     C2D_Color32(0x57, 0x57, 0x5e, 0xFF)
#define COL_PANEL      C2D_Color32(0x2a, 0x2a, 0x2f, 0xE8)
#define COL_PANEL_EDGE C2D_Color32(0xe0, 0x5c, 0x94, 0xFF)
#define COL_ACCENT     C2D_Color32(0xc8, 0x3c, 0x74, 0xFF)
#define COL_GOLD       C2D_Color32(0x9b, 0xbc, 0x0f, 0xFF)
#define COL_WHITE      C2D_Color32(0xf0, 0xf0, 0xf2, 0xFF)
#define COL_DIM        C2D_Color32(0x9a, 0x9a, 0xa2, 0xFF)
#define COL_SHADOW     C2D_Color32(0x00, 0x00, 0x00, 0x50)
#define COL_SEL_BAR    C2D_Color32(0xa8, 0x30, 0x60, 0xFF)
#define COL_DIRTXT     C2D_Color32(0xb8, 0xd4, 0x3a, 0xFF)
#define COL_OVERLAY    C2D_Color32(0x00, 0x00, 0x00, 0xB0)
#define COL_PURPLE_BOX C2D_Color32(0x7a, 0x24, 0x50, 0xFF)
#define COL_PURPLE_EDG C2D_Color32(0xe0, 0x5c, 0x94, 0xFF)
#define COL_SLOT_EMPTY C2D_Color32(0x2e, 0x2e, 0x33, 0xFF)
#define COL_SLOT_FILL  C2D_Color32(0x45, 0x45, 0x4c, 0xFF)

static C2D_TextBuf g_txtbuf;
static C2D_TextBuf g_static_txtbuf;
static C2D_Text    g_ct_playing_hint;
static float       g_ct_playing_hint_w, g_ct_playing_hint_h;

typedef struct {
    C2D_TextBuf buf;
    C2D_Text    text;
    float       w, h;
    float       last_scale;
    char        cached[192];
} CachedText;

static void ct_ensure(CachedText *ct, const char *str, float scale) {
    if (ct->buf && ct->last_scale == scale && strcmp(ct->cached, str) == 0) return;
    if (!ct->buf) ct->buf = C2D_TextBufNew(256);
    else C2D_TextBufClear(ct->buf);
    C2D_TextParse(&ct->text, ct->buf, str);
    C2D_TextOptimize(&ct->text);
    C2D_TextGetDimensions(&ct->text, scale, scale, &ct->w, &ct->h);
    strncpy(ct->cached, str, sizeof(ct->cached) - 1);
    ct->cached[sizeof(ct->cached) - 1] = '\0';
    ct->last_scale = scale;
}

static void ct_draw(const CachedText *ct, float x, float y, float z, float scale, u32 color) {
    C2D_DrawText(&ct->text, C2D_WithColor, x, y, z, scale, scale, color);
}

static CPU       cpu;
static Memory    mem;
static PPU       ppu;
static Timer     timer;
static Input     input;
static APU       apu;
static Cartridge cart;
static GBPrinter printer;

enum { KBM_TEXT, KBM_HEX_GG, KBM_HEX_GS, KBM_NUM };
static bool ui_keyboard(int mode, const char* title, const char* initial, char* out, int outsz);
static void ui_popup(const char* title, const char* msg);
static bool ui_confirm(const char* title, const char* msg);

typedef enum { APP_MENU, APP_BROWSE, APP_PLAYING, APP_PAUSED, APP_SETTINGS, APP_CONTROLS,
               APP_SAVESTATES, APP_STATE_SAVE, APP_STATE_LOAD, APP_GAMEGENIE } AppState;
static AppState g_state = APP_MENU;
static bool     g_want_quit_app = false;

static float g_anim_t = 0.0f;

static int g_menu_sel = 0;
static const char *g_menu_items[] = { "Play Game", "Exit" };
#define MENU_ITEM_COUNT 2

static int g_pause_sel = 0;
static const char *g_pause_items[] = { "Save States", "Settings", "Take Screenshot", "Quit to Menu", "Quit App" };
#define PAUSE_ITEM_COUNT 5
#define PAUSE_SAVESTATES 0
#define PAUSE_SETTINGS   1
#define PAUSE_SCREENSHOT 2
#define PAUSE_QUIT_MENU  3
#define PAUSE_QUIT_APP   4

static int  g_settings_sel = 0;
static bool g_frameskip = false;
static bool g_show_fps  = true;
static bool g_screen_stretch = false;
static bool g_apu_enabled = true;
enum { CLOCK_SYSTEM = 0, CLOCK_CUSTOM = 1, CLOCK_FROZEN = 2 };
static int g_gb_clock_mode = CLOCK_SYSTEM;
static int g_gb_clock_custom_h = 12;
static int g_gb_clock_custom_m = 0;
static const char *g_settings_items[] = { "Frameskip", "Show FPS", "Screen", "Enable APU",
                                          "GB Clock Time", "Game Genie", "Game Shark", "Controller Config" };
#define SETTINGS_ITEM_COUNT 8
#define SETTINGS_APU_INDEX      3
#define SETTINGS_CLOCK_INDEX    4
#define SETTINGS_GG_INDEX       5
#define SETTINGS_GS_INDEX       6
#define SETTINGS_CONTROLS_INDEX 7

typedef struct { u32 mask; const char *name; } KeyOpt;
static const KeyOpt KEY_OPTS[] = {
    { KEY_A,      "A"      },
    { KEY_B,      "B"      },
    { KEY_X,      "X"      },
    { KEY_Y,      "Y"      },
    { KEY_DUP,    "Pad Up"    },
    { KEY_DDOWN,  "Pad Down"  },
    { KEY_DLEFT,  "Pad Left"  },
    { KEY_DRIGHT, "Pad Right" },
    { KEY_START,  "Start"  },
    { KEY_SELECT, "Select" },
};
#define KEY_OPT_COUNT ((int)(sizeof(KEY_OPTS) / sizeof(KEY_OPTS[0])))

static u32 g_keymap[8];

static void keymap_defaults(void) {
    g_keymap[Input::RIGHT]  = KEY_DRIGHT;
    g_keymap[Input::LEFT]   = KEY_DLEFT;
    g_keymap[Input::UP]     = KEY_DUP;
    g_keymap[Input::DOWN]   = KEY_DDOWN;
    g_keymap[Input::A]      = KEY_A;
    g_keymap[Input::B]      = KEY_B;
    g_keymap[Input::SELECT] = KEY_SELECT;
    g_keymap[Input::START]  = KEY_START;
}

static const struct { int gb_idx; const char *label; } GB_ROWS[8] = {
    { Input::UP,     "Up"     },
    { Input::DOWN,   "Down"   },
    { Input::LEFT,   "Left"   },
    { Input::RIGHT,  "Right"  },
    { Input::A,      "A"      },
    { Input::B,      "B"      },
    { Input::START,  "Start"  },
    { Input::SELECT, "Select" },
};

static int  g_controls_sel = 0;
static bool g_controls_listening = false;

static const char *key_mask_name(u32 mask) {
    for (int i = 0; i < KEY_OPT_COUNT; i++)
        if (KEY_OPTS[i].mask == mask) return KEY_OPTS[i].name;
    if (mask == KEY_UP)    return "Up";
    if (mask == KEY_DOWN)  return "Down";
    if (mask == KEY_LEFT)  return "Left";
    if (mask == KEY_RIGHT) return "Right";
    return "?";
}

static int g_frame_parity = 0;

static float g_fps_value       = 0.0f;
static int   g_fps_frame_count = 0;
static u64   g_fps_window_ms   = 0;

static float g_ms_emulate = 0.0f;
static float g_ms_gpu     = 0.0f;
static float g_ms_audio   = 0.0f;
static float g_ms_present = 0.0f;
static float g_ms_work    = 0.0f;
static u64   g_loop_start = 0;

static double g_tick_hz       = 268111856.0;
static u64    g_frame_ticks   = 0;
static u64    g_frame_deadline = 0;

static void pacing_calibrate(void) {
    u64 wall0 = osGetTime();
    u64 t0 = svcGetSystemTick();
    svcSleepThread(80000000LL);
    u64 t1 = svcGetSystemTick();
    u64 wall1 = osGetTime();
    if (wall1 > wall0)
        g_tick_hz = (double)(t1 - t0) * 1000.0 / (double)(wall1 - wall0);
    g_frame_ticks = (u64)(g_tick_hz * (double)CYCLES_PER_FRAME / 4194304.0);
    g_frame_deadline = svcGetSystemTick();
}

static inline float tick_ms(u64 ticks) { return (float)((double)ticks * 1000.0 / g_tick_hz); }

static void frame_pace(void) {
    g_frame_deadline += g_frame_ticks;
    s64 wait = (s64)(g_frame_deadline - svcGetSystemTick());
    if (wait <= 0) {
        g_frame_deadline = svcGetSystemTick();
        return;
    }
    s64 margin = (s64)(g_tick_hz / 333.0);
    if (wait > margin)
        svcSleepThread((s64)((double)(wait - margin) / g_tick_hz * 1.0e9));
    while ((s64)(g_frame_deadline - svcGetSystemTick()) > 0) { }
}

#define MAX_ENTRIES 1024
#define MAX_NAME    256
typedef struct {
    char name[MAX_NAME];
    bool is_dir;
} Entry;

static Entry g_entries[MAX_ENTRIES];
static int   g_entry_count = 0;
static int   g_sel = 0;
static int   g_scroll = 0;
static char  g_cur_path[512] = "sdmc:/";
static float g_sel_bar_y = -1.0f;
#define VISIBLE_ROWS 8
#define ROW_H        22.0f

#define SAVES_DIR     "sdmc:/gbemu-save-files"
#define CONFIG_PATH   SAVES_DIR "/lastdir.txt"
#define SETTINGS_PATH SAVES_DIR "/settings.txt"

static char g_save_path[600];

static void ensure_saves_dir(void) {
    mkdir(SAVES_DIR, 0777);
}

static const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

#define SAVER_JOBS 3
struct SaverJob { std::vector<u8> buf; char path[600]; volatile bool ready; };
static Thread          g_saver_thread = nullptr;
static LightEvent      g_saver_event;
static volatile bool   g_saver_quit = false;
static SaverJob        g_saver_jobs[SAVER_JOBS];

static void saver_worker(void*) {
    for (;;) {
        LightEvent_Wait(&g_saver_event);
        LightEvent_Clear(&g_saver_event);
        if (g_saver_quit) break;
        for (int i = 0; i < SAVER_JOBS; i++) {
            if (!g_saver_jobs[i].ready) continue;
            FILE* f = fopen(g_saver_jobs[i].path, "wb");
            if (f) { fwrite(g_saver_jobs[i].buf.data(), 1, g_saver_jobs[i].buf.size(), f); fclose(f); }
            g_saver_jobs[i].ready = false;
        }
    }
}

static bool saver_busy(void) {
    for (int i = 0; i < SAVER_JOBS; i++) if (g_saver_jobs[i].ready) return true;
    return false;
}

static bool saver_enqueue(const char* path, const u8* data, size_t len) {
    for (int i = 0; i < SAVER_JOBS; i++) {
        if (g_saver_jobs[i].ready) continue;
        g_saver_jobs[i].buf.assign(data, data + len);
        strncpy(g_saver_jobs[i].path, path, sizeof(g_saver_jobs[i].path) - 1);
        g_saver_jobs[i].path[sizeof(g_saver_jobs[i].path) - 1] = '\0';
        g_saver_jobs[i].ready = true;
        LightEvent_Signal(&g_saver_event);
        return true;
    }
    return false;
}

static void saver_drain(void) { while (saver_busy()) svcSleepThread(1000000LL); }

static void saver_init(void) {
    LightEvent_Init(&g_saver_event, RESET_ONESHOT);
    g_saver_quit = false;
    for (int i = 0; i < SAVER_JOBS; i++) g_saver_jobs[i].ready = false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_saver_thread = threadCreate(saver_worker, nullptr, 16 * 1024, prio + 1, -1, false);
}

static void saver_exit(void) {
    if (!g_saver_thread) return;
    saver_drain();
    g_saver_quit = true;
    LightEvent_Signal(&g_saver_event);
    threadJoin(g_saver_thread, U64_MAX);
    threadFree(g_saver_thread);
    g_saver_thread = nullptr;
}

static bool write_save(void) {
    if (g_save_path[0] == '\0') return true;
    if (cart.ram.empty()) return true;
    size_t sz = cart.ram_size();
    if (sz > cart.ram.size()) sz = cart.ram.size();
    if (!saver_enqueue(g_save_path, cart.ram.data(), sz)) return false;
    LOGI(LOG_SYS, "Battery save: writing %u bytes to SD (background)", (unsigned)sz);
    return true;
}

static void write_save_sync(void) {
    if (g_save_path[0] == '\0') return;
    saver_drain();
    cart.save_ram(g_save_path);
}

static void load_save(void) {
    if (g_save_path[0] == '\0') return;
    cart.load_ram(g_save_path);
}

#define SAVESTATES_DIR "sdmc:/gb-save-states"
#define NUM_SLOTS 10

static char g_rom_base[256] = "";

static int  g_ss_choice_sel = 0;
static int  g_ss_slot_sel   = 0;
static bool g_slot_used[NUM_SLOTS];
static char g_slot_name[NUM_SLOTS][40];

static void ss_ensure_dir(void) { mkdir(SAVESTATES_DIR, 0777); }

static void slot_path(int slot, const char* ext, char* out, size_t n) {
    snprintf(out, n, "%s/%s.slot%d.%s", SAVESTATES_DIR, g_rom_base, slot, ext);
}

static void scan_slots(void) {
    for (int i = 0; i < NUM_SLOTS; i++) {
        char sp[600]; slot_path(i, "state", sp, sizeof(sp));
        FILE* f = fopen(sp, "rb");
        g_slot_used[i] = (f != nullptr);
        if (f) fclose(f);

        g_slot_name[i][0] = '\0';
        char np[600]; slot_path(i, "name", np, sizeof(np));
        FILE* nf = fopen(np, "rb");
        if (nf) {
            size_t rd = fread(g_slot_name[i], 1, sizeof(g_slot_name[i]) - 1, nf);
            g_slot_name[i][rd] = '\0';
            for (int k = (int)rd - 1; k >= 0 && (g_slot_name[i][k] == '\n' || g_slot_name[i][k] == '\r'); k--)
                g_slot_name[i][k] = '\0';
            fclose(nf);
        }
        if (g_slot_name[i][0] == '\0')
            snprintf(g_slot_name[i], sizeof(g_slot_name[i]), "Slot %d", i + 1);
    }
}

static void write_preview_bmp(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    const int W = SCREEN_W, H = SCREEN_H;
    const int rowbytes = W * 3;
    const int imgsize = rowbytes * H;
    const int filesize = 54 + imgsize;
    u8 hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=filesize&0xFF; hdr[3]=(filesize>>8)&0xFF; hdr[4]=(filesize>>16)&0xFF; hdr[5]=(filesize>>24)&0xFF;
    hdr[10]=54;
    hdr[14]=40;
    hdr[18]=W&0xFF; hdr[19]=(W>>8)&0xFF;
    hdr[22]=H&0xFF; hdr[23]=(H>>8)&0xFF;
    hdr[26]=1;
    hdr[28]=24;
    hdr[34]=imgsize&0xFF; hdr[35]=(imgsize>>8)&0xFF; hdr[36]=(imgsize>>16)&0xFF; hdr[37]=(imgsize>>24)&0xFF;
    fwrite(hdr, 1, 54, f);
    static u8 row[SCREEN_W * 3];
    for (int y = H - 1; y >= 0; y--) {
        const u32* src = &ppu.fb_done[y * ppu.fb_stride];
        for (int x = 0; x < W; x++) {
            u32 px = src[x];
            u8 r = (px >> 24) & 0xFF, g = (px >> 16) & 0xFF, b = (px >> 8) & 0xFF;
            row[x*3+0] = b; row[x*3+1] = g; row[x*3+2] = r;
        }
        fwrite(row, 1, rowbytes, f);
    }
    fclose(f);
}

static C3D_Tex           g_prev_tex;
static Tex3DS_SubTexture g_prev_subtex;
static C2D_Image         g_prev_image;
static u32*              g_prev_linear = nullptr;
static bool              g_prev_valid = false;
static int               g_prev_slot = -1;

static void preview_init(void) {
    C3D_TexInit(&g_prev_tex, 256, 256, GPU_RGBA8);
    C3D_TexSetFilter(&g_prev_tex, GPU_LINEAR, GPU_LINEAR);
    g_prev_subtex.width = SCREEN_W; g_prev_subtex.height = SCREEN_H;
    g_prev_subtex.left = 0.0f; g_prev_subtex.top = 1.0f;
    g_prev_subtex.right = SCREEN_W / 256.0f; g_prev_subtex.bottom = 1.0f - SCREEN_H / 256.0f;
    g_prev_image.tex = &g_prev_tex; g_prev_image.subtex = &g_prev_subtex;
    g_prev_linear = (u32*)linearAlloc(256 * SCREEN_H * sizeof(u32));
}

static void preview_load(int slot) {
    if (g_prev_slot == slot) return;
    g_prev_slot = slot; g_prev_valid = false;
    if (slot < 0 || !g_slot_used[slot] || !g_prev_linear) return;
    char bp[600]; slot_path(slot, "bmp", bp, sizeof(bp));
    FILE* f = fopen(bp, "rb");
    if (!f) return;
    u8 hdr[54];
    if (fread(hdr, 1, 54, f) != 54) { fclose(f); return; }
    int w = hdr[18] | (hdr[19] << 8);
    int h = hdr[22] | (hdr[23] << 8);
    if (w != SCREEN_W || h != SCREEN_H) { fclose(f); return; }
    static u8 row[SCREEN_W * 3];
    for (int y = SCREEN_H - 1; y >= 0; y--) {
        if (fread(row, 1, SCREEN_W * 3, f) != SCREEN_W * 3) { fclose(f); return; }
        u32* dst = g_prev_linear + y * 256;
        for (int x = 0; x < SCREEN_W; x++) {
            u8 b = row[x*3+0], g = row[x*3+1], r = row[x*3+2];
            dst[x] = ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | 0xFFu;
        }
    }
    fclose(f);
    GSPGPU_FlushDataCache(g_prev_linear, 256 * SCREEN_H * sizeof(u32));
    GX_DisplayTransfer(
        g_prev_linear, GX_BUFFER_DIM(256, SCREEN_H),
        (u32*)g_prev_tex.data, GX_BUFFER_DIM(256, SCREEN_H),
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    gspWaitForPPF();
    g_prev_valid = true;
}

static bool prompt_slot_name(int slot, char* out, size_t n) {
    return ui_keyboard(KBM_TEXT, "Name this save state", g_slot_name[slot], out, (int)n);
}

static void do_save_state(int slot) {
    ss_ensure_dir();
    char name[40];
    if (!prompt_slot_name(slot, name, sizeof(name))) return;

    char sp[600], bp[600], np[600];
    slot_path(slot, "state", sp, sizeof(sp));
    slot_path(slot, "bmp",   bp, sizeof(bp));
    slot_path(slot, "name",  np, sizeof(np));

    if (savestate_write(sp, cpu, mem, ppu, timer, apu, cart)) {
        write_preview_bmp(bp);
        FILE* nf = fopen(np, "wb");
        if (nf) { fwrite(name, 1, strlen(name), nf); fclose(nf); }
        LOGI(LOG_SYS, "saved state to slot %d (%s)", slot + 1, name);
        g_prev_slot = -1;
        scan_slots();
    } else {
        LOGW(LOG_SYS, "FAILED to save state to slot %d", slot + 1);
    }
}

static bool do_load_state(int slot) {
    if (!g_slot_used[slot]) return false;
    char sp[600]; slot_path(slot, "state", sp, sizeof(sp));
    if (savestate_read(sp, cpu, mem, ppu, timer, apu, cart, input)) {
        LOGI(LOG_SYS, "loaded state from slot %d", slot + 1);
        return true;
    }
    LOGW(LOG_SYS, "FAILED to load state from slot %d (wrong game / corrupt)", slot + 1);
    return false;
}

#define CAM_SRC_W 320
#define CAM_SRC_H 240
#define CAM_IDLE_FRAMES 150

#define CAM_MENU_TOP    126.0f
#define CAM_MENU_BOT    190.0f
#define CAM_BTN_Y       154.0f
#define CAM_BTN_H       30.0f
#define CAM_BTN_W       144.0f
#define CAM_BTN_BACK_X  12.0f
#define CAM_BTN_FRONT_X 164.0f

static bool   g_cam_ready = false;
static bool   g_cam_front = false;
static u16*   g_cam_buf   = nullptr;
static Handle g_cam_recv  = 0;
static Handle g_cam_err   = 0;
static u32    g_cam_unit  = 0;
static int    g_cam_idle  = CAM_IDLE_FRAMES + 1;
static u32    g_cam_frames = 0;
static int    g_cam_status_tick = 0;
static u32    g_cam_feed_avg = 0, g_cam_feed_min = 0, g_cam_feed_max = 0;

static bool cam_menu_visible(void) {
    return cart.mbc_type == MBC_CAMERA && g_cam_idle <= CAM_IDLE_FRAMES;
}

static void cam_shutdown(void) {
    if (!g_cam_ready) return;
    CAMU_StopCapture(PORT_CAM1);
    CAMU_Activate(SELECT_NONE);
    if (g_cam_recv) { svcCloseHandle(g_cam_recv); g_cam_recv = 0; }
    if (g_cam_err)  { svcCloseHandle(g_cam_err);  g_cam_err = 0; }
    camExit();
    if (g_cam_buf) { free(g_cam_buf); g_cam_buf = nullptr; }
    g_cam_ready = false;
    g_cam_frames = 0;
    LOGI(LOG_CART, "[CAM EMU] 3DS camera powered down (game stopped using it)");
}

static bool cam_startup(void) {
    if (g_cam_ready) return true;
    Result rc = camInit();
    if (R_FAILED(rc)) {
        LOGE(LOG_CART, "[CAM EMU] camInit() failed (0x%08lX) - pictures will be blank grey",
            (unsigned long)rc);
        return false;
    }
    g_cam_buf = (u16*)malloc(CAM_SRC_W * CAM_SRC_H * 2);
    if (!g_cam_buf) {
        camExit();
        LOGE(LOG_CART, "[CAM EMU] out of memory - pictures will be blank grey");
        return false;
    }
    memset(g_cam_buf, 0, CAM_SRC_W * CAM_SRC_H * 2);

    u32 sel = g_cam_front ? SELECT_IN1 : SELECT_OUT1;
    CAMU_SetSize(sel, SIZE_QVGA, CONTEXT_A);
    CAMU_SetOutputFormat(sel, OUTPUT_RGB_565, CONTEXT_A);
    CAMU_SetFrameRate(sel, FRAME_RATE_15);
    CAMU_SetNoiseFilter(sel, true);
    CAMU_SetAutoExposure(sel, true);
    CAMU_SetAutoWhiteBalance(sel, true);
    CAMU_SetTrimming(PORT_CAM1, false);
    CAMU_GetBufferErrorInterruptEvent(&g_cam_err, PORT_CAM1);

    Result rb = CAMU_GetMaxBytes(&g_cam_unit, CAM_SRC_W, CAM_SRC_H);
    Result rt = CAMU_SetTransferBytes(PORT_CAM1, g_cam_unit, CAM_SRC_W, CAM_SRC_H);
    Result ra = CAMU_Activate(sel);
    CAMU_ClearBuffer(PORT_CAM1);
    Result rs = CAMU_StartCapture(PORT_CAM1);
    if (R_FAILED(rb) || R_FAILED(rt) || R_FAILED(ra) || R_FAILED(rs)) {
        LOGE(LOG_CART, "[CAM EMU] CAMU setup failed: maxbytes=0x%08lX transfer=0x%08lX "
            "activate=0x%08lX start=0x%08lX",
            (unsigned long)rb, (unsigned long)rt, (unsigned long)ra, (unsigned long)rs);
    }
    CAMU_SetReceiving(&g_cam_recv, g_cam_buf, PORT_CAM1, CAM_SRC_W * CAM_SRC_H * 2, (s16)g_cam_unit);

    g_cam_ready = true;
    g_cam_frames = 0;
    g_cam_status_tick = 0;
    LOGI(LOG_CART, "[CAM EMU] 3DS %s camera streaming: %dx%d @15fps (unit=%lu bytes) -> 128x112 grey",
        g_cam_front ? "front" : "back", CAM_SRC_W, CAM_SRC_H, (unsigned long)g_cam_unit);
    return true;
}

static void cam_downscale(void) {
    u32 sum = 0; u8 lo = 255, hi = 0;
    for (int gy = 0; gy < 112; ++gy) {
        const u16* row = g_cam_buf + (8 + gy * 2) * CAM_SRC_W + 32;
        u8* dst = cart.cam_sensor + gy * 128;
        for (int gx = 0; gx < 128; ++gx) {
            u16 p = row[gx * 2];
            int r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
            int r8 = (r << 3) | (r >> 2), g8 = (g << 2) | (g >> 4), b8 = (b << 3) | (b >> 2);
            u8 luma = (u8)((r8 * 77 + g8 * 150 + b8 * 29) >> 8);
            dst[g_cam_front ? (127 - gx) : gx] = luma;
            sum += luma;
            if (luma < lo) lo = luma;
            if (luma > hi) hi = luma;
        }
    }
    g_cam_feed_avg = sum / (112 * 128);
    g_cam_feed_min = lo;
    g_cam_feed_max = hi;
}

static void cam_update(void) {
    if (cart.cam_capture_requested) {
        cart.cam_capture_requested = false;
        g_cam_idle = 0;
        cam_startup();
    }

    if (g_cam_ready) {
        if (g_cam_err && svcWaitSynchronization(g_cam_err, 0) == 0) {
            LOGW(LOG_CART, "[CAM EMU] buffer error - restarting capture");
            svcCloseHandle(g_cam_err); g_cam_err = 0;
            if (g_cam_recv) { svcCloseHandle(g_cam_recv); g_cam_recv = 0; }
            CAMU_StopCapture(PORT_CAM1);
            CAMU_ClearBuffer(PORT_CAM1);
            CAMU_GetBufferErrorInterruptEvent(&g_cam_err, PORT_CAM1);
            CAMU_StartCapture(PORT_CAM1);
            CAMU_SetReceiving(&g_cam_recv, g_cam_buf, PORT_CAM1,
                CAM_SRC_W * CAM_SRC_H * 2, (s16)g_cam_unit);
        }

        if (g_cam_recv && svcWaitSynchronization(g_cam_recv, 0) == 0) {
            svcCloseHandle(g_cam_recv);
            g_cam_recv = 0;
            GSPGPU_InvalidateDataCache(g_cam_buf, CAM_SRC_W * CAM_SRC_H * 2);
            cam_downscale();
            if (g_cam_frames == 0)
                LOGI(LOG_CART, "[CAM EMU] First real frame off the 3DS sensor - feed is live");
            g_cam_frames++;
            CAMU_SetReceiving(&g_cam_recv, g_cam_buf, PORT_CAM1,
                CAM_SRC_W * CAM_SRC_H * 2, (s16)g_cam_unit);
        }

        if (++g_cam_status_tick >= 60) {
            g_cam_status_tick = 0;
            if (g_cam_frames == 0)
                LOGW(LOG_CART, "[CAM EMU] %s cam: NO FRAMES ARRIVING - picture will be flat",
                    g_cam_front ? "front" : "back");
            else {
                u32 tot = 0;
                for (int i = 0; i < 4; i++) tot += cart.cam_shade_hist[i];
                if (!tot) tot = 1;
                LOGI(LOG_CART, "[CAM EMU] %s cam: %lu frames, feed avg=%lu min=%lu max=%lu | "
                    "%lu pics, %s dither, shades white/lt/dk/black = %lu/%lu/%lu/%lu%%",
                    g_cam_front ? "front" : "back", (unsigned long)g_cam_frames,
                    (unsigned long)g_cam_feed_avg, (unsigned long)g_cam_feed_min,
                    (unsigned long)g_cam_feed_max, (unsigned long)cart.cam_pictures,
                    cart.cam_last_matrix_set ? "ROM" : "built-in",
                    (unsigned long)(cart.cam_shade_hist[0] * 100 / tot),
                    (unsigned long)(cart.cam_shade_hist[1] * 100 / tot),
                    (unsigned long)(cart.cam_shade_hist[2] * 100 / tot),
                    (unsigned long)(cart.cam_shade_hist[3] * 100 / tot));
                LOGI(LOG_CART, "[CAM EMU] level %d-%d, regs A000-A005 = %02X %02X %02X %02X %02X %02X, "
                    "dither cell0 = %02X %02X %02X",
                    cart.cam_level_lo, cart.cam_level_hi,
                    cart.cam_regs[0], cart.cam_regs[1], cart.cam_regs[2], cart.cam_regs[3],
                    cart.cam_regs[4], cart.cam_regs[5],
                    cart.cam_regs[6], cart.cam_regs[7], cart.cam_regs[8]);
            }
        }

        if (g_cam_idle > CAM_IDLE_FRAMES) cam_shutdown();
    }
    if (g_cam_idle <= CAM_IDLE_FRAMES) g_cam_idle++;
}

static void cam_flip(bool front) {
    if (g_cam_front == front) return;
    g_cam_front = front;
    LOGI(LOG_CART, "[CAM EMU] Switching to the %s camera", front ? "front" : "back");
    if (g_cam_ready) { cam_shutdown(); cam_startup(); }
}

#define PRINTS_DIR "sdmc:/gb-prints"
#define PRINT_BMP_SCALE 4

static void save_printer_bmp(const char* path) {
    int W = printer.img_w, H = printer.img_h;
    if (W <= 0 || H <= 0) { LOGW(LOG_SYS, "GB Printer: empty image, nothing to save"); return; }
    FILE* f = fopen(path, "wb");
    if (!f) { LOGW(LOG_SYS, "GB Printer: cannot open %s", path); return; }
    const int OW = W * PRINT_BMP_SCALE, OH = H * PRINT_BMP_SCALE;
    int rowbytes = (OW * 3 + 3) & ~3;
    int imgsize = rowbytes * OH, filesize = 54 + imgsize;
    u8 hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=filesize&0xFF; hdr[3]=(filesize>>8)&0xFF; hdr[4]=(filesize>>16)&0xFF; hdr[5]=(filesize>>24)&0xFF;
    hdr[10]=54; hdr[14]=40;
    hdr[18]=OW&0xFF; hdr[19]=(OW>>8)&0xFF;
    hdr[22]=OH&0xFF; hdr[23]=(OH>>8)&0xFF;
    hdr[26]=1; hdr[28]=24;
    hdr[34]=imgsize&0xFF; hdr[35]=(imgsize>>8)&0xFF; hdr[36]=(imgsize>>16)&0xFF; hdr[37]=(imgsize>>24)&0xFF;
    fwrite(hdr, 1, 54, f);
    static const u8 shade_gray[4] = { 0xFF, 0xAA, 0x55, 0x00 };
    std::vector<u8> row(rowbytes, 0);
    for (int oy = OH - 1; oy >= 0; oy--) {
        int sy = oy / PRINT_BMP_SCALE;
        for (int ox = 0; ox < OW; ox++) {
            u8 g = shade_gray[printer.image[(size_t)sy * W + (ox / PRINT_BMP_SCALE)] & 3];
            row[ox*3+0] = g; row[ox*3+1] = g; row[ox*3+2] = g;
        }
        fwrite(row.data(), 1, rowbytes, f);
    }
    fclose(f);
    LOGI(LOG_SYS, "GB Printer: saved %dx%d (%dx upscaled from %dx%d) to %s",
         OW, OH, PRINT_BMP_SCALE, W, H, path);
}
static char             g_cc_path[600];
static std::vector<u8>  g_banks_seen;
static bool             g_cc_dirty = false;
static int              g_cc_tick = 0;
static const int        CC_FLUSH_INTERVAL = 1200;

static u32 rom_file_hash(void) {
    u32 h = 2166136261u;
    for (size_t i = 0; i < cart.rom.size(); i++) h = (h ^ cart.rom[i]) * 16777619u;
    return h;
}
static int cc_count(void) {
    int n = 0; for (u8 b : g_banks_seen) n += (b != 0); return n;
}

static void codecache_load(void) {
    int nb = cart.num_rom_banks > 0 ? cart.num_rom_banks : 1;
    g_banks_seen.assign(nb, 0);
    g_cc_dirty = false;
    g_cc_tick = 0;
    snprintf(g_cc_path, sizeof(g_cc_path), "%s/%08X.txt", SAVES_DIR, rom_file_hash());
    FILE* f = fopen(g_cc_path, "rb");
    int loaded = 0;
    if (f) {
        int filebanks = 0;
        if (fscanf(f, "CPUCACHE1 %d\n", &filebanks) == 1) {
            for (int i = 0; i < filebanks && i < nb; i++) {
                int c = fgetc(f);
                if (c == '1') { g_banks_seen[i] = 1; loaded++; }
            }
        }
        fclose(f);
        LOGI(LOG_SYS, "CPU code cache: loaded %d/%d banks from %08X.txt", loaded, nb, rom_file_hash());
    } else {
        LOGI(LOG_SYS, "CPU code cache: none on disk for %08X - building fresh (miss = interpret & save)", rom_file_hash());
    }
}

static void codecache_save(void) {
    if (!g_cc_dirty || g_cc_path[0] == '\0') return;
    std::vector<u8> buf;
    char hdr[32];
    int n = snprintf(hdr, sizeof(hdr), "CPUCACHE1 %d\n", (int)g_banks_seen.size());
    buf.insert(buf.end(), hdr, hdr + n);
    for (u8 b : g_banks_seen) buf.push_back(b ? '1' : '0');
    buf.push_back('\n');
    if (saver_enqueue(g_cc_path, buf.data(), buf.size())) {
        g_cc_dirty = false;
        LOGI(LOG_SYS, "CPU code cache: saving %d banks to %08X.txt (background)", cc_count(), rom_file_hash());
    }
}

#define GG_MAX_CODES     24

struct GGEntry {
    char text[16];
    char name[24];
    bool enabled;
    bool is_gs;
    u8   gs_type;
    u16  gs_addr;
    u8   gs_val;
};
static GGEntry g_gg[GG_MAX_CODES];
static int     g_gg_count = 0;
static int     g_gg_sel = 0;
static char    g_gg_path[600];
static char    g_gs_path[600];
static bool    g_cheat_screen_gs = false;

static int cheat_visible_count(void) {
    int n = 0; for (int i = 0; i < g_gg_count; i++) if (g_gg[i].is_gs == g_cheat_screen_gs) n++; return n;
}
static int cheat_nth_visible(int n) {
    int seen = 0;
    for (int i = 0; i < g_gg_count; i++)
        if (g_gg[i].is_gs == g_cheat_screen_gs) { if (seen == n) return i; seen++; }
    return -1;
}

static void gg_rebuild(void) {
    cart.gg_clear();
    for (int i = 0; i < g_gg_count; i++) {
        if (!g_gg[i].enabled || g_gg[i].is_gs) continue;
        u16 a; u8 v, c; bool hc;
        if (gg_decode(g_gg[i].text, &a, &v, &c, &hc)) cart.gg_add(a, v, c, hc);
    }
    mem.refresh_fast_pages();
}

static void gs_apply_frame(void) {
    for (int i = 0; i < g_gg_count; i++) {
        if (!g_gg[i].enabled || !g_gg[i].is_gs) continue;
        u8 type = g_gg[i].gs_type, val = g_gg[i].gs_val;
        u16 addr = g_gg[i].gs_addr;

        if ((type & 0xF0) == 0x80 && addr >= 0xA000 && addr < 0xC000) {
            int bank = type & 0x0F;
            size_t off = (size_t)bank * 0x2000 + (addr - 0xA000);
            if (off < cart.ram.size()) { cart.ram[off] = val; cart.dirty = true; }
        } else if ((type & 0xF0) == 0x90 && addr >= 0xD000 && addr < 0xE000) {
            int bank = type & 0x0F;
            size_t off = (size_t)bank * 0x1000 + (addr - 0xD000);
            if (off < mem.wram.size()) mem.wram[off] = val;
        } else {
            mem.write(addr, val);
        }
    }
}

static bool cheat_decode(const char* code, GGEntry& e) {
    u16 a; u8 v, c; bool hc;
    if (gg_decode(code, &a, &v, &c, &hc)) { e.is_gs = false; return true; }
    if (gs_decode(code, &e.gs_type, &e.gs_addr, &e.gs_val)) { e.is_gs = true; return true; }
    return false;
}

static void write_cheat_file(const char* path, bool gs) {
    if (path[0] == '\0') return;
    int n = 0; for (int i = 0; i < g_gg_count; i++) if (g_gg[i].is_gs == gs) n++;
    if (n == 0) { remove(path); return; }
    FILE* f = fopen(path, "wb");
    if (!f) return;
    for (int i = 0; i < g_gg_count; i++)
        if (g_gg[i].is_gs == gs)
            fprintf(f, "%s %d %s\n", g_gg[i].text, g_gg[i].enabled ? 1 : 0, g_gg[i].name);
    fclose(f);
}
static void gg_save_file(void) {
    write_cheat_file(g_gg_path, false);
    write_cheat_file(g_gs_path, true);
}

static void load_cheat_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof(line), f) && g_gg_count < GG_MAX_CODES) {
        char code[16] = {0}, name[24] = {0}; int en = 1;
        int got = sscanf(line, "%15s %d %23[^\n]", code, &en, name);
        if (got < 1) continue;
        GGEntry& e = g_gg[g_gg_count];
        if (!cheat_decode(code, e)) continue;
        strncpy(e.text, code, sizeof(e.text) - 1); e.text[sizeof(e.text)-1] = '\0';
        if (got >= 3 && name[0]) { strncpy(e.name, name, sizeof(e.name)-1); e.name[sizeof(e.name)-1]='\0'; }
        else snprintf(e.name, sizeof(e.name), "Code %d", g_gg_count + 1);
        e.enabled = en != 0;
        g_gg_count++;
    }
    fclose(f);
}

static void gg_load_file(void) {
    g_gg_count = 0; g_gg_sel = 0;
    snprintf(g_gg_path, sizeof(g_gg_path), "%s/%s.gamegeniecodes.txt", SAVES_DIR, g_rom_base);
    snprintf(g_gs_path, sizeof(g_gs_path), "%s/%s.gamesharkcodes.txt", SAVES_DIR, g_rom_base);
    load_cheat_file(g_gg_path);
    load_cheat_file(g_gs_path);
    gg_rebuild();
    if (g_gg_count) LOGI(LOG_SYS, "Cheats: loaded %d code(s) for %s", g_gg_count, g_rom_base);
}

static void gg_add_code_prompt(void) {
    if (g_gg_count >= GG_MAX_CODES) return;

    char buf[16] = {0};
    int  mode  = g_cheat_screen_gs ? KBM_HEX_GS : KBM_HEX_GG;
    const char* title = g_cheat_screen_gs ? "Enter Game Shark Code" : "Enter Game Genie Code";
    if (!ui_keyboard(mode, title, "", buf, sizeof(buf))) return;

    GGEntry& e = g_gg[g_gg_count];
    bool valid;
    if (g_cheat_screen_gs) {
        valid = gs_decode(buf, &e.gs_type, &e.gs_addr, &e.gs_val);
        if (valid) e.is_gs = true;
    } else {
        u16 a; u8 v, c; bool hc;
        valid = gg_decode(buf, &a, &v, &c, &hc);
        if (valid) e.is_gs = false;
    }
    if (!valid) {
        ui_popup("Not a valid code", g_cheat_screen_gs ? "Game Shark needs 8 hex digits"
                                                    : "Game Genie needs 6 or 9 hex digits");
        return;
    }

    strncpy(e.text, buf, sizeof(e.text) - 1); e.text[sizeof(e.text)-1] = '\0';
    snprintf(e.name, sizeof(e.name), "Code %d", g_gg_count + 1);
    char nbuf[24] = {0};
    if (ui_keyboard(KBM_TEXT, "Name this code", e.name, nbuf, sizeof(nbuf)) && nbuf[0])
        { strncpy(e.name, nbuf, sizeof(e.name)-1); e.name[sizeof(e.name)-1]='\0'; }

    e.enabled = true;
    g_gg_count++;
    g_gg_sel = cheat_visible_count() - 1;
    gg_rebuild();
    gg_save_file();
}

static void codecache_tick(void) {
    u8 b = cart.rom_bank;
    if (b < g_banks_seen.size() && !g_banks_seen[b]) {
        g_banks_seen[b] = 1;
        g_cc_dirty = true;
        LOGD(LOG_SYS, "CPU code cache: new bank %d executed (now %d cached)", b, cc_count());
    }
    if (++g_cc_tick >= CC_FLUSH_INTERVAL) { g_cc_tick = 0; codecache_save(); }
}

static u32 g_gb_prev_held = 0;
static int g_save_tick = 0;
static const int SAVE_INTERVAL = 300;

static void codecache_load(void);
static void codecache_save(void);
static void gg_load_file(void);
static void gg_save_file(void);

static void apply_gb_clock(void) {
    if (!cart.has_rtc) return;
    if (g_gb_clock_mode == CLOCK_FROZEN) { cart.rtc_use_system_time = false; return; }

    int h, m, s, day = 0;
    if (g_gb_clock_mode == CLOCK_CUSTOM) {
        h = g_gb_clock_custom_h; m = g_gb_clock_custom_m; s = 0;
    } else {
        time_t t = time(nullptr);
        struct tm* lt = localtime(&t);
        h = lt ? lt->tm_hour : 0;
        m = lt ? lt->tm_min  : 0;
        s = lt ? lt->tm_sec  : 0;
        day = lt ? (lt->tm_yday & 0x1FF) : 0;
    }
    cart.rtc_use_system_time = true;
    cart.rtc[0] = (u8)s; cart.rtc[1] = (u8)m; cart.rtc[2] = (u8)h;
    cart.rtc[3] = (u8)(day & 0xFF);
    cart.rtc[4] = (u8)((cart.rtc[4] & ~0x41) | ((day >> 8) & 1));
    for (int i = 0; i < 5; i++) cart.rtc_latched[i] = cart.rtc[i];
    cart.rtc_base_secs = (long)time(nullptr);
}

static bool load_and_boot(const char *path) {
    if (g_save_path[0] != '\0') write_save_sync();
    codecache_save();
    gg_save_file();
    cam_shutdown();
    g_cam_idle = CAM_IDLE_FRAMES + 1;
    if (!cart.load(path)) return false;

    cpu.reset(cart.is_cgb());
    mem.reset();
    ppu.output_bswap = true;
    ppu.cgb_vivid = true;
    mem.cgb_pal_dirty = true;
    if (cart.is_cgb()) {
        mem.load_boot_rom(cgb_boot_data, cgb_boot_size, true);
    } else {
        mem.load_boot_rom(dmg_boot_data, dmg_boot_size, false);
    }
    ppu.reset();
    timer.reset();
    apu.reset();
    input = Input();
    g_gb_prev_held = 0;

    mem.set_cartridge(&cart);
    mem.set_input(&input);
    mem.set_apu(&apu);
    printer.reset();
    mem.set_printer(&printer); mem.set_ppu(&ppu);

    snprintf(g_save_path, sizeof(g_save_path), "%s/%s.sav", SAVES_DIR, path_basename(path));
    load_save();
    g_save_tick = 0;

    strncpy(g_rom_base, path_basename(path), sizeof(g_rom_base) - 1);
    g_rom_base[sizeof(g_rom_base) - 1] = '\0';
    apply_gb_clock();

    g_prev_slot = -1;
    scan_slots();
    codecache_load();
    gg_load_file();
    return true;
}

static bool has_gb_ext(const char *name) {
    size_t len = strlen(name);
    if (len > 3 && strcasecmp(name + len - 3, ".gb") == 0) return true;
    if (len > 4 && strcasecmp(name + len - 4, ".gbc") == 0) return true;
    if (len > 4 && strcasecmp(name + len - 4, ".zip") == 0) return true;
    return false;
}

static int entry_cmp(const void *a, const void *b) {
    const Entry *ea = (const Entry *)a, *eb = (const Entry *)b;
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
    return strcasecmp(ea->name, eb->name);
}

static void save_last_dir(const char *path);

static void scan_dir(const char *path) {
    g_entry_count = 0;
    g_sel = 0;
    g_scroll = 0;

    DIR *d = opendir(path);
    if (!d) return;

    save_last_dir(path);

    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_entry_count < MAX_ENTRIES) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        bool is_dir;
        if (de->d_type == DT_DIR) {
            is_dir = true;
        } else if (de->d_type == DT_REG) {
            is_dir = false;
        } else {
            char full[600];
            snprintf(full, sizeof(full), "%s%s%s", path,
                (path[strlen(path) - 1] == '/') ? "" : "/", de->d_name);
            struct stat st;
            is_dir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);
        }

        if (!is_dir && !has_gb_ext(de->d_name)) continue;

        strncpy(g_entries[g_entry_count].name, de->d_name, MAX_NAME - 1);
        g_entries[g_entry_count].name[MAX_NAME - 1] = '\0';
        g_entries[g_entry_count].is_dir = is_dir;
        g_entry_count++;
    }
    closedir(d);

    qsort(g_entries, g_entry_count, sizeof(Entry), entry_cmp);
    g_sel_bar_y = -1.0f;
}

static void save_last_dir(const char *path) {
    ensure_saves_dir();
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fputs(path, f);
    fclose(f);
}

static bool load_last_dir(char *out, size_t outsz) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return false;
    char buf[512];
    bool ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    if (!ok) return false;

    buf[strcspn(buf, "\r\n")] = '\0';
    if (buf[0] == '\0') return false;

    DIR *d = opendir(buf);
    if (!d) return false;
    closedir(d);

    strncpy(out, buf, outsz - 1);
    out[outsz - 1] = '\0';
    return true;
}

static void save_settings(void) {
    ensure_saves_dir();
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "frameskip=%d\nshowfps=%d\nstretch=%d\napu=%d\ngbclockmode=%d\ngbclockcustom=%d:%d\n",
        g_frameskip ? 1 : 0, g_show_fps ? 1 : 0, g_screen_stretch ? 1 : 0, g_apu_enabled ? 1 : 0,
        g_gb_clock_mode, g_gb_clock_custom_h, g_gb_clock_custom_m);
    for (int i = 0; i < 8; i++) fprintf(f, "key%d=%lu\n", i, (unsigned long)g_keymap[i]);
    fclose(f);
}

static void load_settings(void) {
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        int val = 0;
        unsigned long mask = 0;
        int idx = 0;
        if (sscanf(line, "frameskip=%d", &val) == 1) g_frameskip = val != 0;
        else if (sscanf(line, "showfps=%d", &val) == 1) g_show_fps = val != 0;
        else if (sscanf(line, "stretch=%d", &val) == 1) g_screen_stretch = val != 0;
        else if (sscanf(line, "apu=%d", &val) == 1) g_apu_enabled = val != 0;
        else if (sscanf(line, "gbclockmode=%d", &val) == 1) { if (val >= 0 && val <= 2) g_gb_clock_mode = val; }
        else if (sscanf(line, "gbclockcustom=%d:%d", &g_gb_clock_custom_h, &g_gb_clock_custom_m) == 2) { }
        else if (sscanf(line, "key%d=%lu", &idx, &mask) == 2) {
            if (idx >= 0 && idx < 8 && mask != 0) g_keymap[idx] = (u32)mask;
        }
    }
    fclose(f);
}

static void path_join(char *dst, size_t dstsz, const char *base, const char *name) {
    if (base[strlen(base) - 1] == '/') snprintf(dst, dstsz, "%s%s", base, name);
    else                                snprintf(dst, dstsz, "%s/%s", base, name);
}

static bool path_is_root(const char *path) {
    return strcmp(path, "sdmc:/") == 0;
}

static void path_up(char *path) {
    char *slash = strrchr(path, '/');
    if (!slash) return;
    if (slash == path + 5) path[6] = '\0';
    else                   *slash = '\0';
}

#define NAV_REPEAT_DELAY_MS    350
#define NAV_REPEAT_INTERVAL_MS 150

static u32 g_nav_active_key = 0;
static u64 g_nav_next_ms    = 0;

static u32 nav_repeat(u32 kDown, u32 kHeld, u32 key_up, u32 key_down) {
    u64 now = osGetTime();
    u32 result = 0;

    if (kDown & key_up)   { result |= key_up;   g_nav_active_key = key_up;   g_nav_next_ms = now + NAV_REPEAT_DELAY_MS; }
    if (kDown & key_down) { result |= key_down; g_nav_active_key = key_down; g_nav_next_ms = now + NAV_REPEAT_DELAY_MS; }

    u32 held_dirs = kHeld & (key_up | key_down);
    if (g_nav_active_key && held_dirs == g_nav_active_key) {
        if (now >= g_nav_next_ms) {
            result |= g_nav_active_key;
            g_nav_next_ms = now + NAV_REPEAT_INTERVAL_MS;
        }
    } else if (!held_dirs) {
        g_nav_active_key = 0;
    }

    return result;
}

static const char *kGbBtnName[8] = { "Right","Left","Up","Down","A","B","Select","Start" };
static const char *g_last_pressed = nullptr;
static int g_input_hb_frames = 0;

static void apply_gb_input(u32 held) {
    for (int i = 0; i < 8; i++) {
        u32 mask = g_keymap[i];
        if (mask & KEY_DUP)    mask |= KEY_CPAD_UP;
        if (mask & KEY_DDOWN)  mask |= KEY_CPAD_DOWN;
        if (mask & KEY_DLEFT)  mask |= KEY_CPAD_LEFT;
        if (mask & KEY_DRIGHT) mask |= KEY_CPAD_RIGHT;
        bool now = (held & mask) != 0;
        if (now) {
            if (!input.buttons[i]) {
                mem.io_regs[0x0F] |= INT_JOYPAD;
                g_last_pressed = kGbBtnName[i];
            }
            input.key_down((Input::Button)i);
        } else {
            input.key_up((Input::Button)i);
        }
    }
    g_gb_prev_held = held;

    if (++g_input_hb_frames >= 240) {
        g_input_hb_frames = 0;
        if (g_last_pressed)
            LOGD(LOG_SYS, "Checking if controls are still working... YES, key pressed: %s", g_last_pressed);
        else
            LOGD(LOG_SYS, "Checking if controls are still working... (no key pressed recently)");
        g_last_pressed = nullptr;
    }
}

#define AUDIO_SAMPLES_PER_BUF 1024
#define AUDIO_RING_SIZE       8192

static ndspWaveBuf g_wavebuf[2];
static s16        *g_audio_buf[2];

static s16 g_audio_ring[AUDIO_RING_SIZE];
static volatile int g_ring_head = 0;
static volatile int g_ring_tail = 0;
static inline int ring_count(void) { return (g_ring_head - g_ring_tail + AUDIO_RING_SIZE) % AUDIO_RING_SIZE; }

static void audio_init(void) {
    ndspInit();
    ndspSetOutputMode(NDSP_OUTPUT_MONO);

    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)APU::SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);

    float mix[12] = {0};
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(0, mix);

    for (int i = 0; i < 2; i++) {
        g_audio_buf[i] = (s16 *)linearAlloc(AUDIO_SAMPLES_PER_BUF * sizeof(s16));
        memset(g_audio_buf[i], 0, AUDIO_SAMPLES_PER_BUF * sizeof(s16));
        memset(&g_wavebuf[i], 0, sizeof(ndspWaveBuf));
        g_wavebuf[i].data_pcm16 = g_audio_buf[i];
        g_wavebuf[i].nsamples   = AUDIO_SAMPLES_PER_BUF;
        g_wavebuf[i].looping    = false;
        ndspChnWaveBufAdd(0, &g_wavebuf[i]);
    }
}

static void audio_push_samples(void) {
    int n = apu.buffer_pos;
    int head = g_ring_head;
    for (int i = 0; i < n; i++) {
        if (((head + 1) % AUDIO_RING_SIZE) == g_ring_tail) break;
        g_audio_ring[head] = apu.audio_buffer[i];
        head = (head + 1) % AUDIO_RING_SIZE;
    }
    __sync_synchronize();
    g_ring_head = head;
    apu.buffer_pos = 0;
}

static void audio_update(void) {
    for (int i = 0; i < 2; i++) {
        if (g_wavebuf[i].status != NDSP_WBUF_DONE) continue;
        if (ring_count() < AUDIO_SAMPLES_PER_BUF) continue;
        int tail = g_ring_tail;
        for (int j = 0; j < AUDIO_SAMPLES_PER_BUF; j++)
            g_audio_buf[i][j] = g_audio_ring[(tail + j) % AUDIO_RING_SIZE];
        g_ring_tail = (tail + AUDIO_SAMPLES_PER_BUF) % AUDIO_RING_SIZE;
        DSP_FlushDataCache(g_audio_buf[i], AUDIO_SAMPLES_PER_BUF * sizeof(s16));
        ndspChnWaveBufAdd(0, &g_wavebuf[i]);
    }
}

static void audio_exit(void) {
    ndspChnWaveBufClear(0);
    ndspChnReset(0);
    linearFree(g_audio_buf[0]);
    linearFree(g_audio_buf[1]);
    ndspExit();
}

static C3D_Tex            g_gb_tex;
static Tex3DS_SubTexture  g_gb_subtex;
static C2D_Image          g_gb_image;

static u32 *g_gb_linear_buf[2];
static int  g_gb_buf_idx = 0;

static const float GB_SCALE  = 240.0f / (float)SCREEN_H;
static const float GB_DISP_W = SCREEN_W * GB_SCALE;
static const float GB_BAR_W  = (400.0f - GB_DISP_W) / 2.0f;

static const float GB_STRETCH_SCALE_X = 400.0f / (float)SCREEN_W;
static const float GB_STRETCH_SCALE_Y = 240.0f / (float)SCREEN_H;

static void gb_tex_init(void) {
    C3D_TexInit(&g_gb_tex, 256, 256, GPU_RGBA8);
    C3D_TexSetFilter(&g_gb_tex, GPU_NEAREST, GPU_NEAREST);

    g_gb_subtex.width  = SCREEN_W;
    g_gb_subtex.height = SCREEN_H;
    g_gb_subtex.left   = 0.0f;
    g_gb_subtex.top    = 1.0f;
    g_gb_subtex.right  = SCREEN_W  / 256.0f;
    g_gb_subtex.bottom = 1.0f - SCREEN_H / 256.0f;

    g_gb_image.tex    = &g_gb_tex;
    g_gb_image.subtex = &g_gb_subtex;

    for (int i = 0; i < 2; i++) {
        g_gb_linear_buf[i] = (u32 *)linearAlloc(256 * SCREEN_H * sizeof(u32));
        memset(g_gb_linear_buf[i], 0, 256 * SCREEN_H * sizeof(u32));
    }

    ppu.fb_draw = g_gb_linear_buf[0];
    ppu.fb_done = g_gb_linear_buf[1];
    ppu.fb_stride = 256;
    ppu.fb_external = true;
}

static void gb_tex_upload(void) {
    u32 *buf = ppu.fb_done;
    GSPGPU_FlushDataCache(buf, 256 * SCREEN_H * sizeof(u32));
    GX_DisplayTransfer(
        buf, GX_BUFFER_DIM(256, SCREEN_H),
        (u32 *)g_gb_tex.data, GX_BUFFER_DIM(256, SCREEN_H),
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
}

static void gb_run_frame(void) {
    bool apu_on = g_apu_enabled;
    u8* iflag = (u8*)&mem.io_regs[0x0F];
    int total = 0;
    while (total < CYCLES_PER_FRAME) {
        int budget = ppu.cycles_to_next_event();
        if (mem.io_regs[0x07] & 0x04) {
            int tmr_sys  = timer.cycles_to_next_event(mem);
            int tmr_dots = cpu.double_speed ? (tmr_sys >> 1) : tmr_sys;
            if (tmr_dots < budget) budget = tmr_dots;
        }
        int rem = CYCLES_PER_FRAME - total;
        if (budget > rem) budget = rem;
        if (budget < 1) budget = 1;

        int done_sys = 0;
        int done_dots = cpu.run_batch(mem, budget, done_sys);
        timer.step(done_sys, mem);
        mem.serial_tick(done_sys);
        ppu.step(done_dots, mem, iflag);
        if (apu_on) apu.step(done_dots, mem);
        if (cart.has_camera) cart.camera_tick(done_dots);
        total += done_dots;
    }
}

static void draw_text(const char *str, float x, float y, float z, float scale, u32 color, u32 flags) {
    C2D_Text t;
    C2D_TextParse(&t, g_txtbuf, str);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, flags | C2D_WithColor, x, y, z, scale, scale, color);
}

typedef struct { C2D_Text text; float w, h; } MeasuredText;

static MeasuredText measure_text(const char *str, float scale) {
    MeasuredText mt;
    C2D_TextParse(&mt.text, g_txtbuf, str);
    C2D_TextOptimize(&mt.text);
    C2D_TextGetDimensions(&mt.text, scale, scale, &mt.w, &mt.h);
    return mt;
}

static void draw_measured(const MeasuredText *mt, float x, float y, float z, float scale, u32 color) {
    C2D_DrawText(&mt->text, C2D_WithColor, x, y, z, scale, scale, color);
}

static void draw_vgradient(float w, float h, u32 top, u32 bot) {
    C2D_DrawRectangle(0, 0, 0, w, h, top, top, bot, bot);
}

static u32 pulse_accent(void) {
    float g = 0.72f + 0.28f * sinf(g_anim_t * 6.0f);
    return C2D_Color32((u8)(0xc8 * g), (u8)(0x3c * g), (u8)(0x74 * g), 0xFF);
}

static float smooth_towards(float current, float target, float rate) {
    if (current < 0.0f) return target;
    return current + (target - current) * rate;
}

#define LOG_RING_CAP 400
struct LogLine { u32 color; u8 hue; char text[110]; };
static LogLine g_log_ring[LOG_RING_CAP];
static int g_log_head = 0;
static int g_log_count = 0;
static int g_log_scroll = 0;
static bool g_show_log = false;
static bool g_log_touching = false;
static int g_log_touch_last_y = 0;

static u32 hue_to_rgb(u8 h) {
    int region = h / 43;
    int rem = (h - region * 43) * 6;
    u8 up = (u8)(rem > 255 ? 255 : rem), dn = (u8)(255 - up);
    switch (region) {
        case 0:  return C2D_Color32(255, up, 0, 0xFF);
        case 1:  return C2D_Color32(dn, 255, 0, 0xFF);
        case 2:  return C2D_Color32(0, 255, up, 0xFF);
        case 3:  return C2D_Color32(0, dn, 255, 0xFF);
        case 4:  return C2D_Color32(up, 0, 255, 0xFF);
        default: return C2D_Color32(255, 0, dn, 0xFF);
    }
}

static u8 log_cat_hue(LogCat cat) {
    switch (cat) {
        case LOG_SYS:    return 40;
        case LOG_CPU:    return 85;
        case LOG_MEM:    return 170;
        case LOG_TIMER:  return 128;
        case LOG_PPU:    return 213;
        case LOG_APU:    return 20;
        case LOG_CART:   return 64;
        case LOG_SERIAL: return 106;
        default:         return 0;
    }
}

#define LOG_CLEAR_BYTES (1u * 1024u * 1024u)
static u32 g_log_total_bytes = 0;

#define LOG_WRAP_CHARS 54

static void log3ds_push(u32 color, u8 hue, const char* text) {
    LogLine &ln = g_log_ring[g_log_head];
    ln.color = color;
    ln.hue = hue;
    snprintf(ln.text, sizeof(ln.text), "%s", text);
    g_log_head = (g_log_head + 1) % LOG_RING_CAP;
    if (g_log_count < LOG_RING_CAP) g_log_count++;
}

static void log3ds_sink(LogLevel level, LogCat cat, const char *text) {
    u32 color; u8 hue;
    if (level == LOG_ERROR)      { color = C2D_Color32(0xff, 0x00, 0x00, 0xFF); hue = 0xFF; }
    else if (level == LOG_WARN)  { color = C2D_Color32(0xff, 0xb0, 0x00, 0xFF); hue = 0xFF; }
    else                         { hue = log_cat_hue(cat); color = hue_to_rgb(hue); }

    char full[512];
    int written = snprintf(full, sizeof(full), "[%s] %s", log_cat_name(cat), text);
    if (written < 0) written = 0;
    int len = (int)strlen(full);

    int start = 0;
    while (start < len) {
        bool cont = (start > 0);
        int budget = LOG_WRAP_CHARS - (cont ? 2 : 0);
        int remain = len - start;
        int take = remain <= budget ? remain : budget;

        if (take < remain) {
            int brk = -1;
            for (int i = take; i > 0; i--) {
                if (full[start + i] == ' ') { brk = i; break; }
            }
            if (brk > budget / 3) take = brk;
        }

        char row[LOG_WRAP_CHARS + 4];
        snprintf(row, sizeof(row), "%s%.*s", cont ? "  " : "", take, full + start);
        log3ds_push(color, hue, row);

        start += take;
        while (start < len && full[start] == ' ') start++;
    }

    g_log_total_bytes += (u32)written;
    if (g_log_total_bytes >= LOG_CLEAR_BYTES) {
        g_log_head = 0;
        g_log_count = 0;
        g_log_scroll = 0;
        g_log_total_bytes = 0;
        LogLine &m = g_log_ring[0];
        m.color = C2D_Color32(0xff, 0xee, 0x00, 0xFF); m.hue = 0xFF;
        snprintf(m.text, sizeof(m.text), "[SYS] log cleared (hit 1 MB, freeing RAM)");
        g_log_head = 1;
        g_log_count = 1;
    }
}

#define LOG_VISIBLE_ROWS 8
#define LOG_ROW_H        17.0f
#define LOG_TOP_Y        48.0f
#define LOG_BOTTOM_Y     204.0f
#define LOG_HINT_H       14.0f

static void update_log_touch(void) {
    bool visible = g_show_log || g_state == APP_PLAYING;
    if (!visible) { g_log_touching = false; return; }

    if (hidKeysHeld() & KEY_TOUCH) {
        touchPosition touch;
        hidTouchRead(&touch);
        if (cam_menu_visible() && touch.py >= CAM_MENU_TOP && touch.py <= CAM_MENU_BOT) {
            g_log_touching = false;
            return;
        }
        if (!g_log_touching) {
            g_log_touching = true;
            g_log_touch_last_y = touch.py;
        } else {
            int dy = (int)touch.py - g_log_touch_last_y;
            g_log_touch_last_y = touch.py;
            g_log_scroll += (int)(dy / LOG_ROW_H * 2.0f);
        }
    } else {
        g_log_touching = false;
    }

    int max_scroll = g_log_count > LOG_VISIBLE_ROWS ? g_log_count - LOG_VISIBLE_ROWS : 0;
    if (g_log_scroll > max_scroll) g_log_scroll = max_scroll;
    if (g_log_scroll < 0) g_log_scroll = 0;
}

static void draw_log_view(void) {
    static CachedText ct_rows[LOG_VISIBLE_ROWS];
    static CachedText ct_empty, ct_hint;

    C2D_DrawRectSolid(0, LOG_TOP_Y, 0, 320, LOG_BOTTOM_Y - LOG_TOP_Y, C2D_Color32(0x05, 0x05, 0x0A, 0xFF));

    for (int row = 0; row < LOG_VISIBLE_ROWS && row < g_log_count; row++) {
        int back = g_log_scroll + row;
        int idx = ((g_log_head - 1 - back) % LOG_RING_CAP + LOG_RING_CAP) % LOG_RING_CAP;
        float y = LOG_BOTTOM_Y - (row + 1) * LOG_ROW_H;
        ct_ensure(&ct_rows[row], g_log_ring[idx].text, 0.42f);
        u8 h = g_log_ring[idx].hue;
        u32 col = (h == 0xFF) ? g_log_ring[idx].color
                              : hue_to_rgb((u8)(h + (int)(g_anim_t * 40.0f)));
        ct_draw(&ct_rows[row], 4, y, 0.2f, 0.42f, col);
    }

    if (g_log_count == 0) {
        ct_ensure(&ct_empty, "(no log output yet)", 0.42f);
        ct_draw(&ct_empty, 4, LOG_BOTTOM_Y - LOG_ROW_H, 0.2f, 0.42f, COL_DIM);
    }

    C2D_DrawRectSolid(0, LOG_TOP_Y, 0.3f, 320, LOG_HINT_H, C2D_Color32(0x00, 0x00, 0x00, 0xC0));
    const char* hint = (g_state == APP_PLAYING) ? "Drag to scroll" : "L: Close log   Drag to scroll";
    ct_ensure(&ct_hint, hint, 0.38f);
    ct_draw(&ct_hint, 4, LOG_TOP_Y + 1, 0.35f, 0.38f, COL_DIM);
}

static void update_menu(u32 kDown, u32 kHeld) {
    u32 nav = nav_repeat(kDown, kHeld, KEY_UP, KEY_DOWN);
    if (nav & KEY_UP)   g_menu_sel = (g_menu_sel - 1 + MENU_ITEM_COUNT) % MENU_ITEM_COUNT;
    if (nav & KEY_DOWN) g_menu_sel = (g_menu_sel + 1) % MENU_ITEM_COUNT;

    if (kDown & KEY_A) {
        if (g_menu_sel == 0) {
            scan_dir(g_cur_path);
            g_state = APP_BROWSE;
        } else {
            g_want_quit_app = true;
        }
    }
}

static void draw_menu_top(void) {
    static CachedText ct_title, ct_sub, ct_item[MENU_ITEM_COUNT];

    draw_vgradient(400, 240, COL_BG_TOP, COL_BG_BOT);
    C2D_DrawRectSolid(0, 0, 0, 400, 4, COL_GOLD);

    ct_ensure(&ct_title, "GBEMU", 1.7f);
    ct_draw(&ct_title, (400 - ct_title.w) / 2 + 2, 44 + 2, 0.05f, 1.7f, COL_SHADOW);
    ct_draw(&ct_title, (400 - ct_title.w) / 2,     44,     0.10f, 1.7f, COL_WHITE);

    ct_ensure(&ct_sub, "GameBoy emulator writen by TS", 0.55f);
    ct_draw(&ct_sub, (400 - ct_sub.w) / 2, 92, 0.1f, 0.55f, COL_DIM);

    const float box_w = 220, box_h = 34, gap = 12, start_y = 132;
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        float bx = (400 - box_w) / 2;
        float by = start_y + i * (box_h + gap);
        bool sel = (g_menu_sel == i);

        C2D_DrawRectSolid(bx + 2, by + 3, 0.1f, box_w, box_h, COL_SHADOW);
        C2D_DrawRectSolid(bx, by, 0.15f, box_w, box_h, sel ? pulse_accent() : COL_PANEL);
        if (sel) C2D_DrawRectSolid(bx, by, 0.2f, 4, box_h, COL_GOLD);

        ct_ensure(&ct_item[i], g_menu_items[i], 0.6f);
        ct_draw(&ct_item[i], bx + (box_w - ct_item[i].w) / 2, by + (box_h - ct_item[i].h) / 2, 0.25f,
            0.6f, sel ? COL_WHITE : COL_DIM);
    }
}

static void draw_menu_bottom(void) {
    static CachedText ct_hint;

    draw_vgradient(320, 240, COL_BG_BOT, COL_BG_TOP);

    float bob1 = sinf(g_anim_t * 0.8f) * 6.0f;
    float bob2 = sinf(g_anim_t * 0.8f + 2.1f) * 6.0f;
    C2D_DrawCircle(60, 190 + bob1, 0, 46, COL_PANEL, COL_PANEL, COL_PANEL, COL_PANEL);
    C2D_DrawCircle(270, 40 + bob2, 0, 60, COL_PANEL, COL_PANEL, COL_PANEL, COL_PANEL);

    ct_ensure(&ct_hint, "D-Pad: Navigate    A: Select", 0.55f);
    ct_draw(&ct_hint, (320 - ct_hint.w) / 2, 108, 0.2f, 0.55f, COL_DIM);
}

static void update_browse(u32 kDown, u32 kHeld) {
    if (g_entry_count > 0) {
        u32 nav = nav_repeat(kDown, kHeld, KEY_UP, KEY_DOWN);
        if (nav & KEY_UP)   g_sel = (g_sel - 1 + g_entry_count) % g_entry_count;
        if (nav & KEY_DOWN) g_sel = (g_sel + 1) % g_entry_count;

        if (g_sel < g_scroll) g_scroll = g_sel;
        if (g_sel >= g_scroll + VISIBLE_ROWS) g_scroll = g_sel - VISIBLE_ROWS + 1;
    }

    if ((kDown & KEY_A) && g_entry_count > 0) {
        char full[600];
        path_join(full, sizeof(full), g_cur_path, g_entries[g_sel].name);
        if (g_entries[g_sel].is_dir) {
            strncpy(g_cur_path, full, sizeof(g_cur_path) - 1);
            g_cur_path[sizeof(g_cur_path) - 1] = '\0';
            scan_dir(g_cur_path);
        } else if (load_and_boot(full)) {
            g_state = APP_PLAYING;
        }
    }

    if (kDown & KEY_B) {
        if (path_is_root(g_cur_path)) {
            g_state = APP_MENU;
        } else {
            path_up(g_cur_path);
            scan_dir(g_cur_path);
        }
    }
}

static void draw_browse_top(void) {
    static CachedText ct_path, ct_empty, ct_row[VISIBLE_ROWS];

    draw_vgradient(400, 240, COL_BG_TOP, COL_BG_BOT);
    C2D_DrawRectSolid(0, 0, 0, 400, 4, COL_GOLD);

    ct_ensure(&ct_path, g_cur_path, 0.5f);
    ct_draw(&ct_path, 16, 13, 0.2f, 0.5f, COL_DIM);
    C2D_DrawRectSolid(12, 33, 0.1f, 376, 1, COL_PANEL_EDGE);

    if (g_entry_count == 0) {
        ct_ensure(&ct_empty, "No .gb/.gbc files or folders here", 0.55f);
        ct_draw(&ct_empty, 20, 100, 0.2f, 0.55f, COL_DIM);
        return;
    }

    const float list_y = 40;
    int last = g_scroll + VISIBLE_ROWS;
    if (last > g_entry_count) last = g_entry_count;

    float target_bar_y = list_y + (g_sel - g_scroll) * ROW_H;
    g_sel_bar_y = smooth_towards(g_sel_bar_y, target_bar_y, 0.35f);
    C2D_DrawRectSolid(12, g_sel_bar_y, 0.1f, 372, ROW_H, COL_SEL_BAR);

    for (int i = g_scroll; i < last; i++) {
        float ry = list_y + (i - g_scroll) * ROW_H;
        bool sel = (i == g_sel);

        char label[300];
        if (g_entries[i].is_dir) snprintf(label, sizeof(label), "%s/", g_entries[i].name);
        else                     snprintf(label, sizeof(label), "%s",  g_entries[i].name);

        u32 col = sel ? COL_WHITE : (g_entries[i].is_dir ? COL_DIRTXT : COL_DIM);
        CachedText *slot = &ct_row[i - g_scroll];
        ct_ensure(slot, label, 0.5f);
        ct_draw(slot, 22, ry + 2, 0.2f, 0.5f, col);
    }

    if (g_entry_count > VISIBLE_ROWS) {
        float track_h = ROW_H * VISIBLE_ROWS;
        float thumb_h = track_h * VISIBLE_ROWS / g_entry_count;
        float thumb_y = list_y + (track_h - thumb_h) * ((float)g_scroll / (g_entry_count - VISIBLE_ROWS));
        C2D_DrawRectSolid(390, list_y, 0.1f, 4, track_h, COL_PANEL);
        C2D_DrawRectSolid(390, thumb_y, 0.15f, 4, thumb_h, COL_ACCENT);
    }
}

static void draw_browse_bottom(void) {
    static CachedText ct_name, ct_kind, ct_hint;

    draw_vgradient(320, 240, COL_BG_BOT, COL_BG_TOP);

    if (g_entry_count > 0) {
        bool is_dir = g_entries[g_sel].is_dir;

        C2D_DrawRectSolid(38, 88, 0, 244, 64, COL_PANEL);
        C2D_DrawRectSolid(38, 88, 0.05f, 244, 3, is_dir ? COL_DIRTXT : COL_GOLD);

        float scale = 0.55f;
        ct_ensure(&ct_name, g_entries[g_sel].name, scale);
        if (ct_name.w > 220.0f) {
            scale = (220.0f / ct_name.w) * 0.55f;
            ct_ensure(&ct_name, g_entries[g_sel].name, scale);
        }
        ct_draw(&ct_name, (320 - ct_name.w) / 2, 100, 0.2f, scale, COL_WHITE);

        ct_ensure(&ct_kind, is_dir ? "Folder" : "GB ROM", 0.45f);
        ct_draw(&ct_kind, (320 - ct_kind.w) / 2, 128, 0.2f, 0.45f, is_dir ? COL_DIRTXT : COL_GOLD);
    }

    ct_ensure(&ct_hint, "A: Open      B: Back", 0.5f);
    ct_draw(&ct_hint, (320 - ct_hint.w) / 2, 205, 0.2f, 0.5f, COL_DIM);
}

static void update_pause(u32 kDown, u32 kHeld) {
    u32 nav = nav_repeat(kDown, kHeld, KEY_UP, KEY_DOWN);
    if (nav & KEY_UP)   g_pause_sel = (g_pause_sel - 1 + PAUSE_ITEM_COUNT) % PAUSE_ITEM_COUNT;
    if (nav & KEY_DOWN) g_pause_sel = (g_pause_sel + 1) % PAUSE_ITEM_COUNT;

    if (kDown & (KEY_R | KEY_B)) {
        ndspChnSetPaused(0, false);
        g_state = APP_PLAYING;
        return;
    }

    if (kDown & KEY_A) {
        if (g_pause_sel == PAUSE_SAVESTATES) {
            g_ss_choice_sel = 0;
            scan_slots();
            g_state = APP_SAVESTATES;
            return;
        } else if (g_pause_sel == PAUSE_SETTINGS) {
            g_settings_sel = 0;
            g_state = APP_SETTINGS;
            return;
        } else if (g_pause_sel == PAUSE_SCREENSHOT) {
            mkdir("sdmc:/gb-screenshots", 0777);
            char name[64] = "";
            if (ui_keyboard(KBM_TEXT, "Screenshot filename", "screenshot", name, sizeof(name)) && name[0]) {
                char path[512];
                snprintf(path, sizeof(path), "sdmc:/gb-screenshots/%s.bmp", name);
                write_preview_bmp(path);
                LOGI(LOG_SYS, "Screenshot saved: %s", path);
                ui_popup("Screenshot saved!", path);
            }
            return;
        }
        write_save_sync();
        codecache_save();
        if (g_pause_sel == PAUSE_QUIT_MENU) {
            ndspChnSetPaused(0, false);
            g_state = APP_MENU;
            g_log_head = 0;
            g_log_count = 0;
            g_log_scroll = 0;
            g_log_total_bytes = 0;
        } else if (g_pause_sel == PAUSE_QUIT_APP) {
            g_want_quit_app = true;
        }
    }
}

static void update_savestates(u32 kDown, u32 kHeld) {
    (void)kHeld;
    if (kDown & (KEY_LEFT | KEY_RIGHT)) g_ss_choice_sel ^= 1;

    if (kDown & KEY_A) {
        g_ss_slot_sel = 0;
        g_prev_slot = -1;
        g_state = (g_ss_choice_sel == 0) ? APP_STATE_SAVE : APP_STATE_LOAD;
        return;
    }
    if (kDown & (KEY_B | KEY_R)) g_state = APP_PAUSED;
}

static void update_state_slots(u32 kDown, u32 kHeld, bool saving) {
    u32 nav = nav_repeat(kDown, kHeld, KEY_UP, KEY_DOWN);
    int cols = 2, rows = (NUM_SLOTS + 1) / 2;
    if (nav & KEY_UP)   g_ss_slot_sel = (g_ss_slot_sel - cols + NUM_SLOTS) % NUM_SLOTS;
    if (nav & KEY_DOWN) g_ss_slot_sel = (g_ss_slot_sel + cols) % NUM_SLOTS;
    if (kDown & KEY_LEFT)  g_ss_slot_sel = (g_ss_slot_sel - 1 + NUM_SLOTS) % NUM_SLOTS;
    if (kDown & KEY_RIGHT) g_ss_slot_sel = (g_ss_slot_sel + 1) % NUM_SLOTS;
    (void)rows;

    if (kDown & KEY_A) {
        if (saving) {
            do_save_state(g_ss_slot_sel);
        } else {
            if (do_load_state(g_ss_slot_sel)) {
                ndspChnSetPaused(0, false);
                g_state = APP_PLAYING;
                return;
            }
        }
    }

    if ((kDown & KEY_Y) && g_slot_used[g_ss_slot_sel]) {
        if (ui_confirm("Delete save state?",
                       "Are you sure you want to delete this save state ?")) {
            char p[600];
            slot_path(g_ss_slot_sel, "state", p, sizeof(p)); remove(p);
            slot_path(g_ss_slot_sel, "bmp",   p, sizeof(p)); remove(p);
            slot_path(g_ss_slot_sel, "name",  p, sizeof(p)); remove(p);
            scan_slots();
            LOGI(LOG_SYS, "Save state slot %d deleted", g_ss_slot_sel + 1);
        }
    }

    if (kDown & (KEY_B | KEY_R)) g_state = APP_SAVESTATES;
}

static void draw_playing_top(void) {
    if (g_screen_stretch) {
        C2D_DrawImageAt(g_gb_image, 0.0f, 0.0f, 0.5f, NULL, GB_STRETCH_SCALE_X, GB_STRETCH_SCALE_Y);
        return;
    }
    u32 black = C2D_Color32(0, 0, 0, 0xFF);
    C2D_DrawRectSolid(0, 0, 0, GB_BAR_W, 240, black);
    C2D_DrawRectSolid(400 - GB_BAR_W, 0, 0, GB_BAR_W, 240, black);
    C2D_DrawImageAt(g_gb_image, GB_BAR_W, 0.0f, 0.5f, NULL, GB_SCALE, GB_SCALE);
}

static void draw_playing_bottom(void) {
    draw_vgradient(320, 240, COL_BG_TOP, COL_BG_BOT);

    C2D_DrawText(&g_ct_playing_hint, C2D_WithColor, (320 - g_ct_playing_hint_w) / 2, 208, 0.2f,
        0.6f, 0.6f, COL_DIM);

    if (g_show_fps) {
        static CachedText ct_fps, ct_ms;
        static char fps_buf[64] = "", ms_buf[80] = "";
        static int upd = 0;
        if ((upd++ % 15) == 0) {
            float potential = g_ms_work > 0.1f ? 1000.0f / g_ms_work : 999.0f;
            snprintf(fps_buf, sizeof(fps_buf), "%.1f FPS   (can do ~%.0f)", g_fps_value, potential);
            snprintf(ms_buf, sizeof(ms_buf), "emu %.1f gpu %.1f aud %.1f gfx %.1f work %.1f",
                g_ms_emulate, g_ms_gpu, g_ms_audio, g_ms_present, g_ms_work);
        }
        ct_ensure(&ct_fps, fps_buf, 0.55f);
        ct_draw(&ct_fps, 10, 8, 0.2f, 0.55f, COL_GOLD);
        ct_ensure(&ct_ms, ms_buf, 0.4f);
        ct_draw(&ct_ms, 10, 30, 0.2f, 0.4f, COL_WHITE);
    }
}

#define COL_CAM_PANEL C2D_Color32(0x14, 0x14, 0x1c, 0xFF)
#define COL_CAM_EDGE  C2D_Color32(0x3a, 0x3a, 0x48, 0xFF)

static void draw_camera_menu(void) {
    static CachedText ct_title, ct_back, ct_front;
    const float h = CAM_MENU_BOT - CAM_MENU_TOP;

    C2D_DrawRectSolid(6, CAM_MENU_TOP, 0.60f, 308, h, COL_CAM_EDGE);
    C2D_DrawRectSolid(8, CAM_MENU_TOP + 2, 0.61f, 304, h - 4, COL_CAM_PANEL);
    C2D_DrawRectSolid(6, CAM_MENU_TOP, 0.62f, 308, 3, COL_GOLD);

    ct_ensure(&ct_title, "Game Boy Camera", 0.45f);
    ct_draw(&ct_title, (320 - ct_title.w) / 2, CAM_MENU_TOP + 7, 0.63f, 0.45f, COL_GOLD);

    for (int i = 0; i < 2; i++) {
        bool front = (i == 1);
        bool on = (g_cam_front == front);
        float bx = front ? CAM_BTN_FRONT_X : CAM_BTN_BACK_X;
        C2D_DrawRectSolid(bx, CAM_BTN_Y, 0.64f, CAM_BTN_W, CAM_BTN_H, COL_CAM_EDGE);
        C2D_DrawRectSolid(bx + 1, CAM_BTN_Y + 1, 0.65f, CAM_BTN_W - 2, CAM_BTN_H - 2,
            on ? pulse_accent() : COL_SLOT_FILL);
        if (on) C2D_DrawRectSolid(bx + 1, CAM_BTN_Y + 1, 0.66f, 3, CAM_BTN_H - 2, COL_GOLD);
        CachedText* ct = front ? &ct_front : &ct_back;
        ct_ensure(ct, front ? "Use Front Camera" : "Use Back Camera", 0.45f);
        ct_draw(ct, bx + (CAM_BTN_W - ct->w) / 2, CAM_BTN_Y + (CAM_BTN_H - ct->h) / 2, 0.68f,
            0.45f, on ? COL_WHITE : COL_DIM);
    }
}

static void cam_menu_touch(u32 kDown) {
    if (!cam_menu_visible() || !(kDown & KEY_TOUCH)) return;
    touchPosition t;
    hidTouchRead(&t);
    if (t.py < CAM_BTN_Y || t.py > CAM_BTN_Y + CAM_BTN_H) return;
    if (t.px >= CAM_BTN_BACK_X && t.px <= CAM_BTN_BACK_X + CAM_BTN_W) cam_flip(false);
    else if (t.px >= CAM_BTN_FRONT_X && t.px <= CAM_BTN_FRONT_X + CAM_BTN_W) cam_flip(true);
}

static void draw_pause_overlay(void) {
    static CachedText ct_title, ct_item[PAUSE_ITEM_COUNT];

    C2D_DrawRectSolid(0, 0, 0.3f, 400, 240, COL_OVERLAY);

    const float box_w = 240, box_h = 224;
    const float bx = (400 - box_w) / 2, by = (240 - box_h) / 2;

    C2D_DrawRectSolid(bx + 3, by + 4, 0.35f, box_w, box_h, COL_SHADOW);
    C2D_DrawRectSolid(bx - 2, by - 2, 0.38f, box_w + 4, box_h + 4, COL_PURPLE_EDG);
    C2D_DrawRectSolid(bx, by, 0.40f, box_w, box_h, COL_PURPLE_BOX);
    C2D_DrawRectSolid(bx, by, 0.45f, box_w, 3, COL_GOLD);

    ct_ensure(&ct_title, "Paused", 0.7f);
    C2D_DrawRectSolid(bx + 12, by + 10, 0.44f, box_w - 24, 30, COL_SHADOW);
    C2D_DrawRectSolid(bx + 12, by + 8, 0.46f, box_w - 24, 30, COL_SLOT_FILL);
    ct_draw(&ct_title, bx + (box_w - ct_title.w) / 2, by + 12, 0.5f, 0.7f, COL_WHITE);

    const float iy = by + 44, ih = 28, gap = 5;
    for (int i = 0; i < PAUSE_ITEM_COUNT; i++) {
        bool sel = (g_pause_sel == i);
        float ry = iy + i * (ih + gap);
        C2D_DrawRectSolid(bx + 12, ry + 2, 0.48f, box_w - 24, ih, COL_SHADOW);
        C2D_DrawRectSolid(bx + 12, ry, 0.50f, box_w - 24, ih, sel ? pulse_accent() : COL_SLOT_FILL);
        if (sel) C2D_DrawRectSolid(bx + 12, ry, 0.52f, 3, ih, COL_GOLD);

        ct_ensure(&ct_item[i], g_pause_items[i], 0.55f);
        ct_draw(&ct_item[i], bx + (box_w - ct_item[i].w) / 2, ry + (ih - ct_item[i].h) / 2, 0.55f,
            0.55f, sel ? COL_WHITE : COL_WHITE);
    }
}

static void draw_savestates_bottom(void) {
    draw_vgradient(320, 240, COL_BG_TOP, COL_BG_BOT);
    static CachedText ct_title, ct_save, ct_load, ct_hint;
    ct_ensure(&ct_title, "Save States", 0.7f);
    ct_draw(&ct_title, (320 - ct_title.w) / 2, 20, 0.2f, 0.7f, COL_WHITE);

    const char* labels[2] = { "Save State", "Load State" };
    CachedText* cts[2] = { &ct_save, &ct_load };
    const float bw = 130, bh = 90, gap = 20;
    const float x0 = (320 - (bw * 2 + gap)) / 2, y0 = 80;
    for (int i = 0; i < 2; i++) {
        bool sel = (g_ss_choice_sel == i);
        float bx = x0 + i * (bw + gap);
        C2D_DrawRectSolid(bx + 2, y0 + 3, 0.15f, bw, bh, COL_SHADOW);
        C2D_DrawRectSolid(bx, y0, 0.2f, bw, bh, sel ? pulse_accent() : COL_PANEL);
        if (sel) C2D_DrawRectSolid(bx, y0, 0.25f, bw, 3, COL_GOLD);
        ct_ensure(cts[i], labels[i], 0.6f);
        ct_draw(cts[i], bx + (bw - cts[i]->w) / 2, y0 + (bh - cts[i]->h) / 2, 0.3f, 0.6f,
                sel ? COL_WHITE : COL_DIM);
    }
    ct_ensure(&ct_hint, "Left/Right: choose    A: open    B/R: back", 0.42f);
    ct_draw(&ct_hint, (320 - ct_hint.w) / 2, 210, 0.2f, 0.42f, COL_DIM);
}

static void draw_state_slots_bottom(bool saving) {
    draw_vgradient(320, 240, COL_BG_TOP, COL_BG_BOT);
    static CachedText ct_title, ct_hint, ct_slot[NUM_SLOTS];
    ct_ensure(&ct_title, saving ? "Save to Slot" : "Load from Slot", 0.6f);
    ct_draw(&ct_title, 10, 8, 0.2f, 0.6f, COL_WHITE);

    const float sw = 68, sh = 32, gapx = 6, gapy = 5;
    const float gx = 10, gy = 34;
    for (int i = 0; i < NUM_SLOTS; i++) {
        int col = i % 2, row = i / 2;
        float bx = gx + col * (sw + gapx), by = gy + row * (sh + gapy);
        bool sel = (g_ss_slot_sel == i);
        C2D_DrawRectSolid(bx, by, 0.2f, sw, sh, g_slot_used[i] ? COL_SLOT_FILL : COL_SLOT_EMPTY);
        if (sel) C2D_DrawRectSolid(bx, by, 0.22f, sw, sh, pulse_accent());
        if (sel) C2D_DrawRectSolid(bx, by, 0.25f, 3, sh, COL_GOLD);
        ct_ensure(&ct_slot[i], g_slot_name[i], 0.4f);
        ct_draw(&ct_slot[i], bx + 6, by + (sh - ct_slot[i].h) / 2, 0.3f, 0.4f,
                g_slot_used[i] ? COL_WHITE : COL_DIM);
    }

    preview_load(g_ss_slot_sel);
    const float pscale = 150.0f / (float)SCREEN_W;
    const float pw = SCREEN_W * pscale, ph = SCREEN_H * pscale, px = 320 - pw - 8, py = 44;
    C2D_DrawRectSolid(px - 2, py - 2, 0.2f, pw + 4, ph + 4, COL_PANEL_EDGE);
    if (g_prev_valid && g_slot_used[g_ss_slot_sel]) {
        C2D_DrawImageAt(g_prev_image, px, py, 0.3f, NULL, pscale, pscale);
    } else {
        C2D_DrawRectSolid(px, py, 0.25f, pw, ph, COL_SLOT_EMPTY);
        static CachedText ct_empty;
        ct_ensure(&ct_empty, g_slot_used[g_ss_slot_sel] ? "(no preview)" : "(empty)", 0.45f);
        ct_draw(&ct_empty, px + (pw - ct_empty.w) / 2, py + ph / 2 - 8, 0.3f, 0.45f, COL_DIM);
    }

    ct_ensure(&ct_hint, saving ? "A: save (name it)   B/R: back"
                               : "A: load   B/R: back", 0.42f);
    ct_draw(&ct_hint, 10, 220, 0.2f, 0.42f, COL_DIM);
}

static void update_settings(u32 kDown, u32 kHeld) {
    u32 nav = nav_repeat(kDown, kHeld, KEY_UP, KEY_DOWN);
    if (nav & KEY_UP)   g_settings_sel = (g_settings_sel - 1 + SETTINGS_ITEM_COUNT) % SETTINGS_ITEM_COUNT;
    if (nav & KEY_DOWN) g_settings_sel = (g_settings_sel + 1) % SETTINGS_ITEM_COUNT;

    if (kDown & KEY_A) {
        if (g_settings_sel == 0) { g_frameskip = !g_frameskip; save_settings(); }
        else if (g_settings_sel == 1) { g_show_fps = !g_show_fps; save_settings(); }
        else if (g_settings_sel == 2) { g_screen_stretch = !g_screen_stretch; save_settings(); }
        else if (g_settings_sel == SETTINGS_APU_INDEX) {
            g_apu_enabled = !g_apu_enabled;
            if (!g_apu_enabled) ndspChnWaveBufClear(0);
            save_settings();
        } else if (g_settings_sel == SETTINGS_CLOCK_INDEX) {
            g_gb_clock_mode = (g_gb_clock_mode + 1) % 3;
            if (g_gb_clock_mode == CLOCK_CUSTOM) {
                char buf[8] = {0};
                if (ui_keyboard(KBM_NUM, "Start time HHMM (e.g. 1830)", "", buf, sizeof(buf))) {
                    int v = atoi(buf);
                    int hh = (v / 100) % 24, mm = (v % 100) % 60;
                    g_gb_clock_custom_h = hh; g_gb_clock_custom_m = mm;
                }
            }
            apply_gb_clock();
            static const char* kmode[3] = { "3DS system clock", "custom start time", "frozen" };
            LOGI(LOG_SYS, "GB Clock Time: %s", kmode[g_gb_clock_mode]);
            save_settings();
        } else if (g_settings_sel == SETTINGS_GG_INDEX || g_settings_sel == SETTINGS_GS_INDEX) {
            g_gg_sel = 0;
            g_cheat_screen_gs = (g_settings_sel == SETTINGS_GS_INDEX);
            g_state = APP_GAMEGENIE;
            return;
        } else if (g_settings_sel == SETTINGS_CONTROLS_INDEX) {
            g_controls_sel = 0;
            g_controls_listening = false;
            g_state = APP_CONTROLS;
            return;
        }
    }

    if (kDown & (KEY_R | KEY_B)) {
        g_pause_sel = PAUSE_SETTINGS;
        g_state = APP_PAUSED;
    }
}

static void update_gamegenie(u32 kDown, u32 kHeld) {
    int vis = cheat_visible_count();
    u32 nav = nav_repeat(kDown, kHeld, KEY_UP, KEY_DOWN);
    if (vis > 0) {
        if (nav & KEY_UP)   g_gg_sel = (g_gg_sel - 1 + vis) % vis;
        if (nav & KEY_DOWN) g_gg_sel = (g_gg_sel + 1) % vis;
    } else g_gg_sel = 0;

    if (kDown & KEY_X) { gg_add_code_prompt(); return; }

    if ((kDown & KEY_A) && vis > 0) {
        int idx = cheat_nth_visible(g_gg_sel);
        if (idx >= 0) { g_gg[idx].enabled = !g_gg[idx].enabled; gg_rebuild(); gg_save_file(); }
    }

    if ((kDown & KEY_Y) && vis > 0) {
        int idx = cheat_nth_visible(g_gg_sel);
        if (idx >= 0) {
            for (int i = idx; i < g_gg_count - 1; i++) g_gg[i] = g_gg[i + 1];
            g_gg_count--;
            int nv = cheat_visible_count();
            if (g_gg_sel >= nv) g_gg_sel = nv > 0 ? nv - 1 : 0;
            gg_rebuild();
            gg_save_file();
        }
    }

    if (kDown & (KEY_B | KEY_R)) {
        g_settings_sel = g_cheat_screen_gs ? SETTINGS_GS_INDEX : SETTINGS_GG_INDEX;
        g_state = APP_SETTINGS;
    }
}

static void draw_gamegenie_bottom(void) {
    draw_vgradient(320, 240, COL_BG_TOP, COL_BG_BOT);
    static CachedText ct_title, ct_hint, ct_empty, ct_code[GG_MAX_CODES];
    ct_ensure(&ct_title, g_cheat_screen_gs ? "Game Shark" : "Game Genie", 0.6f);
    ct_draw(&ct_title, 10, 8, 0.2f, 0.6f, COL_WHITE);

    int vis = cheat_visible_count();
    if (vis == 0) {
        ct_ensure(&ct_empty, "No codes. Press X to add one.", 0.5f);
        ct_draw(&ct_empty, 20, 100, 0.2f, 0.5f, COL_DIM);
    } else {
        const float rh = 26, y0 = 38;
        for (int v = 0; v < vis && v < GG_MAX_CODES; v++) {
            int idx = cheat_nth_visible(v);
            float ry = y0 + v * rh;
            bool sel = (v == g_gg_sel);
            if (sel) C2D_DrawRectSolid(8, ry, 0.2f, 304, rh - 3, pulse_accent());
            char lbl[72];
            snprintf(lbl, sizeof(lbl), "%s  (%s)  %s",
                     g_gg[idx].name, g_gg[idx].text, g_gg[idx].enabled ? "ON" : "off");
            ct_ensure(&ct_code[v], lbl, 0.46f);
            ct_draw(&ct_code[v], 18, ry + 4, 0.3f, 0.46f,
                    g_gg[idx].enabled ? (sel ? COL_WHITE : COL_GOLD) : COL_DIM);
        }
    }

    ct_ensure(&ct_hint, "X: add   A: on/off   Y: delete   B: back", 0.42f);
    ct_draw(&ct_hint, 10, 220, 0.2f, 0.42f, COL_DIM);
}

static void update_controls(u32 kDown, u32 kHeld) {
    if (g_controls_listening) {
        if (kDown & (KEY_L | KEY_R)) { g_controls_listening = false; return; }
        for (int i = 0; i < KEY_OPT_COUNT; i++) {
            if (kDown & KEY_OPTS[i].mask) {
                g_keymap[GB_ROWS[g_controls_sel].gb_idx] = KEY_OPTS[i].mask;
                g_controls_listening = false;
                save_settings();
                return;
            }
        }
        return;
    }

    u32 nav = nav_repeat(kDown, kHeld, KEY_UP, KEY_DOWN);
    if (nav & KEY_UP)   g_controls_sel = (g_controls_sel - 1 + 8) % 8;
    if (nav & KEY_DOWN) g_controls_sel = (g_controls_sel + 1) % 8;

    if (kDown & KEY_A) g_controls_listening = true;
    if (kDown & KEY_B) { g_state = APP_SETTINGS; }
}

static void draw_settings_bottom(void) {
    static CachedText ct_title, ct_item[SETTINGS_ITEM_COUNT], ct_value[SETTINGS_ITEM_COUNT], ct_hint;

    draw_vgradient(320, 240, COL_BG_BOT, COL_BG_TOP);

    ct_ensure(&ct_title, "Quick Settings", 0.55f);
    ct_draw(&ct_title, (320 - ct_title.w) / 2, 8, 0.2f, 0.55f, COL_WHITE);
    C2D_DrawRectSolid(40, 32, 0.1f, 240, 1, COL_PANEL_EDGE);

    const float iy = 38, ih = 21, gap = 4;
    static char gg_val[16];
    for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
        bool sel = (g_settings_sel == i);
        float ry = iy + i * (ih + gap);

        const char *val_text;
        u32 val_color;
        if (i == 0) { val_text = g_frameskip ? "ON" : "OFF"; val_color = g_frameskip ? COL_GOLD : COL_DIM; }
        else if (i == 1) { val_text = g_show_fps ? "ON" : "OFF"; val_color = g_show_fps ? COL_GOLD : COL_DIM; }
        else if (i == 2) { val_text = g_screen_stretch ? "Stretch" : "GB Size"; val_color = COL_GOLD; }
        else if (i == SETTINGS_APU_INDEX) { val_text = g_apu_enabled ? "ON" : "OFF"; val_color = g_apu_enabled ? COL_GOLD : COL_DIM; }
        else if (i == SETTINGS_CLOCK_INDEX) {
            static char clk[16];
            if (g_gb_clock_mode == CLOCK_CUSTOM) snprintf(clk, sizeof(clk), "%02d:%02d", g_gb_clock_custom_h, g_gb_clock_custom_m);
            else snprintf(clk, sizeof(clk), "%s", g_gb_clock_mode == CLOCK_SYSTEM ? "System" : "Frozen");
            val_text = clk; val_color = g_gb_clock_mode == CLOCK_FROZEN ? COL_DIM : COL_GOLD;
        }
        else if (i == SETTINGS_GG_INDEX || i == SETTINGS_GS_INDEX) {
            bool gs = (i == SETTINGS_GS_INDEX);
            int n = 0; for (int k = 0; k < g_gg_count; k++) if (g_gg[k].is_gs == gs) n++;
            snprintf(gg_val, sizeof(gg_val), "%d codes >", n);
            val_text = gg_val; val_color = COL_ACCENT;
        }
        else { val_text = "Open >"; val_color = COL_ACCENT; }

        C2D_DrawRectSolid(40, ry, 0.15f, 240, ih, sel ? pulse_accent() : COL_PANEL);

        ct_ensure(&ct_item[i], g_settings_items[i], 0.44f);
        ct_draw(&ct_item[i], 52, ry + (ih - 14) / 2, 0.2f, 0.44f, sel ? COL_WHITE : COL_DIM);

        ct_ensure(&ct_value[i], val_text, 0.44f);
        ct_draw(&ct_value[i], 40 + 240 - 12 - ct_value[i].w, ry + (ih - 14) / 2, 0.2f, 0.44f, val_color);
    }

    ct_ensure(&ct_hint, "A: Select    R/B: Close", 0.42f);
    ct_draw(&ct_hint, (320 - ct_hint.w) / 2, 222, 0.2f, 0.42f, COL_DIM);
}

static void draw_controls_top(void) {
    static CachedText ct_title, ct_row[8], ct_val[8], ct_hint;

    draw_vgradient(400, 240, COL_BG_TOP, COL_BG_BOT);
    C2D_DrawRectSolid(0, 0, 0, 400, 4, COL_GOLD);

    ct_ensure(&ct_title, "Controller Config", 0.7f);
    ct_draw(&ct_title, (400 - ct_title.w) / 2, 10, 0.2f, 0.7f, COL_WHITE);
    C2D_DrawRectSolid(30, 40, 0.1f, 340, 1, COL_PANEL_EDGE);

    const float iy = 46, ih = 19, gap = 2;
    for (int i = 0; i < 8; i++) {
        bool sel = (g_controls_sel == i);
        float ry = iy + i * (ih + gap);

        C2D_DrawRectSolid(30, ry, 0.15f, 340, ih, sel ? pulse_accent() : COL_PANEL);
        if (sel) C2D_DrawRectSolid(30, ry, 0.2f, 4, ih, COL_GOLD);

        char lbl[32];
        snprintf(lbl, sizeof(lbl), "GB %s", GB_ROWS[i].label);
        ct_ensure(&ct_row[i], lbl, 0.5f);
        ct_draw(&ct_row[i], 44, ry + (ih - 16) / 2, 0.2f, 0.5f, sel ? COL_WHITE : COL_DIM);

        bool listening_here = sel && g_controls_listening;
        const char *vtext = listening_here ? "press a button..."
                                            : key_mask_name(g_keymap[GB_ROWS[i].gb_idx]);
        u32 vcolor = listening_here ? COL_GOLD : (sel ? COL_WHITE : COL_DIRTXT);
        ct_ensure(&ct_val[i], vtext, 0.5f);
        ct_draw(&ct_val[i], 366 - ct_val[i].w, ry + (ih - 16) / 2, 0.2f, 0.5f, vcolor);
    }

    ct_ensure(&ct_hint, "L = Pause, R = Settings are fixed and not remappable", 0.4f);
    ct_draw(&ct_hint, (400 - ct_hint.w) / 2, 222, 0.2f, 0.4f, COL_DIM);
}

static void draw_controls_bottom(void) {
    static CachedText ct_hint1, ct_hint2;

    draw_vgradient(320, 240, COL_BG_BOT, COL_BG_TOP);

    const char *h1 = g_controls_listening
        ? "Press the 3DS button to bind"
        : "D-Pad: Pick   A: Rebind";
    ct_ensure(&ct_hint1, h1, 0.55f);
    ct_draw(&ct_hint1, (320 - ct_hint1.w) / 2, 96, 0.2f, 0.55f, COL_WHITE);

    const char *h2 = g_controls_listening
        ? "L or R: Cancel"
        : "B: Back to Settings";
    ct_ensure(&ct_hint2, h2, 0.5f);
    ct_draw(&ct_hint2, (320 - ct_hint2.w) / 2, 128, 0.2f, 0.5f, COL_DIM);
}

static C3D_RenderTarget *g_top, *g_bottom;

static void fatal_error(const char *line1, const char *line2) {
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;

        C2D_TextBufClear(g_txtbuf);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        C2D_TargetClear(g_top, C2D_Color32(0x40, 0x00, 0x00, 0xFF));
        C2D_SceneBegin(g_top);
        draw_text("Fatal error", 16, 16, 0.2f, 0.7f, COL_WHITE, 0);
        draw_text(line1, 16, 60, 0.2f, 0.5f, COL_WHITE, 0);
        draw_text(line2, 16, 84, 0.2f, 0.5f, COL_WHITE, 0);
        draw_text("Press START to exit", 16, 200, 0.2f, 0.5f, COL_DIM, 0);

        C2D_TargetClear(g_bottom, C2D_Color32(0x40, 0x00, 0x00, 0xFF));
        C2D_SceneBegin(g_bottom);

        C3D_FrameEnd(0);
    }
}

static bool pt_in(touchPosition t, float x, float y, float w, float h) {
    return t.px >= x && t.px <= x + w && t.py >= y && t.py <= y + h;
}

static void ui_popup(const char* title, const char* msg) {
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        touchPosition tp; hidTouchRead(&tp);

        const float bw = 280, bh = 150, bx = (320 - bw) / 2, by = (240 - bh) / 2;
        const float okw = 120, okh = 38, okx = bx + (bw - okw) / 2, oky = by + bh - 50;
        bool ok = (kDown & (KEY_A | KEY_B | KEY_START)) ||
                  ((kDown & KEY_TOUCH) && pt_in(tp, okx, oky, okw, okh));

        C2D_TextBufClear(g_txtbuf);
        C3D_FrameBegin(0);
        C2D_TargetClear(g_top, COL_BG_TOP);
        C2D_SceneBegin(g_top);
        draw_vgradient(400, 240, COL_BG_TOP, COL_BG_BOT);

        C2D_TargetClear(g_bottom, COL_BG_TOP);
        C2D_SceneBegin(g_bottom);
        draw_vgradient(320, 240, COL_BG_BOT, COL_BG_TOP);
        C2D_DrawRectSolid(bx - 2, by - 2, 0.3f, bw + 4, bh + 4, COL_PURPLE_EDG);
        C2D_DrawRectSolid(bx, by, 0.35f, bw, bh, COL_PURPLE_BOX);
        C2D_DrawRectSolid(bx, by, 0.40f, bw, 3, COL_GOLD);
        MeasuredText mt = measure_text(title, 0.6f);
        draw_text(title, bx + (bw - mt.w) / 2, by + 16, 0.5f, 0.6f, COL_WHITE, 0);
        MeasuredText mm = measure_text(msg, 0.5f);
        draw_text(msg, bx + (bw - mm.w) / 2, by + 58, 0.5f, 0.5f, COL_WHITE, 0);
        C2D_DrawRectSolid(okx, oky, 0.5f, okw, okh, pulse_accent());
        MeasuredText mo = measure_text("OK", 0.55f);
        draw_text("OK", okx + (okw - mo.w) / 2, oky + (okh - mo.h) / 2, 0.55f, 0.55f, COL_WHITE, 0);
        C3D_FrameEnd(0);

        if (ok) break;
        frame_pace();
    }
}

static bool ui_confirm(const char* title, const char* msg) {
    bool result = false;
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        touchPosition tp; hidTouchRead(&tp);

        const float bw = 292, bh = 150, bx = (320 - bw) / 2, by = (240 - bh) / 2;
        const float btw = 110, bth = 38, bty = by + bh - 50;
        const float yesx = bx + 20, nox = bx + bw - 20 - btw;

        bool yes = (kDown & KEY_A) || ((kDown & KEY_TOUCH) && pt_in(tp, yesx, bty, btw, bth));
        bool no  = (kDown & (KEY_B | KEY_START)) || ((kDown & KEY_TOUCH) && pt_in(tp, nox, bty, btw, bth));

        C2D_TextBufClear(g_txtbuf);
        C3D_FrameBegin(0);
        C2D_TargetClear(g_top, COL_BG_TOP);
        C2D_SceneBegin(g_top);
        draw_vgradient(400, 240, COL_BG_TOP, COL_BG_BOT);

        C2D_TargetClear(g_bottom, COL_BG_TOP);
        C2D_SceneBegin(g_bottom);
        draw_vgradient(320, 240, COL_BG_BOT, COL_BG_TOP);
        C2D_DrawRectSolid(bx - 2, by - 2, 0.3f, bw + 4, bh + 4, COL_PURPLE_EDG);
        C2D_DrawRectSolid(bx, by, 0.35f, bw, bh, COL_PURPLE_BOX);
        C2D_DrawRectSolid(bx, by, 0.40f, bw, 3, COL_GOLD);
        MeasuredText mt = measure_text(title, 0.55f);
        draw_text(title, bx + (bw - mt.w) / 2, by + 16, 0.5f, 0.55f, COL_WHITE, 0);
        MeasuredText mm = measure_text(msg, 0.45f);
        draw_text(msg, bx + (bw - mm.w) / 2, by + 56, 0.5f, 0.45f, COL_WHITE, 0);

        C2D_DrawRectSolid(yesx, bty, 0.5f, btw, bth, pulse_accent());
        MeasuredText my = measure_text("Yes (A)", 0.5f);
        draw_text("Yes (A)", yesx + (btw - my.w) / 2, bty + (bth - my.h) / 2, 0.55f, 0.5f, COL_WHITE, 0);
        C2D_DrawRectSolid(nox, bty, 0.5f, btw, bth, COL_SLOT_FILL);
        MeasuredText mn = measure_text("No (B)", 0.5f);
        draw_text("No (B)", nox + (btw - mn.w) / 2, bty + (bth - mn.h) / 2, 0.55f, 0.5f, COL_WHITE, 0);
        C3D_FrameEnd(0);

        if (yes) { result = true; break; }
        if (no)  { result = false; break; }
        frame_pace();
    }
    return result;
}

enum { ACT_CHAR = 0, ACT_BACK, ACT_CLEAR, ACT_SPACE, ACT_SHIFT, ACT_OK, ACT_CANCEL };
struct KbKey { char label[8]; int act; char ch; };

static bool ui_keyboard(int mode, const char* title, const char* initial, char* out, int outsz) {
    const char* rows[5]; int nrows = 0; int maxlen;
    if (mode == KBM_TEXT) {
        rows[0] = "1234567890"; rows[1] = "qwertyuiop"; rows[2] = "asdfghjkl"; rows[3] = "zxcvbnm"; nrows = 4;
        maxlen = outsz - 1; if (maxlen > 30) maxlen = 30;
    } else if (mode == KBM_NUM) {
        rows[0] = "123"; rows[1] = "456"; rows[2] = "789"; rows[3] = "0"; nrows = 4; maxlen = 4;
    } else {
        rows[0] = "0123456789"; rows[1] = "ABCDEF"; nrows = 2; maxlen = (mode == KBM_HEX_GS) ? 8 : 9;
    }

    KbKey grid[6][12]; int rowlen[6]; int nrow = nrows + 1;
    for (int r = 0; r < nrows; r++) {
        int n = (int)strlen(rows[r]);
        rowlen[r] = n;
        for (int c = 0; c < n; c++) { grid[r][c].act = ACT_CHAR; grid[r][c].ch = rows[r][c];
            grid[r][c].label[0] = rows[r][c]; grid[r][c].label[1] = 0; }
    }
    int cr = nrows;
    auto setctl = [&](int idx, const char* lbl, int act) {
        strncpy(grid[cr][idx].label, lbl, 7); grid[cr][idx].label[7] = 0; grid[cr][idx].act = act; grid[cr][idx].ch = 0;
    };
    if (mode == KBM_TEXT) { rowlen[cr] = 5; setctl(0,"Shift",ACT_SHIFT); setctl(1,"Space",ACT_SPACE); setctl(2,"Del",ACT_BACK); setctl(3,"Cancel",ACT_CANCEL); setctl(4,"OK",ACT_OK); }
    else                  { rowlen[cr] = 4; setctl(0,"Del",ACT_BACK); setctl(1,"Clear",ACT_CLEAR); setctl(2,"Cancel",ACT_CANCEL); setctl(3,"OK",ACT_OK); }

    char raw[40] = {0};
    if (initial) { int k = 0; for (const char* p = initial; *p && k < maxlen; p++) if (*p != '-') raw[k++] = *p; raw[k] = 0; }
    int len = (int)strlen(raw);
    bool shift = false;
    int crow = 0, ccol = 0;

    const float kb_top = 56.0f;
    const float rowh = (240.0f - kb_top - 6.0f) / nrow;

    auto keyrect = [&](int r, int c, float& x, float& y, float& w, float& h) {
        float pad = 4.0f;
        float rw = (320.0f - 2 * pad) / rowlen[r];
        x = pad + c * rw + 2; y = kb_top + r * rowh + 2; w = rw - 4; h = rowh - 4;
    };

    bool result = false;
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        touchPosition tp; hidTouchRead(&tp);

        if (kDown & KEY_UP)    { crow = (crow - 1 + nrow) % nrow; }
        if (kDown & KEY_DOWN)  { crow = (crow + 1) % nrow; }
        if (crow >= nrow) crow = nrow - 1;
        if (ccol >= rowlen[crow]) ccol = rowlen[crow] - 1;
        if (kDown & KEY_LEFT)  { ccol = (ccol - 1 + rowlen[crow]) % rowlen[crow]; }
        if (kDown & KEY_RIGHT) { ccol = (ccol + 1) % rowlen[crow]; }

        int selr = -1, selc = -1;
        if (kDown & KEY_A) { selr = crow; selc = ccol; }
        if (kDown & KEY_TOUCH) {
            for (int r = 0; r < nrow && selr < 0; r++)
                for (int c = 0; c < rowlen[r]; c++) {
                    float x, y, w, h; keyrect(r, c, x, y, w, h);
                    if (pt_in(tp, x, y, w, h)) { selr = r; selc = c; crow = r; ccol = c; break; }
                }
        }
        if (kDown & KEY_B) { result = false; break; }

        if (selr >= 0) {
            KbKey& k = grid[selr][selc];
            switch (k.act) {
                case ACT_CHAR: if (len < maxlen) { char ch = k.ch; if (mode == KBM_TEXT && shift && ch >= 'a' && ch <= 'z') ch -= 32; raw[len++] = ch; raw[len] = 0; } break;
                case ACT_BACK: if (len > 0) raw[--len] = 0; break;
                case ACT_CLEAR: len = 0; raw[0] = 0; break;
                case ACT_SPACE: if (len < maxlen) { raw[len++] = ' '; raw[len] = 0; } break;
                case ACT_SHIFT: shift = !shift; break;
                case ACT_CANCEL: result = false; goto done;
                case ACT_OK: result = true; goto done;
            }
        }

        char disp[48]; int di = 0;
        if (mode == KBM_HEX_GG) {
            for (int i = 0; i < len && di < 46; i++) { if (i > 0 && i % 3 == 0) disp[di++] = '-'; disp[di++] = raw[i]; }
            disp[di] = 0;
        } else { strncpy(disp, raw, sizeof(disp) - 1); disp[sizeof(disp)-1] = 0; }

        C2D_TextBufClear(g_txtbuf);
        C3D_FrameBegin(0);
        C2D_TargetClear(g_top, COL_BG_TOP);
        C2D_SceneBegin(g_top);
        draw_vgradient(400, 240, COL_BG_TOP, COL_BG_BOT);
        C2D_DrawRectSolid(0, 0, 0, 400, 4, COL_GOLD);
        MeasuredText mt = measure_text(title, 0.7f);
        draw_text(title, (400 - mt.w) / 2, 90, 0.2f, 0.7f, COL_WHITE, 0);
        MeasuredText md = measure_text(disp[0] ? disp : "_", 1.0f);
        draw_text(disp[0] ? disp : "_", (400 - md.w) / 2, 140, 0.2f, 1.0f, COL_GOLD, 0);

        C2D_TargetClear(g_bottom, COL_BG_TOP);
        C2D_SceneBegin(g_bottom);
        draw_vgradient(320, 240, COL_BG_BOT, COL_BG_TOP);
        C2D_DrawRectSolid(6, 8, 0.1f, 308, 40, COL_PANEL);
        C2D_DrawRectSolid(6, 8, 0.15f, 308, 3, COL_PURPLE_EDG);
        draw_text(disp[0] ? disp : "", 14, 18, 0.2f, 0.6f, COL_WHITE, 0);
        for (int r = 0; r < nrow; r++) {
            for (int c = 0; c < rowlen[r]; c++) {
                float x, y, w, h; keyrect(r, c, x, y, w, h);
                bool sel = (r == crow && c == ccol);
                bool isShiftOn = (grid[r][c].act == ACT_SHIFT && shift);
                C2D_DrawRectSolid(x, y, 0.2f, w, h, sel ? pulse_accent() : (isShiftOn ? COL_ACCENT : COL_PANEL));
                if (sel) C2D_DrawRectSolid(x, y, 0.22f, w, 2, COL_GOLD);
                const char* lbl = grid[r][c].label;
                char up[2]; if (grid[r][c].act == ACT_CHAR && mode == KBM_TEXT && shift && lbl[0] >= 'a' && lbl[0] <= 'z') { up[0] = lbl[0] - 32; up[1] = 0; lbl = up; }
                MeasuredText mk = measure_text(lbl, 0.5f);
                draw_text(lbl, x + (w - mk.w) / 2, y + (h - mk.h) / 2, 0.3f, 0.5f, sel ? COL_WHITE : COL_DIM, 0);
            }
        }
        C3D_FrameEnd(0);
        frame_pace();
    }
done:
    if (result) {
        char disp[48]; int di = 0;
        if (mode == KBM_HEX_GG) { for (int i = 0; i < len && di < 46; i++) { if (i > 0 && i % 3 == 0) disp[di++] = '-'; disp[di++] = raw[i]; } disp[di] = 0; strncpy(out, disp, outsz - 1); }
        else strncpy(out, raw, outsz - 1);
        out[outsz - 1] = 0;
    }
    return result;
}

static C3D_Tex           g_print_tex;
static Tex3DS_SubTexture g_print_subtex;
static C2D_Image         g_print_image;
static u32*              g_print_linear = nullptr;
static bool              g_print_tex_ok = false;
#define PRINT_TEX_DIM 256

static void print_tex_init(void) {
    C3D_TexInit(&g_print_tex, PRINT_TEX_DIM, PRINT_TEX_DIM, GPU_RGBA8);
    C3D_TexSetFilter(&g_print_tex, GPU_NEAREST, GPU_NEAREST);
    g_print_linear = (u32*)linearAlloc(PRINT_TEX_DIM * PRINT_TEX_DIM * sizeof(u32));
}

static bool print_tex_upload(void) {
    g_print_tex_ok = false;
    int W = printer.img_w, H = printer.img_h;
    if (W <= 0 || H <= 0 || !g_print_linear) return false;
    if (H > PRINT_TEX_DIM) H = PRINT_TEX_DIM;
    static const u8 sg[4] = { 0xFF, 0xAA, 0x55, 0x00 };
    memset(g_print_linear, 0, PRINT_TEX_DIM * PRINT_TEX_DIM * sizeof(u32));
    for (int y = 0; y < H; y++) {
        u32* dst = g_print_linear + y * PRINT_TEX_DIM;
        for (int x = 0; x < W; x++) {
            u8 g = sg[printer.image[(size_t)y * printer.img_w + x] & 3];
            dst[x] = ((u32)g << 24) | ((u32)g << 16) | ((u32)g << 8) | 0xFFu;
        }
    }
    GSPGPU_FlushDataCache(g_print_linear, PRINT_TEX_DIM * PRINT_TEX_DIM * sizeof(u32));
    GX_DisplayTransfer(
        g_print_linear, GX_BUFFER_DIM(PRINT_TEX_DIM, PRINT_TEX_DIM),
        (u32*)g_print_tex.data, GX_BUFFER_DIM(PRINT_TEX_DIM, PRINT_TEX_DIM),
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    gspWaitForPPF();
    g_print_subtex.width = W; g_print_subtex.height = H;
    g_print_subtex.left = 0.0f; g_print_subtex.top = 1.0f;
    g_print_subtex.right = (float)W / PRINT_TEX_DIM;
    g_print_subtex.bottom = 1.0f - (float)H / PRINT_TEX_DIM;
    g_print_image.tex = &g_print_tex; g_print_image.subtex = &g_print_subtex;
    g_print_tex_ok = true;
    return true;
}

static void print_unload(void) {
    printer.image.clear(); printer.image.shrink_to_fit();
    printer.tiles.clear(); printer.tiles.shrink_to_fit();
    printer.img_w = printer.img_h = 0;
    g_print_tex_ok = false;
    LOGI(LOG_SYS, "GB Printer: image unloaded from memory");
}

#define PRINT_MAX_FILES 64
static bool pick_print_filename(char* out, int outsz) {
    static char files[PRINT_MAX_FILES][40];
    int nfiles = 0;
    mkdir(PRINTS_DIR, 0777);
    DIR* d = opendir(PRINTS_DIR);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL && nfiles < PRINT_MAX_FILES) {
            size_t l = strlen(e->d_name);
            if (l > 4 && strcasecmp(e->d_name + l - 4, ".bmp") == 0) {
                strncpy(files[nfiles], e->d_name, sizeof(files[0]) - 1);
                files[nfiles][sizeof(files[0]) - 1] = '\0';
                nfiles++;
            }
        }
        closedir(d);
    }
    int total = nfiles + 1;
    int sel = 0, scroll = 0;
    const int VIS = 8;
    const float rh = 22.0f, y0 = 34.0f;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown(), kHeld = hidKeysHeld();
        touchPosition tp; hidTouchRead(&tp);
        u32 nav = nav_repeat(kDown, kHeld, KEY_UP, KEY_DOWN);
        if (nav & KEY_UP)   sel = (sel - 1 + total) % total;
        if (nav & KEY_DOWN) sel = (sel + 1) % total;
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + VIS) scroll = sel - VIS + 1;

        if (kDown & KEY_TOUCH) {
            for (int i = 0; i < VIS && scroll + i < total; i++)
                if (pt_in(tp, 8, y0 + i * rh, 304, rh - 2)) { sel = scroll + i; kDown |= KEY_A; break; }
        }
        if (kDown & KEY_B) return false;
        if (kDown & KEY_A) {
            if (sel == 0) {
                char name[24] = {0};
                if (ui_keyboard(KBM_TEXT, "New print filename", "gbprint", name, sizeof(name)) && name[0]) {
                    strncpy(out, name, outsz - 1); out[outsz - 1] = '\0';
                    return true;
                }
            } else {
                char nm[40];
                strncpy(nm, files[sel - 1], sizeof(nm) - 1); nm[sizeof(nm) - 1] = '\0';
                size_t l = strlen(nm); if (l > 4) nm[l - 4] = '\0';
                strncpy(out, nm, outsz - 1); out[outsz - 1] = '\0';
                return true;
            }
        }

        C2D_TextBufClear(g_txtbuf);
        C3D_FrameBegin(0);
        C2D_TargetClear(g_top, COL_BG_TOP);
        C2D_SceneBegin(g_top);
        draw_playing_top();
        C2D_TargetClear(g_bottom, COL_BG_TOP);
        C2D_SceneBegin(g_bottom);
        draw_vgradient(320, 240, COL_BG_BOT, COL_BG_TOP);
        MeasuredText mt = measure_text("Save print - pick a name", 0.5f);
        draw_text("Save print - pick a name", (320 - mt.w) / 2, 4, 0.2f, 0.5f, COL_GOLD, 0);

        for (int i = 0; i < VIS && scroll + i < total; i++) {
            int idx = scroll + i;
            float ry = y0 + i * rh;
            bool s = (idx == sel);
            C2D_DrawRectSolid(8, ry, 0.2f, 304, rh - 2, s ? pulse_accent() : COL_PANEL);
            const char* lbl = (idx == 0) ? "[ New name... ]" : files[idx - 1];
            draw_text(lbl, 16, ry + 3, 0.3f, 0.45f, s ? COL_WHITE : COL_DIM, 0);
        }
        MeasuredText mh = measure_text("A: pick   B: cancel   (gb-prints folder only)", 0.4f);
        draw_text("A: pick   B: cancel   (gb-prints folder only)", (320 - mh.w) / 2, 222, 0.2f, 0.4f, COL_DIM, 0);
        C3D_FrameEnd(0);
        frame_pace();
    }
    return false;
}

static void handle_print(void) {
    ndspChnSetPaused(0, true);
    bool have = print_tex_upload();

    const float img_w = 160.0f;
    const float img_h = (float)(printer.img_h > 0 ? printer.img_h : 1);
    const float avail_w = 304.0f, avail_h = 240.0f - 22.0f - 44.0f;
    float scale = avail_w / img_w;
    if (img_h * scale > avail_h) scale = avail_h / img_h;
    if (scale < 1.0f) scale = 1.0f;
    const float dw = img_w * scale, dh = img_h * scale;
    const float px = (320.0f - dw) / 2.0f, py = 20.0f;

    float revealed = 0.0f;
    bool done = false;
    int sel = 1;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        touchPosition tp; hidTouchRead(&tp);

        const float bw = 130, bh = 32, by = 240 - bh - 6;
        const float b1x = 12, b2x = 320 - bw - 12;

        if (!done) {
            revealed += img_h / 1200.0f;
            if (kDown & (KEY_A | KEY_TOUCH | KEY_B)) revealed = img_h;
            if (revealed >= img_h) { revealed = img_h; done = true; }
        } else {
            if (kDown & (KEY_LEFT | KEY_RIGHT)) sel ^= 1;
            bool t_close = (kDown & KEY_TOUCH) && pt_in(tp, b1x, by, bw, bh);
            bool t_save  = (kDown & KEY_TOUCH) && pt_in(tp, b2x, by, bw, bh);
            if (t_close) sel = 0;
            if (t_save)  sel = 1;
            if ((kDown & KEY_A) || t_close || t_save) {
                if (sel == 1) {
                    char name[24] = {0};
                    if (pick_print_filename(name, sizeof(name)) && name[0]) {
                        mkdir(PRINTS_DIR, 0777);
                        char path[600];
                        snprintf(path, sizeof(path), "%s/%s.bmp", PRINTS_DIR, name);
                        save_printer_bmp(path);
                    } else {
                        continue;
                    }
                }
                print_unload();
                break;
            }
            if (kDown & KEY_B) { print_unload(); break; }
        }

        C2D_TextBufClear(g_txtbuf);
        C3D_FrameBegin(0);
        C2D_TargetClear(g_top, COL_BG_TOP);
        C2D_SceneBegin(g_top);
        draw_playing_top();

        C2D_TargetClear(g_bottom, COL_BG_TOP);
        C2D_SceneBegin(g_bottom);
        draw_vgradient(320, 240, COL_BG_BOT, COL_BG_TOP);
        MeasuredText mt = measure_text(done ? "GB Printer - done" : "GB Printer - printing...", 0.5f);
        draw_text(done ? "GB Printer - done" : "GB Printer - printing...",
                  (320 - mt.w) / 2, 3, 0.2f, 0.5f, COL_GOLD, 0);

        C2D_DrawRectSolid(px - 3, py - 3, 0.2f, dw + 6, dh + 6, COL_PANEL_EDGE);
        C2D_DrawRectSolid(px, py, 0.25f, dw, dh, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
        if (have && g_print_tex_ok) {
            int rows = (int)revealed; if (rows < 1) rows = 1;
            if (rows > printer.img_h && printer.img_h > 0) rows = printer.img_h;
            g_print_subtex.height = (u16)rows;
            g_print_subtex.bottom = 1.0f - (float)rows / PRINT_TEX_DIM;
            C2D_DrawImageAt(g_print_image, px, py, 0.3f, NULL, scale, scale);
            if (!done) C2D_DrawRectSolid(px, py + revealed * scale, 0.35f, dw, 2, COL_ACCENT);
        }

        if (!done) {
            C2D_DrawRectSolid(b1x, by, 0.3f, 320 - 24, bh, pulse_accent());
            MeasuredText mh = measure_text("Hurry Up !!", 0.55f);
            draw_text("Hurry Up !!", (320 - mh.w) / 2, by + (bh - mh.h) / 2, 0.4f, 0.55f, COL_WHITE, 0);
        } else {
            const char* lbl[2] = { "Close", "Save" };
            float bx[2] = { b1x, b2x };
            for (int i = 0; i < 2; i++) {
                C2D_DrawRectSolid(bx[i], by, 0.3f, bw, bh, (sel == i) ? pulse_accent() : COL_PANEL);
                if (sel == i) C2D_DrawRectSolid(bx[i], by, 0.32f, bw, 2, COL_GOLD);
                MeasuredText ml = measure_text(lbl[i], 0.55f);
                draw_text(lbl[i], bx[i] + (bw - ml.w) / 2, by + (bh - ml.h) / 2, 0.4f, 0.55f,
                          (sel == i) ? COL_WHITE : COL_DIM, 0);
            }
        }
        C3D_FrameEnd(0);
        frame_pace();
    }
    ndspChnSetPaused(0, false);
}

int main(void) {
    gfxInitDefault();
    osSetSpeedupEnable(true);
    gfxSet3D(false);

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    g_top    = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    g_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    g_txtbuf = C2D_TextBufNew(4096);

    g_static_txtbuf = C2D_TextBufNew(256);
    C2D_TextParse(&g_ct_playing_hint, g_static_txtbuf, "R: Pause");
    C2D_TextOptimize(&g_ct_playing_hint);
    C2D_TextGetDimensions(&g_ct_playing_hint, 0.6f, 0.6f, &g_ct_playing_hint_w, &g_ct_playing_hint_h);

    gb_tex_init();

    if (!g_gb_linear_buf[0] || !g_gb_linear_buf[1] || !g_gb_tex.data) {
        fatal_error("Could not allocate GPU texture", "buffers for the Game Boy screen.");

        C2D_TextBufDelete(g_txtbuf);
        C2D_TextBufDelete(g_static_txtbuf);
        C2D_Fini();
        C3D_Fini();
        gfxExit();
        return 1;
    }

    audio_init();

    log_set_sink(log3ds_sink);
    log_set_min_level(LOG_DEBUG);
    log_print_banner("Game Boy Emu by TS");

    ensure_saves_dir();
    ss_ensure_dir();
    saver_init();
    preview_init();
    print_tex_init();
    load_last_dir(g_cur_path, sizeof(g_cur_path));
    keymap_defaults();
    load_settings();

    pacing_calibrate();

    g_fps_window_ms = osGetTime();

    while (aptMainLoop() && !g_want_quit_app) {
        g_loop_start = svcGetSystemTick();
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if ((kDown & KEY_L) && (g_state == APP_MENU || g_state == APP_BROWSE))
            g_show_log = !g_show_log;
        update_log_touch();

        g_anim_t += 1.0f / 60.0f;

        switch (g_state) {
            case APP_MENU:   update_menu(kDown, kHeld); break;
            case APP_BROWSE: update_browse(kDown, kHeld); break;
            case APP_PLAYING: {
                apply_gb_input(kHeld);
                cam_menu_touch(kDown);

                bool skip_upload = g_frameskip && (g_frame_parity & 1);
                g_frame_parity ^= 1;
                ppu.render_enabled = !skip_upload;

                u64 pt0 = svcGetSystemTick();
                gb_run_frame();
                gs_apply_frame();
                if (cart.has_camera) cam_update();
                u64 pt1 = svcGetSystemTick();

                if (printer.print_ready) { printer.print_ready = false; handle_print(); }
                if (!skip_upload) gb_tex_upload();
                u64 pt2 = svcGetSystemTick();
                audio_push_samples();
                u64 pt3 = svcGetSystemTick();

                g_ms_emulate = tick_ms(pt1 - pt0);
                g_ms_gpu     = tick_ms(pt2 - pt1);
                g_ms_audio   = tick_ms(pt3 - pt2);

                g_fps_frame_count++;
                u64 now = osGetTime();
                if (now - g_fps_window_ms >= 500) {
                    g_fps_value = g_fps_frame_count * 1000.0f / (float)(now - g_fps_window_ms);
                    g_fps_frame_count = 0;
                    g_fps_window_ms = now;
                }

                codecache_tick();

                g_save_tick++;
                if (cart.dirty && g_save_tick >= SAVE_INTERVAL) {
                    g_save_tick = 0;
                    if (write_save()) cart.dirty = false;
                }

                if (kDown & KEY_R) {
                    ndspChnSetPaused(0, true);
                    g_pause_sel = 0;
                    g_state = APP_PAUSED;
                }
                break;
            }
            case APP_PAUSED:
                update_pause(kDown, kHeld);
                break;
            case APP_SETTINGS:
                update_settings(kDown, kHeld);
                break;
            case APP_CONTROLS:
                update_controls(kDown, kHeld);
                break;
            case APP_SAVESTATES:
                update_savestates(kDown, kHeld);
                break;
            case APP_STATE_SAVE:
                update_state_slots(kDown, kHeld, true);
                break;
            case APP_STATE_LOAD:
                update_state_slots(kDown, kHeld, false);
                break;
            case APP_GAMEGENIE:
                update_gamegenie(kDown, kHeld);
                break;
        }

        audio_update();

        u64 pt4 = svcGetSystemTick();

        C2D_TextBufClear(g_txtbuf);
        C3D_FrameBegin(0);

        C2D_TargetClear(g_top, COL_BG_TOP);
        C2D_SceneBegin(g_top);
        switch (g_state) {
            case APP_MENU:     draw_menu_top(); break;
            case APP_BROWSE:   draw_browse_top(); break;
            case APP_PLAYING:  draw_playing_top(); break;
            case APP_PAUSED:   draw_playing_top(); draw_pause_overlay(); break;
            case APP_SETTINGS: draw_playing_top(); break;
            case APP_CONTROLS: draw_controls_top(); break;
            case APP_SAVESTATES:
            case APP_STATE_SAVE:
            case APP_STATE_LOAD:
            case APP_GAMEGENIE:  draw_playing_top(); break;
        }

        C2D_TargetClear(g_bottom, COL_BG_TOP);
        C2D_SceneBegin(g_bottom);
        switch (g_state) {
            case APP_MENU:     draw_menu_bottom(); break;
            case APP_BROWSE:   draw_browse_bottom(); break;
            case APP_PLAYING:
            case APP_PAUSED:   draw_playing_bottom(); break;
            case APP_SETTINGS: draw_settings_bottom(); break;
            case APP_CONTROLS: draw_controls_bottom(); break;
            case APP_SAVESTATES: draw_savestates_bottom(); break;
            case APP_STATE_SAVE: draw_state_slots_bottom(true); break;
            case APP_STATE_LOAD: draw_state_slots_bottom(false); break;
            case APP_GAMEGENIE:  draw_gamegenie_bottom(); break;
        }
        if (g_state == APP_PLAYING || g_show_log) {
            draw_log_view();
        }
        if (g_state == APP_PLAYING && cam_menu_visible()) draw_camera_menu();

        C3D_FrameEnd(0);
        u64 pt5 = svcGetSystemTick();
        g_ms_present = tick_ms(pt5 - pt4);
        g_ms_work = tick_ms(pt5 - g_loop_start);

        frame_pace();
    }

    if (g_state == APP_PLAYING || g_state == APP_PAUSED || g_state == APP_SETTINGS || g_state == APP_CONTROLS) write_save_sync();
    codecache_save();
    gg_save_file();
    saver_exit();

    audio_exit();
    cam_shutdown();
    C3D_TexDelete(&g_gb_tex);
    linearFree(g_gb_linear_buf[0]);
    linearFree(g_gb_linear_buf[1]);
    C2D_TextBufDelete(g_txtbuf);
    C2D_TextBufDelete(g_static_txtbuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
