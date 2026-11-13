// combo/tests/ComboAnchorOutboxTest.cpp — unit tests for the co-op outbox rules
// (combo/net/ComboAnchorOutbox.h). No game, no socket, no ROM: builds as the `combo_anchor_tests`
// target and runs in CI. Deliberately framework-free (one CHECK macro) so it adds no dependency.
// Exit code 0 = all checks passed.

#include "net/ComboAnchorOutbox.h"

#include <cstdint>
#include <deque>
#include <iostream>
#include <string>

using namespace ComboAnchorCore;

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        sChecks++;                                                                          \
        if (!(cond)) {                                                                      \
            sFailures++;                                                                    \
            std::cerr << "FAILED " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n"; \
        }                                                                                   \
    } while (0)

static const int64_t kNow = 1800000000; // any fixed "now" (unix seconds)
static const std::string kRoomA = MakeScope("anchor.hm64.org", 43383, "roomA");
static const std::string kRoomB = MakeScope("anchor.hm64.org", 43383, "roomB");

static OutboxEntry Entry(uint32_t seed, const std::string& scope, const std::string& packet, int64_t heldAt = kNow) {
    OutboxEntry e;
    e.seed = seed;
    e.heldAt = heldAt;
    e.scope = scope;
    e.packet = packet;
    return e;
}

static std::string GiveItem(int getItemId, uint32_t clientId) {
    nlohmann::json j = nlohmann::json::object();
    j["type"] = "GIVE_ITEM";
    j["getItemId"] = getItemId;
    j["clientId"] = clientId;
    return j.dump();
}

static void TestPacketClassification() {
    // Everything that changes a teammate's save is held ...
    for (const char* t : { "GIVE_ITEM", "COMBO_CROSS_ITEM", "SET_FLAG", "UNSET_FLAG", "SET_CHECK_STATUS",
                           "UPDATE_DUNGEON_ITEMS", "UPDATE_BEANS_COUNT", "ENTRANCE_DISCOVERED", "GAME_COMPLETE" }) {
        CHECK(IsDurablePacketType(t));
    }
    // ... live-only traffic and full snapshots are not.
    for (const char* t : { "PLAYER_UPDATE", "PLAYER_SFX", "UPDATE_CLIENT_STATE", "UPDATE_TEAM_STATE",
                           "REQUEST_TEAM_STATE", "COMBO_PARTNER_FIND", "HANDSHAKE", "", "give_item" }) {
        CHECK(!IsDurablePacketType(t));
    }

    CHECK(PacketType("{\"type\":\"GIVE_ITEM\",\"getItemId\":5}") == "GIVE_ITEM");
    CHECK(PacketType("{\"getItemId\":5}").empty());        // no type
    CHECK(PacketType("{\"type\":7}").empty());             // type is not a string
    CHECK(PacketType("[\"type\",\"GIVE_ITEM\"]").empty()); // not an object
    CHECK(PacketType("not json at all").empty());
    CHECK(PacketType("").empty());

    CHECK(HandshakeRoomId("{\"type\":\"HANDSHAKE\",\"roomId\":\"roomA\"}") == "roomA");
    CHECK(HandshakeRoomId("{\"type\":\"HANDSHAKE\"}").empty());
    CHECK(MakeScope("host", 43383, "room") == "host:43383/room");
    CHECK(kRoomA != kRoomB);
}

static void TestPushCapDropsOldest() {
    std::deque<OutboxEntry> outbox;
    for (size_t i = 0; i < kOutboxMaxEntries; i++) {
        CHECK(!OutboxPush(outbox, Entry(1, kRoomA, std::to_string(i))));
    }
    CHECK(outbox.size() == kOutboxMaxEntries);
    CHECK(OutboxPush(outbox, Entry(1, kRoomA, "newest")));
    CHECK(outbox.size() == kOutboxMaxEntries);
    CHECK(outbox.front().packet == "1"); // "0" was the oldest
    CHECK(outbox.back().packet == "newest");
}

static void TestFlushSeedScoping() {
    std::deque<OutboxEntry> outbox;
    OutboxPush(outbox, Entry(111, kRoomA, "slotA-1"));
    OutboxPush(outbox, Entry(222, kRoomA, "slotB-1"));
    OutboxPush(outbox, Entry(0, kRoomA, "unknown-seed"));
    OutboxPush(outbox, Entry(111, kRoomA, "slotA-2"));

    // No slot loaded: only the entry that never had a seed may go. Nothing is dropped.
    {
        std::deque<OutboxEntry> copy = outbox;
        OutboxFlush f = OutboxTakeSendable(copy, 0, kRoomA, 0, kNow);
        CHECK(f.send.size() == 1 && f.send[0] == "unknown-seed");
        CHECK(f.expired == 0);
        CHECK(copy.size() == 3);
    }

    // Slot A loaded: its entries go out in held order; slot B's entry STAYS (the first outbox build
    // deleted it here, losing the finds of a second playthrough).
    OutboxFlush f = OutboxTakeSendable(outbox, 111, kRoomA, 0, kNow);
    CHECK(f.send.size() == 3);
    CHECK(f.send[0] == "slotA-1" && f.send[1] == "unknown-seed" && f.send[2] == "slotA-2");
    CHECK(outbox.size() == 1 && outbox.front().packet == "slotB-1");

    // Flushing again sends nothing twice.
    f = OutboxTakeSendable(outbox, 111, kRoomA, 0, kNow);
    CHECK(f.send.empty() && outbox.size() == 1);

    // Loading slot B later still delivers it.
    f = OutboxTakeSendable(outbox, 222, kRoomA, 0, kNow);
    CHECK(f.send.size() == 1 && f.send[0] == "slotB-1");
    CHECK(outbox.empty());
}

static void TestFlushRoomScoping() {
    std::deque<OutboxEntry> outbox;
    OutboxPush(outbox, Entry(111, kRoomA, "for-A"));
    OutboxPush(outbox, Entry(111, kRoomB, "for-B"));
    OutboxPush(outbox, Entry(111, "", "no-scope")); // e.g. a file written by the first outbox build

    // Connected to room B: A's find must not leak into it.
    OutboxFlush f = OutboxTakeSendable(outbox, 111, kRoomB, 0, kNow);
    CHECK(f.send.size() == 2 && f.send[0] == "for-B" && f.send[1] == "no-scope");
    CHECK(outbox.size() == 1 && outbox.front().packet == "for-A");

    // Back in room A it is delivered.
    f = OutboxTakeSendable(outbox, 111, kRoomA, 0, kNow);
    CHECK(f.send.size() == 1 && f.send[0] == "for-A" && outbox.empty());
}

static void TestAdoptScope() {
    std::deque<OutboxEntry> outbox;
    OutboxPush(outbox, Entry(111, "", "held-before-first-connect"));
    OutboxPush(outbox, Entry(111, kRoomB, "older-session"));
    CHECK(OutboxAdoptScope(outbox, kRoomA) == 1);
    CHECK(outbox[0].scope == kRoomA);
    CHECK(outbox[1].scope == kRoomB); // an existing scope is never rewritten
    CHECK(OutboxAdoptScope(outbox, kRoomA) == 0);
}

static void TestExpiry() {
    std::deque<OutboxEntry> outbox;
    OutboxPush(outbox, Entry(111, kRoomA, "fresh", kNow - 60));
    OutboxPush(outbox, Entry(111, kRoomA, "edge", kNow - kOutboxMaxAgeSeconds));      // exactly at the limit
    OutboxPush(outbox, Entry(111, kRoomA, "stale", kNow - kOutboxMaxAgeSeconds - 1)); // one second past it
    OutboxPush(outbox, Entry(222, kRoomA, "stale-other-slot", kNow - kOutboxMaxAgeSeconds - 1));
    OutboxPush(outbox, Entry(111, kRoomA, "clock-went-back", kNow + 3600)); // held "in the future"

    OutboxFlush f = OutboxTakeSendable(outbox, 111, kRoomA, 0, kNow);
    CHECK(f.expired == 2); // age wins over "would have been kept for another slot"
    CHECK(f.send.size() == 3 && f.send[0] == "fresh" && f.send[1] == "edge" && f.send[2] == "clock-went-back");
    CHECK(outbox.empty());
}

static void TestRestampClientId() {
    // Held with a stale/zero id: the server would echo it back to its own sender.
    const std::string held = GiveItem(42, 0);
    const std::string stamped = RestampClientId(held, 7);
    const nlohmann::json j = nlohmann::json::parse(stamped);
    CHECK(j.value("clientId", 0u) == 7u);
    CHECK(j.value("getItemId", 0) == 42); // payload untouched
    CHECK(j.value("type", std::string()) == "GIVE_ITEM");

    CHECK(RestampClientId(held, 0) == held);             // own id unknown: byte-identical
    CHECK(RestampClientId("not json", 7) == "not json"); // never throws, never mangles
    CHECK(RestampClientId("[1,2,3]", 7) == "[1,2,3]");   // not an object
    CHECK(nlohmann::json::parse(RestampClientId("{\"type\":\"SET_FLAG\"}", 9)).value("clientId", 0u) == 9u);

    // The flush applies it to everything it sends.
    std::deque<OutboxEntry> outbox;
    OutboxPush(outbox, Entry(111, kRoomA, GiveItem(1, 3)));
    OutboxPush(outbox, Entry(111, kRoomA, GiveItem(2, 0)));
    OutboxFlush f = OutboxTakeSendable(outbox, 111, kRoomA, 5, kNow);
    CHECK(f.send.size() == 2);
    for (const auto& p : f.send) {
        CHECK(nlohmann::json::parse(p).value("clientId", 0u) == 5u);
    }
}

static void TestPersistenceRoundTrip() {
    std::deque<OutboxEntry> outbox;
    OutboxPush(outbox, Entry(111, kRoomA, GiveItem(1, 3), kNow - 10));
    OutboxPush(outbox, Entry(0, kRoomA, GiveItem(2, 3), kNow - 10)); // unknown seed: memory only
    OutboxPush(outbox, Entry(222, "", GiveItem(3, 3), kNow - 20));   // no scope yet

    const nlohmann::json file = OutboxToJson(outbox);
    CHECK(file.value("version", 0) == 1);
    CHECK(file["entries"].size() == 2);
    CHECK(!file["entries"][1].contains("scope")); // empty scope is simply omitted

    // Through text, like the real file.
    std::deque<OutboxEntry> loaded;
    CHECK(OutboxFromJson(nlohmann::json::parse(file.dump()), loaded, kNow) == 2);
    CHECK(loaded.size() == 2);
    CHECK(loaded[0].seed == 111u && loaded[0].scope == kRoomA && loaded[0].heldAt == kNow - 10);
    CHECK(loaded[0].packet == GiveItem(1, 3));
    CHECK(loaded[1].seed == 222u && loaded[1].scope.empty());

    // An outbox with nothing persistable serializes to an empty entry list (the launcher removes the file).
    std::deque<OutboxEntry> memoryOnly;
    OutboxPush(memoryOnly, Entry(0, kRoomA, "x"));
    CHECK(OutboxToJson(memoryOnly)["entries"].empty());
}

static void TestLoadIsDefensive() {
    // A file written by the first outbox build: no "scope" field.
    const char* legacy = "{\"version\":1,\"entries\":[{\"seed\":111,\"heldAt\":1799999990,\"packet\":\"{}\"}]}";
    std::deque<OutboxEntry> outbox;
    CHECK(OutboxFromJson(nlohmann::json::parse(legacy), outbox, kNow) == 1);
    CHECK(outbox.size() == 1 && outbox[0].scope.empty() && outbox[0].packet == "{}");

    // Bad entries are skipped one by one; the good one in between survives.
    nlohmann::json file = nlohmann::json::object();
    file["entries"] = nlohmann::json::array();
    file["entries"].push_back(nlohmann::json::parse("{\"seed\":0,\"heldAt\":1799999990,\"packet\":\"p\"}")); // no seed
    file["entries"].push_back(nlohmann::json::parse("{\"seed\":5,\"heldAt\":1799999990,\"packet\":\"\"}"));  // empty
    file["entries"].push_back(nlohmann::json::parse("{\"seed\":5,\"heldAt\":1,\"packet\":\"p\"}"));          // ancient
    file["entries"].push_back(
        nlohmann::json::parse("{\"seed\":\"five\",\"heldAt\":1799999990,\"packet\":\"p\"}")); // type
    file["entries"].push_back(nlohmann::json::parse("\"just a string\""));
    file["entries"].push_back(nlohmann::json::parse("{\"seed\":5,\"heldAt\":1799999990,\"packet\":\"good\"}"));
    outbox.clear();
    CHECK(OutboxFromJson(file, outbox, kNow) == 1);
    CHECK(outbox.size() == 1 && outbox[0].packet == "good");

    // Wrong shapes at the top never throw and load nothing.
    outbox.clear();
    CHECK(OutboxFromJson(nlohmann::json::parse("[]"), outbox, kNow) == 0);
    CHECK(OutboxFromJson(nlohmann::json::parse("{}"), outbox, kNow) == 0);
    CHECK(OutboxFromJson(nlohmann::json::parse("{\"entries\":5}"), outbox, kNow) == 0);
    CHECK(outbox.empty());
}

int main() {
    TestPacketClassification();
    TestPushCapDropsOldest();
    TestFlushSeedScoping();
    TestFlushRoomScoping();
    TestAdoptScope();
    TestExpiry();
    TestRestampClientId();
    TestPersistenceRoundTrip();
    TestLoadIsDefensive();

    if (sFailures != 0) {
        std::cerr << sFailures << " of " << sChecks << " checks FAILED\n";
        return 1;
    }
    std::cout << "combo_anchor_tests: all " << sChecks << " checks passed\n";
    return 0;
}
