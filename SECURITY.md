# Security

## Supported Versions

| Version | Supported |
| ------- | --------- |
| main    | yes       |
| < main  | no        |

Only the latest commit on main gets security fixes. No stable release branches right now, this is active dev.

## Scope

The emulator parses untrusted input in a few places, and all of it is in scope:

- ROM files (.gb, .gbc)
- Savestates
- Battery backed save files (.sav)
- Config or cheat code files loaded from disk

Not in scope:

- A malicious ROM that just crashes the emulator (panic/abort) with no further impact
- Behavior that's just accurately emulating real Game Boy quirks

## Threat Model

A malformed ROM or save file shouldn't be able to:

- Cause memory corruption (OOB read/write, use after free, buffer overflow) in the core
- Escape emulated memory and touch host process memory
- Cause unbounded resource use (infinite loops in the loader, huge allocations from a header field)
- Get code execution on PC or on 3DS (this one's critical given the homebrew/CFW context)

Areas that need the most scrutiny:

- Cartridge header parsing (ROM/RAM size fields, MBC detection). Bad headers shouldn't drive unchecked allocation or indexing.
- MBC bank switching. Bank indices from ROM data need to be masked/bounds checked before touching arrays.
- Savestate loading. A savestate is basically a raw dump of emulator state, loading a corrupted one shouldn't write past fixed size buffers.
- APU/core threading on Old 3DS (2 cores only). Shared state between audio and CPU/PPU threads needs to be race free, not just "doesn't crash most of the time."
- Any unsafe/FFI boundary between the Rust core and the platform layer (libctru bindings, C interop). These get first priority in review.

# Report anything by DMing me (@luvescodebtw) 
