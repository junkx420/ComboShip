// combo/net/ComboAnchorOutbox.h — ComboShip co-op: the offline outbox rules, without the socket.
//
// The launcher (combo/ComboShip.cpp, namespace ComboAnchor) owns the Anchor connection. Save-affecting
// packets it cannot deliver right now are held in an "outbox" and sent later. Everything that decides
// WHAT is held, WHEN it may go out and HOW it is stored lives here as plain functions over plain data:
// no globals, no locks, no clock, no file access. The launcher adds those; combo/tests/ runs these
// functions in CI without a game, a server or a ROM.
//
// Header-only, depends on the standard library and nlohmann/json only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace ComboAnchorCore {

// A held packet older than this is not worth sending any more (and is dropped on load and on flush).
constexpr int64_t kOutboxMaxAgeSeconds = 48 * 60 * 60;
// A runaway flag loop while offline must not grow the outbox without bound; the oldest entry goes first.
constexpr size_t kOutboxMaxEntries = 4096;

struct OutboxEntry {
    uint32_t seed = 0;  // masterSeed of the slot loaded when it was held; 0 = unknown (memory only)
    int64_t heldAt = 0; // unix seconds
    // Where the packet was meant to go: "host:port/roomId" (MakeScope). Empty = held before this
    // session ever reached a room (or written by a build that had no scope); see OutboxAdoptScope.
    std::string scope;
    std::string packet; // the exact JSON the game handed to the launcher
};

// Packets worth holding across a disconnect: everything that changes a teammate's save. Their receive
// paths are idempotent or progressive (see docs/deviations/anchor.md), and none of them is
// time-sensitive. UPDATE_TEAM_STATE is not held: it is a full snapshot that the next save re-sends.
inline bool IsDurablePacketType(const std::string& t) {
    return t == "GIVE_ITEM" || t == "COMBO_CROSS_ITEM" || t == "SET_FLAG" || t == "UNSET_FLAG" ||
           t == "SET_CHECK_STATUS" || t == "UPDATE_DUNGEON_ITEMS" || t == "UPDATE_BEANS_COUNT" ||
           t == "ENTRANCE_DISCOVERED" || t == "GAME_COMPLETE";
}

// Value of a top-level string field of a JSON object packet, or "" for anything else (not JSON, not
// an object, field missing, field not a string). Never throws.
inline std::string PacketStringField(const std::string& packet, const char* key) {
    try {
        const nlohmann::json j = nlohmann::json::parse(packet);
        if (!j.is_object())
            return std::string();
        const auto it = j.find(key);
        if (it == j.end() || !it->is_string())
            return std::string();
        return it->get<std::string>();
    } catch (...) { return std::string(); }
}

inline std::string PacketType(const std::string& packet) {
    return PacketStringField(packet, "type");
}

// The room a HANDSHAKE packet asks for.
inline std::string HandshakeRoomId(const std::string& packet) {
    return PacketStringField(packet, "roomId");
}

inline std::string MakeScope(const std::string& host, uint16_t port, const std::string& roomId) {
    return host + ":" + std::to_string(port) + "/" + roomId;
}

inline bool OutboxEntryExpired(const OutboxEntry& e, int64_t now) {
    return now - e.heldAt > kOutboxMaxAgeSeconds;
}

// Append, dropping the oldest entry when full. Returns true if one was dropped.
inline bool OutboxPush(std::deque<OutboxEntry>& outbox, OutboxEntry entry) {
    bool droppedOldest = false;
    if (outbox.size() >= kOutboxMaxEntries) {
        outbox.pop_front();
        droppedOldest = true;
    }
    outbox.push_back(std::move(entry));
    return droppedOldest;
}

// The first HANDSHAKE of a connection names the room. Entries without a scope were held on the way
// there (finds before the first connection of a session), so they belong to it. Returns how many.
inline size_t OutboxAdoptScope(std::deque<OutboxEntry>& outbox, const std::string& scope) {
    size_t adopted = 0;
    for (auto& e : outbox) {
        if (e.scope.empty()) {
            e.scope = scope;
            adopted++;
        }
    }
    return adopted;
}

// A held packet carries the client id the game had when it was made. The server tells sender and
// receivers apart by exactly that field (it skips the client whose id equals the packet's clientId),
// so a packet with a stale or zero id is echoed back to its own sender — and OOT's GIVE_ITEM would
// grant the item a second time. Rewrite it to the id this connection really has. clientId 0 (not
// known) or an unparsable packet leaves the text untouched.
inline std::string RestampClientId(const std::string& packet, uint32_t clientId) {
    if (clientId == 0)
        return packet;
    try {
        nlohmann::json j = nlohmann::json::parse(packet);
        if (!j.is_object())
            return packet;
        j["clientId"] = clientId;
        return j.dump();
    } catch (...) { return packet; }
}

struct OutboxFlush {
    std::vector<std::string> send; // in held order, client id already restamped
    size_t expired = 0;            // dropped for age
};

// Split the outbox into what may be sent on the current connection and what has to wait. An entry is
// sent when BOTH hold:
//  - room:  its scope is this connection's scope, or it has none;
//  - seed:  its seed is the loaded slot's masterSeed, or it has none (0).
// Everything else STAYS (it is not dropped): held finds of another save slot, or of another room, go
// out when that slot is loaded in that room again, until they expire. loadedSeed 0 = no slot loaded.
inline OutboxFlush OutboxTakeSendable(std::deque<OutboxEntry>& outbox, uint32_t loadedSeed, const std::string& scope,
                                      uint32_t ownClientId, int64_t now) {
    OutboxFlush result;
    std::deque<OutboxEntry> keep;
    for (auto& e : outbox) {
        if (OutboxEntryExpired(e, now)) {
            result.expired++;
            continue;
        }
        const bool roomOk = e.scope.empty() || e.scope == scope;
        const bool seedOk = e.seed == 0 || e.seed == loadedSeed;
        if (roomOk && seedOk) {
            result.send.push_back(RestampClientId(e.packet, ownClientId));
        } else {
            keep.push_back(std::move(e));
        }
    }
    outbox.swap(keep);
    return result;
}

// Disk form: { "version": 1, "entries": [ { seed, heldAt, packet, scope? } ] }. Entries with an
// unknown seed are not written: they could not be matched to a playthrough on a later run. "scope" is
// optional, so files written by the first outbox build still load (and that build ignores the field).
inline nlohmann::json OutboxToJson(const std::deque<OutboxEntry>& outbox) {
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& e : outbox) {
        if (e.seed == 0)
            continue;
        nlohmann::json j = nlohmann::json::object();
        j["seed"] = e.seed;
        j["heldAt"] = e.heldAt;
        j["packet"] = e.packet;
        if (!e.scope.empty())
            j["scope"] = e.scope;
        entries.push_back(std::move(j));
    }
    nlohmann::json out = nlohmann::json::object();
    out["version"] = 1;
    out["entries"] = std::move(entries);
    return out;
}

// Append the usable entries of a parsed outbox file. Skips expired, seedless, empty and malformed
// entries; one bad entry never discards the rest. Returns how many were loaded.
inline size_t OutboxFromJson(const nlohmann::json& file, std::deque<OutboxEntry>& outbox, int64_t now) {
    if (!file.is_object())
        return 0;
    const auto entries = file.find("entries");
    if (entries == file.end() || !entries->is_array())
        return 0;
    size_t loaded = 0;
    for (const auto& e : *entries) {
        if (!e.is_object())
            continue;
        try {
            OutboxEntry entry;
            entry.seed = e.value("seed", static_cast<uint32_t>(0));
            entry.heldAt = e.value("heldAt", static_cast<int64_t>(0));
            entry.scope = e.value("scope", std::string());
            entry.packet = e.value("packet", std::string());
            if (entry.seed == 0 || entry.packet.empty() || OutboxEntryExpired(entry, now))
                continue;
            OutboxPush(outbox, std::move(entry));
            loaded++;
        } catch (...) {} // wrong field type in this entry
    }
    return loaded;
}

} // namespace ComboAnchorCore
