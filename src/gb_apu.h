#pragma once
#include "gb_types.h"
#include "gb_memory.h"
#include <array>
struct APU {
    static const int SAMPLE_RATE = 11025;
    static const int CPU_CLOCK = 4194304;
    static const int SAMPLE_BASE = CPU_CLOCK / SAMPLE_RATE;
    static const int SAMPLE_REM  = CPU_CLOCK % SAMPLE_RATE;
    std::array<s16, 4096> audio_buffer;
    int buffer_pos = 0;
    int frame_seq_step = 0;
    int frame_seq_countdown = 8192;
    int sample_countdown = SAMPLE_BASE;
    int sample_err = 0;
    int cycles_since_sample = 0;
    int hp_prev = 0;
    int hp_out  = 0;
    struct SquareChannel {
        bool enabled = false;
        u8 duty = 0;
        u8 duty_pos = 0;
        u8 volume = 0;
        u8 envelope_dir = 0;
        u8 envelope_period = 0;
        int envelope_counter = 0;
        u16 frequency = 0;
        int freq_counter = 0;
        u16 length_counter = 0;
        u8 sweep_period = 0;
        u8 sweep_shift = 0;
        u8 sweep_dir = 0;
        int sweep_counter = 0;
        bool sweep_enabled = false;
        u16 shadow_freq = 0;
        bool length_enable = false;
        bool dac_enabled = false;
    } ch1, ch2;
    struct WaveChannel {
        bool enabled = false;
        bool dac_enabled = false;
        u8 volume_code = 0;
        u16 length_counter = 0;
        u16 frequency = 0;
        int freq_counter = 0;
        u8 sample_pos = 0;
        bool length_enable = false;
    } ch3;
    struct NoiseChannel {
        bool enabled = false;
        u8 volume = 0;
        u8 envelope_dir = 0;
        u8 envelope_period = 0;
        int envelope_counter = 0;
        u16 lfsr = 0x7FFF;
        int lfsr_pos = 0;
        u8 divisor_code = 0;
        u8 width_mode = 0;
        u8 shift_amount = 0;
        int freq_counter = 0;
        u16 length_counter = 0;
        bool length_enable = false;
        bool dac_enabled = false;
    } ch4;
    void reset();
    void step(int cycles, Memory& mem);
    void write_reg(u16 addr, u8 val);
    void update_nr52(Memory& mem);
private:
    void trigger_ch1_helper(u8 nr14_val);
    void trigger_ch2_helper(u8 nr24_val);
    void trigger_ch3_helper(u8 nr34_val);
    void trigger_ch4_helper(u8 nr44_val);
    void tick_frame_sequencer();
    void generate_sample(Memory& mem);
    int read_square(const SquareChannel& ch);
    int read_wave(const WaveChannel& ch, Memory& mem);
    int read_noise(const NoiseChannel& ch);
    u16 noise_period() const;
};