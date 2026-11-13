// combo/tests/ComboAnchorDeliveryTest.cpp — unit tests for the co-op delivery rules
// (combo/net/ComboAnchorDelivery.h): packet ids + dedup, ack markers, the team-state request gate.
// Part of the `combo_anchor_tests` target (see ComboAnchorTestsMain.cpp).

#include "net/ComboAnchorDelivery.h"

#include <cstdint>
#include <string>
#include <vector>

#include "ComboTestCheck.h"

using namespace ComboAnchorCore;

// A packet the way a game hands it to the launcher: nlohmann dump, keys sorted, no whitespace.
static std::string Packet(const char* type, int value = 0) {
    nlohmann::json j = nlohmann::json::object();
    j["type"] = type;
    j["clientId"] = 3;
    j["value"] = value;
    return j.dump();
}

static void TestPreChecks() {
    // The cheap text check must never miss a packet the outbox would hold.
    for (const char* t : { "GIVE_ITEM", "COMBO_CROSS_ITEM", "SET_FLAG", "UNSET_FLAG", "SET_CHECK_STATUS",
                           "UPDATE_DUNGEON_ITEMS", "UPDATE_BEANS_COUNT", "ENTRANCE_DISCOVERED", "GAME_COMPLETE" }) {
        CHECK(IsDurablePacketType(t));
        CHECK(MightBeDurable(Packet(t).c_str()));
    }
    CHECK(!MightBeDurable(Packet("PLAYER_UPDATE").c_str()));
    CHECK(!MightBeDurable(Packet("UPDATE_CLIENT_STATE").c_str()));
    CHECK(!MightBeDurable(Packet("REQUEST_TEAM_STATE").c_str()));

    // A type name inside a string VALUE is escaped in the raw text and must not match.
    nlohmann::json tricky = nlohmann::json::object();
    tricky["type"] = "PLAYER_UPDATE";
    tricky["name"] = "\"type\":\"GIVE_ITEM\"";
    CHECK(!MightBeDurable(tricky.dump().c_str()));

    CHECK(LooksLikeTeamStateRequest(Packet("REQUEST_TEAM_STATE").c_str()));
    CHECK(!LooksLikeTeamStateRequest(Packet("UPDATE_TEAM_STATE").c_str()));

    CHECK(LooksLikeTeamState(Packet("UPDATE_TEAM_STATE").c_str()));
    CHECK(LooksLikeTeamState("{\"type\": \"UPDATE_TEAM_STATE\",\"queue\":[]}")); // as the server builds it
    CHECK(!LooksLikeTeamState(Packet("REQUEST_TEAM_STATE").c_str()));
}

static void TestPacketIds() {
    const std::string plain = Packet("GIVE_ITEM", 42);
    CHECK(PacketId(plain).empty());
    CHECK(!HasTopLevelPacketId(plain.c_str()));

    const std::string stamped = StampPacketId(plain, "abc-1");
    CHECK(PacketId(stamped) == "abc-1");
    CHECK(HasTopLevelPacketId(stamped.c_str()));
    CHECK(PacketType(stamped) == "GIVE_ITEM");
    CHECK(nlohmann::json::parse(stamped).value("value", 0) == 42); // payload untouched

    // Stamped once: a held packet keeps its id through every re-send.
    CHECK(StampPacketId(stamped, "other-9") == stamped);
    // Survives the client-id restamp the outbox flush applies.
    CHECK(PacketId(RestampClientId(stamped, 77)) == "abc-1");

    CHECK(StampPacketId("not json", "x") == "not json");
    CHECK(StampPacketId("[1,2]", "x") == "[1,2]");
}

static void TestSeenIds() {
    SeenIds seen(3);
    CHECK(seen.Insert("a"));
    CHECK(!seen.Insert("a")); // a repeat
    CHECK(seen.Insert("b"));
    CHECK(seen.Insert("c"));
    CHECK(seen.Size() == 3);
    CHECK(seen.Insert("d")); // capacity 3: "a" falls out
    CHECK(seen.Size() == 3);
    CHECK(!seen.Contains("a"));
    CHECK(seen.Contains("b") && seen.Contains("c") && seen.Contains("d"));
    CHECK(seen.Insert("a")); // forgotten, so it counts as new again

    seen.Clear();
    CHECK(seen.Size() == 0);
    CHECK(seen.Insert("b")); // after a slot load everything may be replayed
}

// An UPDATE_TEAM_STATE as the server builds it: queue entries are the raw text of the queued packets.
static std::string TeamState(const std::vector<std::string>& queue) {
    nlohmann::json j = nlohmann::json::object();
    j["type"] = "UPDATE_TEAM_STATE";
    j["state"] = nlohmann::json::object();
    j["state"]["healthCapacity"] = 48;
    j["queue"] = nlohmann::json::array();
    for (const auto& q : queue)
        j["queue"].push_back(q);
    return j.dump();
}

static void TestTeamStateQueueFilter() {
    const std::string bow = StampPacketId(Packet("GIVE_ITEM", 1), "p-1");
    const std::string hook = StampPacketId(Packet("GIVE_ITEM", 2), "p-2");
    const std::string legacy = Packet("GIVE_ITEM", 3); // from a build without packet ids

    // Ids inside the queue are escaped in the raw text: the top-level check must not fire on them.
    CHECK(!HasTopLevelPacketId(TeamState({ bow }).c_str()));

    SeenIds seen;
    CHECK(seen.Insert("p-1")); // the bow already arrived live

    // First reply: the bow is removed, the hookshot and the id-less packet stay.
    std::string reply = TeamState({ bow, hook, legacy });
    CHECK(FilterTeamStateQueue(reply, seen) == 1);
    nlohmann::json j = nlohmann::json::parse(reply);
    CHECK(j["queue"].size() == 2);
    CHECK(j["queue"][0].get<std::string>() == hook);
    CHECK(j["queue"][1].get<std::string>() == legacy);
    CHECK(j["state"].value("healthCapacity", 0) == 48); // the state is carried through
    CHECK(seen.Contains("p-2"));

    // The server answers each request with the same queue: the second copy brings nothing new, except
    // the packet without an id, which cannot be told apart (old builds behave as before).
    std::string second = TeamState({ bow, hook, legacy });
    CHECK(FilterTeamStateQueue(second, seen) == 2);
    j = nlohmann::json::parse(second);
    CHECK(j["queue"].size() == 1 && j["queue"][0].get<std::string>() == legacy);

    // Nothing to remove: the text is left exactly as it came.
    std::string untouched = TeamState({ legacy });
    const std::string before = untouched;
    CHECK(FilterTeamStateQueue(untouched, seen) == 0);
    CHECK(untouched == before);

    // Other packets and garbage are left alone.
    std::string other = Packet("GIVE_ITEM");
    CHECK(FilterTeamStateQueue(other, seen) == 0);
    std::string garbage = "not json";
    CHECK(FilterTeamStateQueue(garbage, seen) == 0 && garbage == "not json");
    std::string noQueue = "{\"type\":\"UPDATE_TEAM_STATE\"}";
    CHECK(FilterTeamStateQueue(noQueue, seen) == 0);
}

static void TestAckMark() {
    const std::string mark = MakeAckMark(7, 3);
    const nlohmann::json j = nlohmann::json::parse(mark);
    CHECK(j.value("type", std::string()) == kAckMarkType);
    CHECK(j.value("targetClientId", 0u) == 7u); // addressed to ourselves: the relay sends it back
    CHECK(j.value("clientId", 0u) == 7u);
    CHECK(AckMarkNumber(mark) == 3u);

    CHECK(AckMarkNumber(Packet("GIVE_ITEM")) == 0u);
    CHECK(AckMarkNumber("not json") == 0u);
    CHECK(AckMarkNumber("{\"type\":\"COMBO_ACK_MARK\"}") == 0u);             // no number
    CHECK(AckMarkNumber("{\"type\":\"COMBO_ACK_MARK\",\"n\":\"3\"}") == 0u); // wrong type
    CHECK(!MightBeDurable(mark.c_str()));
}

static void TestInFlight() {
    InFlight inFlight;
    CHECK(!inFlight.HasUnmarked());

    inFlight.OnSent("a");
    inFlight.OnSent("b");
    CHECK(inFlight.HasUnmarked());
    inFlight.OnMarkerSent(1);
    CHECK(!inFlight.HasUnmarked());

    inFlight.OnSent("c");
    CHECK(inFlight.HasUnmarked());
    inFlight.OnMarkerSent(2);
    inFlight.OnSent("d"); // written, no marker behind it yet

    // Marker 1 came back: a and b reached the server. c (marker 2) and d did not prove it yet.
    inFlight.OnMarkerEchoed(1);
    CHECK(inFlight.Size() == 2);

    // The connection dies here: c and d go back to the outbox, oldest first.
    std::vector<std::string> lost = inFlight.TakeUnconfirmed();
    CHECK(lost.size() == 2 && lost[0] == "c" && lost[1] == "d");
    CHECK(inFlight.Size() == 0);

    // A later marker confirms everything before it, also what an earlier marker covered.
    inFlight.OnSent("e");
    inFlight.OnMarkerSent(1);
    inFlight.OnSent("f");
    inFlight.OnMarkerSent(2);
    inFlight.OnMarkerEchoed(2);
    CHECK(inFlight.Size() == 0);

    // A packet without a marker behind it is never confirmed by an older echo.
    inFlight.OnSent("g");
    inFlight.OnMarkerEchoed(99);
    CHECK(inFlight.Size() == 1);
    CHECK(inFlight.TakeUnconfirmed().size() == 1);

    // Bounded: the oldest entry is given up first.
    InFlight small(2);
    small.OnSent("1");
    small.OnSent("2");
    small.OnSent("3");
    lost = small.TakeUnconfirmed();
    CHECK(lost.size() == 2 && lost[0] == "2" && lost[1] == "3");
}

static void TestTeamStateRequestGate() {
    const int64_t t0 = 1000000;
    TeamStateRequestGate gate;

    // Connect: OOT and MM both ask in the same frame. Only one may be on the wire, or the server
    // delivers the first answer twice and the second one to nobody.
    CHECK(gate.OnRequest("oot", "REQ-OOT", t0));
    CHECK(!gate.OnRequest("mm", "REQ-MM", t0));
    CHECK(gate.ParkedCount() == 1);

    // The same game asking again while its request is out adds nothing.
    CHECK(!gate.OnRequest("oot", "REQ-OOT-AGAIN", t0 + 10));
    CHECK(gate.ParkedCount() == 1);
    // A newer request of the parked game replaces the parked one.
    CHECK(!gate.OnRequest("mm", "REQ-MM-NEWER", t0 + 20));
    CHECK(gate.ParkedCount() == 1);

    // Nothing to hand out while waiting.
    CHECK(gate.OnTick(t0 + 100).empty());

    // OOT's answer arrives: MM's request goes out now.
    CHECK(gate.OnReply(t0 + 200) == "REQ-MM-NEWER");
    CHECK(gate.ParkedCount() == 0);
    // MM's answer arrives: nothing left, and the gate is open again.
    CHECK(gate.OnReply(t0 + 300).empty());
    CHECK(gate.OnRequest("oot", "REQ-OOT-2", t0 + 400));

    // No answer at all (teammate's game cannot answer): the parked request is released by the tick
    // once the wait is over, not before.
    CHECK(!gate.OnRequest("mm", "REQ-MM-2", t0 + 500));
    CHECK(gate.OnTick(t0 + 400 + TeamStateRequestGate::kTimeoutMs - 1).empty());
    CHECK(gate.OnTick(t0 + 400 + TeamStateRequestGate::kTimeoutMs) == "REQ-MM-2");
    // ... and that one is on the wire now.
    CHECK(!gate.OnRequest("mm", "REQ-MM-3", t0 + 400 + TeamStateRequestGate::kTimeoutMs + 1));

    // After a timeout with nothing parked a new request goes straight out.
    TeamStateRequestGate stale;
    CHECK(stale.OnRequest("oot", "A", t0));
    CHECK(stale.OnRequest("oot", "B", t0 + TeamStateRequestGate::kTimeoutMs));

    // A new connection forgets everything.
    CHECK(!gate.OnRequest("oot", "PARKED", t0 + 400 + TeamStateRequestGate::kTimeoutMs + 2));
    gate.Reset();
    CHECK(gate.ParkedCount() == 0);
    CHECK(gate.OnTick(t0 + 999999).empty());
    CHECK(gate.OnRequest("mm", "FRESH", t0 + 999999));
}

void RunDeliveryTests() {
    TestPreChecks();
    TestPacketIds();
    TestSeenIds();
    TestTeamStateQueueFilter();
    TestAckMark();
    TestInFlight();
    TestTeamStateRequestGate();
}
