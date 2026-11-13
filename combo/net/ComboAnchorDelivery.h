// combo/net/ComboAnchorDelivery.h — ComboShip co-op: delivery rules on top of the outbox, without the
// socket. Four small pieces the launcher (combo/ComboShip.cpp, namespace ComboAnchor) wires together:
//
//  1. Packet ids. Every save-affecting packet gets a unique "comboPid" when it leaves the game. A
//     receiver remembers the ids it has passed on (SeenIds) and drops a second copy, whether it arrives
//     as a live packet or inside the "queue" of an UPDATE_TEAM_STATE. That is what makes re-sending
//     safe at all: OOT's GIVE_ITEM carries no check and is not idempotent on its own.
//  2. Server acknowledgement without a server change (InFlight). The relay delivers a packet with
//     "targetClientId" to exactly that client — also when it is the sender. So after writing
//     save-affecting packets the launcher writes a tiny marker addressed to itself; TCP keeps the
//     order, so when marker N comes back, everything written before it has reached the server.
//     Whatever is still unconfirmed when the connection dies goes back into the outbox.
//  3. One team-state request at a time (TeamStateRequestGate). The server keeps ONE list of clients
//     waiting for a team state and empties it on the FIRST UPDATE_TEAM_STATE it sees. OOT and MM each
//     ask; asked back to back, the first answer was delivered twice and the other game's answer to
//     nobody.
//  4. Cheap text pre-checks, so per-frame pose packets never pay for a JSON parse.
//
// Old fork builds and plain SoH Anchor clients ignore the extra field and never see the marker (it is
// addressed to its own sender only). Header-only; standard library + nlohmann/json.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

#include "ComboAnchorOutbox.h"

namespace ComboAnchorCore {

constexpr const char* kPacketIdField = "comboPid";
constexpr const char* kAckMarkType = "COMBO_ACK_MARK";

// ---- text pre-checks (no parse). nlohmann::json::dump() writes no whitespace, and a quote inside a
// string value is escaped, so `"key":"VALUE"` in the raw text can only be a real key/value pair. ----

// Can this be one of the IsDurablePacketType packets? False positives are fine (the caller parses to
// be sure), false negatives are not: keep the list in step with IsDurablePacketType (unit-tested).
inline bool MightBeDurable(const char* text) {
    static const char* const kNeedles[] = {
        "\"type\":\"GIVE_ITEM\"",          "\"type\":\"COMBO_CROSS_ITEM\"",    "\"type\":\"SET_FLAG\"",
        "\"type\":\"UNSET_FLAG\"",         "\"type\":\"SET_CHECK_STATUS\"",    "\"type\":\"UPDATE_DUNGEON_ITEMS\"",
        "\"type\":\"UPDATE_BEANS_COUNT\"", "\"type\":\"ENTRANCE_DISCOVERED\"", "\"type\":\"GAME_COMPLETE\"",
    };
    for (const char* needle : kNeedles) {
        if (std::strstr(text, needle) != nullptr)
            return true;
    }
    return false;
}

// A packet id at the top level. Ids inside a team-state queue are escaped (\"comboPid\") and do not match.
inline bool HasTopLevelPacketId(const char* text) {
    return std::strstr(text, "\"comboPid\":\"") != nullptr;
}

// UPDATE_TEAM_STATE, as a client dumps it or as the server builds its own reply (`{"type": "UPDATE_...`).
inline bool LooksLikeTeamState(const char* text) {
    return std::strstr(text, "\"type\":\"UPDATE_TEAM_STATE\"") != nullptr ||
           std::strstr(text, "\"type\": \"UPDATE_TEAM_STATE\"") != nullptr;
}

inline bool LooksLikeTeamStateRequest(const char* text) {
    return std::strstr(text, "\"type\":\"REQUEST_TEAM_STATE\"") != nullptr;
}

// ---- 1. packet ids ----

// Add the id to a JSON object packet. A packet that already has one keeps it (a held packet is
// stamped once and re-sent under the same id); anything unparsable is returned unchanged.
inline std::string StampPacketId(const std::string& packet, const std::string& id) {
    try {
        nlohmann::json j = nlohmann::json::parse(packet);
        if (!j.is_object() || j.contains(kPacketIdField))
            return packet;
        j[kPacketIdField] = id;
        return j.dump();
    } catch (...) { return packet; }
}

inline std::string PacketId(const std::string& packet) {
    return PacketStringField(packet, kPacketIdField);
}

// Bounded "have I passed this id on already?" memory. Oldest ids fall out first.
class SeenIds {
  public:
    explicit SeenIds(size_t capacity = 8192) : mCapacity(capacity) {
    }

    // True the first time an id is seen, false for a repeat.
    bool Insert(const std::string& id) {
        if (!mIds.insert(id).second)
            return false;
        mOrder.push_back(id);
        if (mOrder.size() > mCapacity) {
            mIds.erase(mOrder.front());
            mOrder.pop_front();
        }
        return true;
    }

    bool Contains(const std::string& id) const {
        return mIds.count(id) != 0;
    }

    void Clear() {
        mIds.clear();
        mOrder.clear();
    }

    size_t Size() const {
        return mIds.size();
    }

  private:
    size_t mCapacity;
    std::unordered_set<std::string> mIds;
    std::deque<std::string> mOrder;
};

// An UPDATE_TEAM_STATE can carry a "queue": the raw text of packets the server collected for the team.
// The games replay those themselves, past the launcher's live dedup, and the server answers EACH
// request with the same queue — so two requests replayed every queued packet twice. Remove the entries
// whose id was already passed on and remember the others. Entries without an id (old builds) are kept.
// Rewrites `packet` only if something was removed; returns how many entries were removed.
inline size_t FilterTeamStateQueue(std::string& packet, SeenIds& seen) {
    try {
        nlohmann::json j = nlohmann::json::parse(packet);
        if (!j.is_object())
            return 0;
        const auto type = j.find("type");
        if (type == j.end() || !type->is_string() || type->get<std::string>() != "UPDATE_TEAM_STATE")
            return 0;
        const auto queue = j.find("queue");
        if (queue == j.end() || !queue->is_array())
            return 0;
        nlohmann::json kept = nlohmann::json::array();
        size_t removed = 0;
        for (const auto& item : *queue) {
            if (item.is_string()) {
                const std::string id = PacketId(item.get<std::string>());
                if (!id.empty() && !seen.Insert(id)) {
                    removed++;
                    continue;
                }
            }
            kept.push_back(item);
        }
        if (removed == 0)
            return 0;
        j["queue"] = std::move(kept);
        packet = j.dump();
        return removed;
    } catch (...) { return 0; }
}

// ---- 2. server acknowledgement by loop-back marker ----

inline std::string MakeAckMark(uint32_t ownClientId, uint32_t number) {
    nlohmann::json j = nlohmann::json::object();
    j["type"] = kAckMarkType;
    j["targetClientId"] = ownClientId; // the relay delivers it to this client only: back to us
    j["clientId"] = ownClientId;
    j["n"] = number;
    j["quiet"] = true;
    return j.dump();
}

// The marker number of an echoed ack mark, or 0 if the packet is not one.
inline uint32_t AckMarkNumber(const std::string& packet) {
    try {
        const nlohmann::json j = nlohmann::json::parse(packet);
        if (!j.is_object())
            return 0;
        const auto type = j.find("type");
        if (type == j.end() || !type->is_string() || type->get<std::string>() != kAckMarkType)
            return 0;
        const auto n = j.find("n");
        if (n == j.end() || !n->is_number_unsigned())
            return 0;
        return n->get<uint32_t>();
    } catch (...) { return 0; }
}

// Save-affecting packets written to the socket but not yet confirmed by an echoed marker. Markers are
// numbered 1, 2, 3 ... per connection; one tracker per connection.
class InFlight {
  public:
    explicit InFlight(size_t capacity = 512) : mCapacity(capacity) {
    }

    // A save-affecting packet was written to the socket. Beyond the capacity the oldest entry is
    // assumed delivered (a connection that long without a single echo has no working marker anyway).
    void OnSent(const std::string& packet) {
        mEntries.push_back({ 0, packet });
        if (mEntries.size() > mCapacity)
            mEntries.pop_front();
    }

    bool HasUnmarked() const {
        return !mEntries.empty() && mEntries.back().marker == 0;
    }

    // Marker `number` was written right after everything sent so far.
    void OnMarkerSent(uint32_t number) {
        for (auto it = mEntries.rbegin(); it != mEntries.rend() && it->marker == 0; ++it)
            it->marker = number;
    }

    // Marker `number` came back: everything written before it has reached the server.
    void OnMarkerEchoed(uint32_t number) {
        while (!mEntries.empty() && mEntries.front().marker != 0 && mEntries.front().marker <= number)
            mEntries.pop_front();
    }

    // The connection ended: hand back what was never confirmed, oldest first, and start over.
    std::vector<std::string> TakeUnconfirmed() {
        std::vector<std::string> out;
        out.reserve(mEntries.size());
        for (auto& e : mEntries)
            out.push_back(std::move(e.packet));
        mEntries.clear();
        return out;
    }

    size_t Size() const {
        return mEntries.size();
    }

  private:
    struct Entry {
        uint32_t marker; // 0 = no marker written behind it yet
        std::string packet;
    };
    size_t mCapacity;
    std::deque<Entry> mEntries;
};

// ---- 3. one team-state request at a time ----

class TeamStateRequestGate {
  public:
    // No answer within this long (teammate's game cannot answer right now): stop waiting.
    static constexpr int64_t kTimeoutMs = 3000;

    // A game wants to send a REQUEST_TEAM_STATE. `origin` is its originGame ("oot"/"mm").
    // True = send it now. False = not now: either the same game's request is already on the wire (this
    // one is redundant and dropped), or another game's is (this one is parked, newest per game, and
    // handed out by OnReply/OnTick).
    bool OnRequest(const std::string& origin, const std::string& packet, int64_t nowMs) {
        if (Waiting(nowMs)) {
            if (origin == mWaitingFor)
                return false;
            for (auto& parked : mParked) {
                if (parked.first == origin) {
                    parked.second = packet;
                    return false;
                }
            }
            mParked.emplace_back(origin, packet);
            return false;
        }
        Start(origin, nowMs);
        return true;
    }

    // An UPDATE_TEAM_STATE arrived. Returns the next parked request to send, or "".
    std::string OnReply(int64_t nowMs) {
        mWaiting = false;
        return Next(nowMs);
    }

    // Periodic. Returns a parked request to send once the wait is over (answered or timed out), or "".
    std::string OnTick(int64_t nowMs) {
        if (Waiting(nowMs))
            return std::string();
        mWaiting = false;
        return Next(nowMs);
    }

    // New connection: nothing is on the wire, nothing parked survives.
    void Reset() {
        mWaiting = false;
        mWaitingFor.clear();
        mParked.clear();
    }

    size_t ParkedCount() const {
        return mParked.size();
    }

  private:
    bool Waiting(int64_t nowMs) const {
        return mWaiting && nowMs - mSentAtMs < kTimeoutMs;
    }

    void Start(const std::string& origin, int64_t nowMs) {
        mWaiting = true;
        mWaitingFor = origin;
        mSentAtMs = nowMs;
    }

    std::string Next(int64_t nowMs) {
        if (mParked.empty())
            return std::string();
        std::pair<std::string, std::string> next = std::move(mParked.front());
        mParked.erase(mParked.begin());
        Start(next.first, nowMs);
        return next.second;
    }

    bool mWaiting = false;
    std::string mWaitingFor;
    int64_t mSentAtMs = 0;
    std::vector<std::pair<std::string, std::string>> mParked; // (origin, packet), oldest first
};

} // namespace ComboAnchorCore
