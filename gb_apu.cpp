#include "gb_apu.h"
#include "gb_log.h"
#include <cstring>

static const u8 duty_waves[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 1, 1, 1},
    {0, 1, 1, 1, 1, 1, 1, 0},
};

static const u16 noise_divisors[8] = {
    8, 16, 32, 48, 64, 80, 96, 112
};

static const int g_noise_period[2] = { 32767, 127 };
static u8 g_noise_out[2][32768];
static bool g_noise_init = false;

static void init_noise_tables() {
    for (int wm = 0; wm < 2; wm++) {
        u16 lfsr = 0x7FFF;
        int period = g_noise_period[wm];
        for (int i = 0; i < period; i++) {
            g_noise_out[wm][i] = ((lfsr & 1) == 0) ? 1 : 0;
            u8 xor_r = (lfsr & 1) ^ ((lfsr >> 1) & 1);
            lfsr >>= 1;
            if (xor_r) lfsr |= 0x4000;
            if (wm) { lfsr &= ~0x40; if (xor_r) lfsr |= 0x40; lfsr &= 0x407F; }
        }
    }
    g_noise_init = true;
}

void APU::reset() {
    if (!g_noise_init) init_noise_tables();
    buffer_pos = 0;
    frame_seq_step = 0;
    frame_seq_countdown = 8192;
    sample_countdown = SAMPLE_BASE;
    sample_err = 0;
    cycles_since_sample = 0;
    hp_prev = 0;
    hp_out = 0;
    std::memset(&ch1, 0, sizeof(ch1));
    std::memset(&ch2, 0, sizeof(ch2));
    std::memset(&ch3, 0, sizeof(ch3));
    std::memset(&ch4, 0, sizeof(ch4));
    ch4.lfsr = 0x7FFF;
    ch4.lfsr_pos = 0;
}

u16 APU::noise_period() const {
    u16 base = noise_divisors[ch4.divisor_code & 7];
    return base << ch4.shift_amount;
}

void APU::write_reg(u16 addr, u8 val) {
    switch (addr) {
        case NR10:
            ch1.sweep_period = (val >> 4) & 7;
            ch1.sweep_dir    = (val >> 3) & 1;
            ch1.sweep_shift  = val & 7;
            break;
        case NR11:
            ch1.duty = (val >> 6) & 3;
            ch1.length_counter = 64 - (val & 63);
            break;
        case NR12:
            ch1.volume          = (val >> 4) & 0xF;
            ch1.envelope_dir    = (val >> 3) & 1;
            ch1.envelope_period = val & 7;
            ch1.dac_enabled     = (val & 0xF8) != 0;
            if (!ch1.dac_enabled) ch1.enabled = false;
            break;
        case NR13:
            ch1.frequency = (ch1.frequency & 0x700) | val;
            break;
        case NR14:
            ch1.frequency = (ch1.frequency & 0xFF) | ((val & 7) << 8);
            ch1.length_enable = (val >> 6) & 1;
            break;

        case NR21:
            ch2.duty = (val >> 6) & 3;
            ch2.length_counter = 64 - (val & 63);
            break;
        case NR22:
            ch2.volume          = (val >> 4) & 0xF;
            ch2.envelope_dir    = (val >> 3) & 1;
            ch2.envelope_period = val & 7;
            ch2.dac_enabled     = (val & 0xF8) != 0;
            if (!ch2.dac_enabled) ch2.enabled = false;
            break;
        case NR23:
            ch2.frequency = (ch2.frequency & 0x700) | val;
            break;
        case NR24:
            ch2.frequency = (ch2.frequency & 0xFF) | ((val & 7) << 8);
            ch2.length_enable = (val >> 6) & 1;
            break;

        case NR30:
            ch3.dac_enabled = (val & 0x80) != 0;
            if (!ch3.dac_enabled) ch3.enabled = false;
            break;
        case NR31:
            ch3.length_counter = 256 - val;
            break;
        case NR32:
            ch3.volume_code = (val >> 5) & 3;
            break;
        case NR33:
            ch3.frequency = (ch3.frequency & 0x700) | val;
            break;
        case NR34:
            ch3.frequency = (ch3.frequency & 0xFF) | ((val & 7) << 8);
            ch3.length_enable = (val >> 6) & 1;
            break;

        case NR41:
            ch4.length_counter = 64 - (val & 63);
            break;
        case NR42:
            ch4.volume          = (val >> 4) & 0xF;
            ch4.envelope_dir    = (val >> 3) & 1;
            ch4.envelope_period = val & 7;
            ch4.dac_enabled     = (val & 0xF8) != 0;
            if (!ch4.dac_enabled) ch4.enabled = false;
            break;
        case NR43:
            ch4.divisor_code  = val & 7;
            ch4.width_mode    = (val >> 3) & 1;
            ch4.shift_amount  = (val >> 4) & 0xF;
            break;
        case NR44:
            ch4.length_enable = (val >> 6) & 1;
            break;

        case NR52:
            LOGD(LOG_APU, "APU power %s", (val & 0x80) ? "ON" : "OFF");
            break;

        default:
            break;
    }

    if (addr == NR14 && (val & 0x80)) {
        trigger_ch1_helper(val);
        LOGT(LOG_APU, "CH1 trigger freq=%u vol=%u duty=%u", ch1.frequency, ch1.volume, ch1.duty);
    }
    if (addr == NR24 && (val & 0x80)) {
        trigger_ch2_helper(val);
        LOGT(LOG_APU, "CH2 trigger freq=%u vol=%u duty=%u", ch2.frequency, ch2.volume, ch2.duty);
    }
    if (addr == NR34 && (val & 0x80)) {
        trigger_ch3_helper(val);
        LOGT(LOG_APU, "CH3 trigger freq=%u", ch3.frequency);
    }
    if (addr == NR44 && (val & 0x80)) {
        trigger_ch4_helper(val);
        LOGT(LOG_APU, "CH4 trigger vol=%u divisor=%u shift=%u", ch4.volume, ch4.divisor_code, ch4.shift_amount);
    }
}

void APU::trigger_ch1_helper(u8 nr14_val) {
    ch1.enabled = ch1.dac_enabled;
    ch1.freq_counter = (2048 - ch1.frequency) * 4;
    ch1.duty_pos = 0;
    ch1.envelope_counter = ch1.envelope_period;
    if (ch1.length_counter == 0) ch1.length_counter = 64;
    ch1.shadow_freq = ch1.frequency;
    ch1.sweep_counter = ch1.sweep_period ? ch1.sweep_period : 8;
    ch1.sweep_enabled = ch1.sweep_period > 0 || ch1.sweep_shift > 0;
    if (ch1.sweep_shift > 0) {
        u16 delta = ch1.shadow_freq >> ch1.sweep_shift;
        u16 new_freq = ch1.sweep_dir
            ? ch1.shadow_freq - delta
            : ch1.shadow_freq + delta;
        if (new_freq > 2047) ch1.enabled = false;
    }
}

void APU::trigger_ch2_helper(u8 nr24_val) {
    ch2.enabled = ch2.dac_enabled;
    ch2.freq_counter = (2048 - ch2.frequency) * 4;
    ch2.duty_pos = 0;
    ch2.envelope_counter = ch2.envelope_period;
    if (ch2.length_counter == 0) ch2.length_counter = 64;
}

void APU::trigger_ch3_helper(u8 nr34_val) {
    ch3.enabled = ch3.dac_enabled;
    ch3.freq_counter = (2048 - ch3.frequency) * 2;
    ch3.sample_pos = 0;
    if (ch3.length_counter == 0) ch3.length_counter = 256;
}

void APU::trigger_ch4_helper(u8 nr44_val) {
    ch4.enabled = ch4.dac_enabled;
    ch4.freq_counter = noise_period();
    ch4.lfsr = 0x7FFF;
    ch4.lfsr_pos = 0;
    ch4.envelope_counter = ch4.envelope_period;
    if (ch4.length_counter == 0) ch4.length_counter = 64;
}

static inline void advance_square(APU::SquareChannel& ch, int n) {
    if (!ch.enabled || !ch.dac_enabled) return;
    int c = ch.freq_counter;
    if (n < c) { ch.freq_counter = c - n; return; }
    int p = (2048 - ch.frequency) * 4;
    n -= c;
    if (n < p) { ch.duty_pos = (ch.duty_pos + 1) & 7; ch.freq_counter = p - n; return; }
    int adv = 1 + n / p;
    ch.duty_pos = (ch.duty_pos + adv) & 7;
    ch.freq_counter = p - (n % p);
}

static inline void advance_wave(APU::WaveChannel& ch, int n) {
    if (!ch.enabled || !ch.dac_enabled) return;
    int c = ch.freq_counter;
    if (n < c) { ch.freq_counter = c - n; return; }
    int p = (2048 - ch.frequency) * 2;
    n -= c;
    if (n < p) { ch.sample_pos = (ch.sample_pos + 1) & 31; ch.freq_counter = p - n; return; }
    int adv = 1 + n / p;
    ch.sample_pos = (ch.sample_pos + adv) & 31;
    ch.freq_counter = p - (n % p);
}

static inline void advance_noise(APU::NoiseChannel& ch, int n) {
    if (!ch.enabled || !ch.dac_enabled) return;
    int c = ch.freq_counter;
    if (n < c) { ch.freq_counter = c - n; return; }
    int p = noise_divisors[ch.divisor_code & 7] << ch.shift_amount;
    if (p <= 0) p = 1;
    int period = g_noise_period[ch.width_mode];
    n -= c;
    int adv, rem;
    if (n < p) { adv = 1; rem = n; }
    else       { adv = 1 + n / p; rem = n % p; }
    int pos = ch.lfsr_pos + adv;
    if (pos >= period) pos -= period;
    if (pos >= period) pos %= period;
    ch.lfsr_pos = pos;
    ch.freq_counter = p - rem;
}

int APU::read_square(const SquareChannel& ch) {
    if (!ch.enabled || !ch.dac_enabled) return 0;
    u8 wave_out = duty_waves[ch.duty][ch.duty_pos];
    int dac = wave_out ? ch.volume : 0;
    return 2 * dac - 15;
}

int APU::read_wave(const WaveChannel& ch, Memory& mem) {
    if (!ch.enabled || !ch.dac_enabled) return 0;
    u16 addr = 0xFF30 + ch.sample_pos / 2;
    u8 byte = mem.io_regs[addr - 0xFF00];
    u8 nibble = (ch.sample_pos & 1) ? (byte & 0x0F) : (byte >> 4);
    int shifted;
    switch (ch.volume_code) {
        case 1: shifted = nibble; break;
        case 2: shifted = nibble >> 1; break;
        case 3: shifted = nibble >> 2; break;
        default: shifted = 0; break;
    }
    return 2 * shifted - 15;
}

int APU::read_noise(const NoiseChannel& ch) {
    if (!ch.enabled || !ch.dac_enabled) return 0;
    int dac = g_noise_out[ch.width_mode][ch.lfsr_pos] ? ch.volume : 0;
    return 2 * dac - 15;
}

void APU::tick_frame_sequencer() {
    frame_seq_step = (frame_seq_step + 1) & 7;

    if ((frame_seq_step & 1) == 0) {
        auto tick_length = [](auto& ch, int max_len) {
            if (ch.length_enable && ch.length_counter > 0) {
                ch.length_counter--;
                if (ch.length_counter == 0) ch.enabled = false;
            }
        };
        tick_length(ch1, 64);
        tick_length(ch2, 64);
        tick_length(ch3, 256);
        tick_length(ch4, 64);
    }

    if (frame_seq_step == 2 || frame_seq_step == 6) {
        if (ch1.sweep_enabled && ch1.sweep_period > 0) {
            ch1.sweep_counter--;
            if (ch1.sweep_counter <= 0) {
                ch1.sweep_counter = ch1.sweep_period ? ch1.sweep_period : 8;
                u16 delta = ch1.shadow_freq >> ch1.sweep_shift;
                u16 new_freq;
                if (ch1.sweep_dir)
                    new_freq = ch1.shadow_freq - delta;
                else
                    new_freq = ch1.shadow_freq + delta;

                if (new_freq > 2047) {
                    ch1.enabled = false;
                } else if (ch1.sweep_shift > 0) {
                    ch1.frequency = new_freq;
                    ch1.shadow_freq = new_freq;
                    u16 delta2 = new_freq >> ch1.sweep_shift;
                    u16 check = ch1.sweep_dir
                        ? new_freq - delta2
                        : new_freq + delta2;
                    if (check > 2047) ch1.enabled = false;
                }
            }
        }
    }

    if (frame_seq_step == 7) {
        auto tick_env = [](auto& ch) {
            if (ch.envelope_period > 0 && ch.dac_enabled) {
                ch.envelope_counter--;
                if (ch.envelope_counter <= 0) {
                    ch.envelope_counter = ch.envelope_period;
                    if (ch.envelope_dir) {
                        if (ch.volume < 15) ch.volume++;
                    } else {
                        if (ch.volume > 0) ch.volume--;
                    }
                }
            }
        };
        tick_env(ch1);
        tick_env(ch2);
        tick_env(ch4);
    }
}

void APU::update_nr52(Memory& mem) {
    u8 nr52 = mem.io_regs[NR52 - 0xFF00];
    if (nr52 & 0x80) {
        u8 status = 0x80;
        if (ch1.enabled && ch1.dac_enabled) status |= 1;
        if (ch2.enabled && ch2.dac_enabled) status |= 2;
        if (ch3.enabled && ch3.dac_enabled) status |= 4;
        if (ch4.enabled && ch4.dac_enabled) status |= 8;
        mem.io_regs[NR52 - 0xFF00] = status;
    }
}

void APU::generate_sample(Memory& mem) {
    if (buffer_pos >= (int)audio_buffer.size()) return;

    u8 nr50 = mem.io_regs[NR50 - 0xFF00];
    u8 nr51 = mem.io_regs[NR51 - 0xFF00];

    int c1 = read_square(ch1);
    int c2 = read_square(ch2);
    int c3 = read_wave(ch3, mem);
    int c4 = read_noise(ch4);

    int left = 0, right = 0;
    if (nr51 & 0x10) left  += c1;
    if (nr51 & 0x01) right += c1;
    if (nr51 & 0x20) left  += c2;
    if (nr51 & 0x02) right += c2;
    if (nr51 & 0x40) left  += c3;
    if (nr51 & 0x04) right += c3;
    if (nr51 & 0x80) left  += c4;
    if (nr51 & 0x08) right += c4;

    int left_vol  = ((nr50 >> 4) & 7) + 1;
    int right_vol = (nr50 & 7) + 1;

    int mono = (left * left_vol + right * right_vol) * 34;

    int t = hp_out + (mono - hp_prev);
    hp_prev = mono;
    hp_out  = t - (t >> 10);

    int out = hp_out;
    if (out >  32767) out =  32767;
    if (out < -32768) out = -32768;

    audio_buffer[buffer_pos++] = (s16)out;
}

void APU::step(int cycles, Memory& mem) {
    u8 nr52 = mem.io_regs[NR52 - 0xFF00];
    if (!(nr52 & 0x80)) return;

    frame_seq_countdown -= cycles;
    while (frame_seq_countdown <= 0) {
        frame_seq_countdown += 8192;
        tick_frame_sequencer();
        update_nr52(mem);
    }

    cycles_since_sample += cycles;
    sample_countdown    -= cycles;
    while (sample_countdown <= 0) {
        int overshoot = -sample_countdown;
        int adv = cycles_since_sample - overshoot;
        advance_square(ch1, adv);
        advance_square(ch2, adv);
        advance_wave(ch3, adv);
        advance_noise(ch4, adv);
        cycles_since_sample = overshoot;

        generate_sample(mem);

        sample_countdown += SAMPLE_BASE;
        sample_err += SAMPLE_REM;
        if (sample_err >= SAMPLE_RATE) { sample_err -= SAMPLE_RATE; sample_countdown++; }
    }
}