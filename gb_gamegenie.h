#pragma once
#include "gb_types.h"

struct Cartridge;

struct GGPatch { u32 offset; u8 orig; };

bool gg_decode(const char* code, u16* addr, u8* val, u8* cmp, bool* has_cmp);

int  gg_apply(Cartridge& cart, const char* code, GGPatch* out, int maxpatches);

void gg_unapply(Cartridge& cart, const GGPatch* patches, int n);
