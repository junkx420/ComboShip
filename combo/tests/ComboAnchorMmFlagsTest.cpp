// combo/tests/ComboAnchorMmFlagsTest.cpp — unit tests for the game-independent part of MM flag sync
// (combo/net/ComboAnchorMmFlags.h). Part of the `combo_anchor_tests` target.

#include "net/ComboAnchorMmFlags.h"
#include "net/ComboAnchorDelivery.h"

#include <cstdint>
#include <string>

#include "ComboTestCheck.h"

using namespace ComboAnchorCore;

static void TestWeekEventPersistMask() {
    // Two persistence bits per flag bit; either one set keeps the flag across a cycle reset.
    CHECK(WeekEventPersistMask(0x0000) == 0x00);
    CHECK(WeekEventPersistMask(0xFFFF) == 0xFF);
    CHECK(WeekEventPersistMask(0x0003) == 0x01); // PERSISTENT_WEEKEVENTREG(bit 0)
    CHECK(WeekEventPersistMask(0x0002) == 0x01); // PERSISTENT_WEEKEVENTREG_ALT(bit 0)
    CHECK(WeekEventPersistMask(0x0001) == 0x01);
    CHECK(WeekEventPersistMask(0x000C) == 0x02); // bit 1
    CHECK(WeekEventPersistMask(0xC000) == 0x80); // bit 7
    CHECK(WeekEventPersistMask(0x8000) == 0x80);
    CHECK(WeekEventPersistMask(0x0330) == 0x14); // bits 2 and 4
}

static void TestSceneFlagBits() {
    uint32_t bit = 0;
    int word = -1;

    CHECK(ChestFlagBit(0, bit) && bit == 0x00000001u);
    CHECK(ChestFlagBit(31, bit) && bit == 0x80000000u);
    CHECK(!ChestFlagBit(32, bit));
    CHECK(!ChestFlagBit(-1, bit));

    CHECK(SwitchFlagBit(0, word, bit) && word == 0 && bit == 0x00000001u);
    CHECK(SwitchFlagBit(26, word, bit) && word == 0 && bit == (1u << 26)); // Stone Tower's persistent switch
    CHECK(SwitchFlagBit(31, word, bit) && word == 0 && bit == 0x80000000u);
    CHECK(SwitchFlagBit(32, word, bit) && word == 1 && bit == 0x00000001u);
    CHECK(SwitchFlagBit(63, word, bit) && word == 1 && bit == 0x80000000u);
    CHECK(!SwitchFlagBit(64, word, bit)); // temporary switches are never saved
    CHECK(!SwitchFlagBit(-1, word, bit));

    CHECK(!CollectibleFlagBit(0, bit)); // the game ignores collectible flag 0
    CHECK(CollectibleFlagBit(1, bit) && bit == 0x00000002u);
    CHECK(CollectibleFlagBit(31, bit) && bit == 0x80000000u);
    CHECK(!CollectibleFlagBit(32, bit)); // only the first word is saved
}

static void TestPacketIsHeld() {
    // The flag packet must be treated like the other save-affecting packets by the launcher: held
    // while offline, stamped with an id, acknowledged.
    CHECK(IsDurablePacketType(kMmFlagPacketType));
    nlohmann::json j = nlohmann::json::object();
    j["type"] = kMmFlagPacketType;
    j["kind"] = kMmFlagChest;
    j["scene"] = 22;
    j["flag"] = 5;
    CHECK(MightBeDurable(j.dump().c_str()));
    // It must not be mistaken for OOT's SET_FLAG.
    CHECK(std::string(kMmFlagPacketType) != "SET_FLAG");
}

void RunMmFlagTests() {
    TestWeekEventPersistMask();
    TestSceneFlagBits();
    TestPacketIsHeld();
}
