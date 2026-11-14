// combo/net/ComboAnchorMmFlags.h — ComboShip co-op: the game-independent part of MM flag sync.
//
// Majora's Mask resets most of its world state with every Song of Time. Each player runs their own
// three-day cycle, so only what the game itself keeps ACROSS a reset is shared:
//   - scene flags (chest / switch / collectible) inside the scene's "persistent cycle" mask,
//   - weekEventReg bits marked persistent (this includes the Bombers' Notebook entries),
//   - owl statue activations.
// Everything else (cleared-room flags, non-persistent scene and weekEventReg bits, eventInf) stays
// local. Flags are only ever SET by the sync, never cleared: like the team-state resync, it can only
// add, which also makes every replay harmless.
//
// This header holds the packet vocabulary and the bit arithmetic, so both can be unit-tested without
// the game (combo/tests). The game side is mm/2s2h/Network/Anchor/MMAnchor.cpp.
#pragma once

#include <cstdint>

namespace ComboAnchorCore {

// Own packet type instead of SET_FLAG: packets replayed from the server's team queue reach BOTH games
// past their origin filters, and OOT's SET_FLAG handler must never see an MM flag.
constexpr const char* kMmFlagPacketType = "COMBO_MM_FLAG";

// "kind" values of a COMBO_MM_FLAG packet.
constexpr const char* kMmFlagChest = "chest";               // scene + flag 0..31
constexpr const char* kMmFlagSwitch = "switch";             // scene + flag 0..63
constexpr const char* kMmFlagCollectible = "collectible";   // scene + flag 1..31
constexpr const char* kMmFlagWeekEventReg = "weekEventReg"; // flag = (index << 8) | bit mask
constexpr const char* kMmFlagOwl = "owl";                   // flag = owl statue id

// The game stores two persistence bits per weekEventReg flag bit; either one set means "survives the
// cycle reset" (Sram_SaveEndOfCycle tests `bits & 3`). Returns the persistent bits of one byte.
inline uint8_t WeekEventPersistMask(uint16_t persistBits) {
    uint8_t mask = 0;
    for (int bit = 0; bit < 8; bit++) {
        if (((persistBits >> (2 * bit)) & 3) != 0)
            mask = static_cast<uint8_t>(mask | (1u << bit));
    }
    return mask;
}

// Scene flag ids -> the bit in the SAVED flag words. The game has more flags than it saves (temporary
// switches 64..127, collectibles 32..127 and collectible 0): those return false.
inline bool ChestFlagBit(int flag, uint32_t& bit) {
    if (flag < 0 || flag > 31)
        return false;
    bit = 1u << flag;
    return true;
}

// word 0 = switch0 (flags 0..31), word 1 = switch1 (flags 32..63).
inline bool SwitchFlagBit(int flag, int& word, uint32_t& bit) {
    if (flag < 0 || flag > 63)
        return false;
    word = flag >> 5;
    bit = 1u << (flag & 31);
    return true;
}

inline bool CollectibleFlagBit(int flag, uint32_t& bit) {
    if (flag < 1 || flag > 31)
        return false;
    bit = 1u << flag;
    return true;
}

} // namespace ComboAnchorCore
