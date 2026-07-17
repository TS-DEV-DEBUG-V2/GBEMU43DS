#include "unzip.h"
#include "gb_log.h"
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace {

struct BitReader {
    const u8* src;
    size_t len;
    size_t pos;
    int bitbuf;
    int bitcnt;
    bool error;

    int getbit() {
        if (bitcnt == 0) {
            if (pos >= len) { error = true; return 0; }
            bitbuf = src[pos++];
            bitcnt = 8;
        }
        int b = bitbuf & 1;
        bitbuf >>= 1;
        bitcnt--;
        return b;
    }
    int getbits(int n) {
        int v = 0;
        for (int i = 0; i < n; i++) v |= getbit() << i;
        return v;
    }
};

struct Huffman {
    short count[16];
    short symbol[288];
};

void build(Huffman& h, const u8* lengths, int n) {
    for (int i = 0; i < 16; i++) h.count[i] = 0;
    for (int i = 0; i < n; i++) h.count[lengths[i]]++;
    h.count[0] = 0;
    short offs[16];
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h.count[i];
    for (int i = 0; i < n; i++)
        if (lengths[i]) h.symbol[offs[lengths[i]]++] = (short)i;
}

int decode(BitReader& br, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= br.getbit();
        int cnt = h.count[len];
        if (code - first < cnt) return h.symbol[index + (code - first)];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
        if (br.error) return -1;
    }
    return -1;
}

const short LEN_BASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
const u8    LEN_EXTRA[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
const short DIST_BASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
const u8    DIST_EXTRA[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

bool inflate_block(BitReader& br, std::vector<u8>& out, const Huffman& lh, const Huffman& dh) {
    for (;;) {
        int sym = decode(br, lh);
        if (sym < 0) return false;
        if (sym == 256) return true;
        if (sym < 256) { out.push_back((u8)sym); continue; }
        sym -= 257;
        if (sym >= 29) return false;
        int length = LEN_BASE[sym] + br.getbits(LEN_EXTRA[sym]);
        int dsym = decode(br, dh);
        if (dsym < 0 || dsym >= 30) return false;
        int dist = DIST_BASE[dsym] + br.getbits(DIST_EXTRA[dsym]);
        if ((size_t)dist > out.size()) return false;
        size_t from = out.size() - dist;
        for (int i = 0; i < length; i++) out.push_back(out[from + i]);
        if (br.error) return false;
    }
}

bool inflate(const u8* data, size_t len, std::vector<u8>& out) {
    BitReader br{ data, len, 0, 0, 0, false };
    Huffman fixed_l, fixed_d;
    bool fixed_built = false;
    for (;;) {
        int final = br.getbit();
        int type = br.getbits(2);
        if (br.error) return false;
        if (type == 0) {
            br.bitcnt = 0;
            if (br.pos + 4 > br.len) return false;
            int l = br.src[br.pos] | (br.src[br.pos + 1] << 8);
            br.pos += 4;
            if (br.pos + l > br.len) return false;
            out.insert(out.end(), br.src + br.pos, br.src + br.pos + l);
            br.pos += l;
        } else if (type == 1) {
            if (!fixed_built) {
                u8 ll[288];
                for (int i = 0; i < 144; i++) ll[i] = 8;
                for (int i = 144; i < 256; i++) ll[i] = 9;
                for (int i = 256; i < 280; i++) ll[i] = 7;
                for (int i = 280; i < 288; i++) ll[i] = 8;
                build(fixed_l, ll, 288);
                u8 dl[30];
                for (int i = 0; i < 30; i++) dl[i] = 5;
                build(fixed_d, dl, 30);
                fixed_built = true;
            }
            if (!inflate_block(br, out, fixed_l, fixed_d)) return false;
        } else if (type == 2) {
            int hlit = br.getbits(5) + 257;
            int hdist = br.getbits(5) + 1;
            int hclen = br.getbits(4) + 4;
            static const u8 order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            u8 cl[19] = { 0 };
            for (int i = 0; i < hclen; i++) cl[order[i]] = (u8)br.getbits(3);
            Huffman clh;
            build(clh, cl, 19);
            u8 lengths[288 + 32] = { 0 };
            int i = 0;
            while (i < hlit + hdist) {
                int s = decode(br, clh);
                if (s < 0) return false;
                if (s < 16) lengths[i++] = (u8)s;
                else if (s == 16) { if (i == 0) return false; int r = br.getbits(2) + 3; u8 p = lengths[i - 1]; while (r-- && i < hlit + hdist) lengths[i++] = p; }
                else if (s == 17) { int r = br.getbits(3) + 3;  while (r-- && i < hlit + hdist) lengths[i++] = 0; }
                else               { int r = br.getbits(7) + 11; while (r-- && i < hlit + hdist) lengths[i++] = 0; }
                if (br.error) return false;
            }
            Huffman lh, dh;
            build(lh, lengths, hlit);
            build(dh, lengths + hlit, hdist);
            if (!inflate_block(br, out, lh, dh)) return false;
        } else {
            return false;
        }
        if (final) break;
        if (br.error) return false;
    }
    return true;
}

u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
u16 rd16(const u8* p) { return p[0] | (p[1] << 8); }

bool name_is_rom(const char* name, u16 name_len) {
    if (name_len < 3) return false;
    const char* ext3 = name + name_len - 3;
    const char* ext4 = name + name_len - 4;
    if (strncasecmp(ext3, ".gb", 3) == 0) return true;
    if (name_len >= 4 && strncasecmp(ext4, ".gbc", 4) == 0) return true;
    return false;
}

}

bool path_is_zip(const std::string& path) {
    size_t n = path.size();
    return n > 4 && strcasecmp(path.c_str() + n - 4, ".zip") == 0;
}

bool unzip_extract_rom(const std::string& zip_path, std::vector<u8>& out) {
    FILE* f = fopen(zip_path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    std::vector<u8> zip((size_t)sz);
    if (fread(zip.data(), 1, zip.size(), f) != zip.size()) { fclose(f); return false; }
    fclose(f);

    size_t p = 0;
    while (p + 30 <= zip.size()) {
        const u8* h = &zip[p];
        if (rd32(h) != 0x04034b50) break;
        u16 method   = rd16(h + 8);
        u32 comp_sz  = rd32(h + 18);
        u32 uncomp_sz= rd32(h + 22);
        u16 name_len = rd16(h + 26);
        u16 extra_len= rd16(h + 28);
        u16 flags    = rd16(h + 6);
        size_t name_off = p + 30;
        size_t data_off = name_off + name_len + extra_len;
        if (data_off + comp_sz > zip.size()) break;

        const char* name = (const char*)&zip[name_off];
        bool is_rom = name_is_rom(name, name_len);

        if (is_rom && !(flags & 0x08) && comp_sz > 0) {
            const u8* data = &zip[data_off];
            if (method == 0) {
                out.assign(data, data + comp_sz);
                LOGI(LOG_CART, "unzip: '%.*s' (stored, %u bytes)", name_len, name, comp_sz);
                return true;
            } else if (method == 8) {
                out.clear();
                if (uncomp_sz) out.reserve(uncomp_sz);
                if (inflate(data, comp_sz, out)) {
                    LOGI(LOG_CART, "unzip: '%.*s' (deflate -> %zu bytes)", name_len, name, out.size());
                    return true;
                }
                LOGW(LOG_CART, "unzip: inflate failed for '%.*s'", name_len, name);
                out.clear();
            }
        }
        p = data_off + comp_sz;
    }
    LOGW(LOG_CART, "unzip: no .gb/.gbc entry found in %s", zip_path.c_str());
    return false;
}
