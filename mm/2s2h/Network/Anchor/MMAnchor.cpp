#include "MMAnchor.h"
#ifdef COMBO_BUILD
#include "ComboExport.h"

#include <memory>
#include <spdlog/spdlog.h>
#include "rando/ComboAnchorToast.h" // shared cross-game resync-toast debounce
#include "net/ComboAnchorMmFlags.h" // MM flag sync: packet vocabulary + bit rules (unit-tested)
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/NameTag/NameTag.h"
#include "2s2h/Rando/Rando.h" // Rando::GiveItem / ConvertItem / CurrentJunkItem / RANDO_SAVE_CHECKS / IS_RANDO
#include "2s2h/Rando/CheckTracker/CheckTracker.h" // Phase 2d: re-derive after team-state apply
#include "2s2h/Rando/ActorBehavior/ActorBehavior.h"
#include "2s2h/Rando/MiscBehavior/MiscBehavior.h" // MM_LookupForeign (foreign backfill)
#include "2s2h/ShipInit.hpp"
#include "2s2h/ShipUtils.h" // ComboShip: Ship_GetSceneName for the roster area-name supplement
#include "2s2h/BenGui/Notification.h"
#include "2s2h/SaveManager/SaveManager.h" // dormant team-state persist (SaveManager_SaveCurrentForCombo)
#include "BenJsonConversions.hpp"         // to_json/from_json for Vec3f/Vec3s/PosRot AND Save (Phase 2d)

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "build.h" // declares gGitCommitHash
extern PlayState* gPlayState;
}

// Shared CVar keys (process-global libultraship store): OOT's Anchor menu writes these and MM reads
// the same values. CVAR_REMOTE_ANCHOR("X") expands to "gRemote.Anchor.X" in ComboShip's SoH; MM has
// no access to that macro, so the literals are spelled out here.
static constexpr const char* kCvarName = "gRemote.Anchor.Name";
static constexpr const char* kCvarColor = "gRemote.Anchor.Color.Value";
static constexpr const char* kCvarTeamId = "gRemote.Anchor.TeamId";
static constexpr const char* kCvarLastClientId = "gRemote.Anchor.LastClientId";
static constexpr const char* kCvarTeleportMode = "gRemote.Anchor.RoomSettings.TeleportMode"; // teleport gate

// Packet type strings — must match SoH's Anchor exactly for cross-client interop.
static const std::string PKT_ALL_CLIENT_STATE = "ALL_CLIENT_STATE";
static const std::string PKT_UPDATE_CLIENT_STATE = "UPDATE_CLIENT_STATE";
static const std::string PKT_PLAYER_UPDATE = "PLAYER_UPDATE";
static const std::string PKT_DAMAGE_PLAYER = "DAMAGE_PLAYER";
static const std::string PKT_GIVE_ITEM = "GIVE_ITEM";
static const std::string PKT_UPDATE_TEAM_STATE = "UPDATE_TEAM_STATE";
static const std::string PKT_REQUEST_TEAM_STATE = "REQUEST_TEAM_STATE";
// Co-op next (2026-11-13): the room owner's settings. Sent by the server after the handshake and by
// soh's Anchor when the owner changes them (neither copy carries originGame, so both pass our filter).
static const std::string PKT_UPDATE_ROOM_STATE = "UPDATE_ROOM_STATE";
// Co-op next block 3 (2026-11-14): MM flag sync (see the end of this file).
static const std::string PKT_MM_FLAG = ComboAnchorCore::kMmFlagPacketType;
// Issue #3: cross-game item delivery. ComboShip-private packet type (the public server relays unknown
// types peer-to-peer, so no server change is needed).
static const std::string PKT_CROSS_ITEM = "COMBO_CROSS_ITEM";
// Live partner feed (2026-11-07): display-only, shared with soh's Anchor (Anchor.cpp). Passes the
// origin filter in both directions, since showing the OTHER game's finds is the point of it.
static const std::string PKT_PARTNER_FIND = "COMBO_PARTNER_FIND";
// Same-game teleport. Same wire strings as soh's Anchor; MM-origin so soh clients drop them.
static const std::string PKT_REQUEST_TELEPORT = "REQUEST_TELEPORT";
static const std::string PKT_TELEPORT_TO = "TELEPORT_TO";

// Launcher-registered outbound transport (set via MM_SetAnchorSend). Null until the exe wires it.
extern "C" {
void (*gMMComboAnchorSend)(const char* json) = nullptr;
}

// Issue #3: cross-game delivery seams, defined in BenPort.cpp and registered by the launcher. Route
// a received cross-game item into the TARGET game's save, and mark the SOURCE check obtained.
extern "C" void (*gMMComboCrossDeliver)(int targetGame, const char* itemName, const char* srcCheckName);
extern "C" void (*gMMComboTriforceProgress)(int game, int fileNum);
// Shared Items: a teammate's merged tier can be higher than ours — re-evaluate.
extern "C" void (*gMMComboSharedChanged)(int game, int fileNum);
extern "C" void (*gMMComboMarkForeignObtained)(int srcGame, const char* checkName);

// ComboShip A6: launcher pump fn (set via MM_SetPumpDormant). The ACTIVE game calls it each frame so
// the launcher can drive the DORMANT sibling's save-affecting packet apply on this (game) thread.
extern "C" void (*gMMComboPumpDormant)() = nullptr;
// Dormant-safe give of a resolved MM rando item into the resident save + persist (BenPort.cpp).
void Combo_MM_GiveDormantResolved(RandoItemId rid);

MMAnchor* MMAnchor::Instance = nullptr;

// MARK: - Lifecycle

void MMAnchor::Activate() {
    isActive = true;
    // Adopt the shared connection's client id (OOT caches it on ALL_CLIENT_STATE; same socket => same
    // id). Without this MM stamps outbound packets with clientId 0 and peers drop them.
    if (ownClientId == 0) {
        ownClientId = (uint32_t)CVarGetInteger(kCvarLastClientId, 0);
    }
    RegisterHooks();
    SendUpdateClientState();    // announce MM presence / live location
    shouldRefreshActors = true; // spawn puppets for already-known peers
    if (IsSaveLoaded()) {
        SendPacket_RequestTeamState(); // connected while already in-game -> catch up to the team
    }
    SPDLOG_INFO("[MMAnchor] activated (ownClientId={})", ownClientId);
}

void MMAnchor::Deactivate() {
    isActive = false;
    // Drop an unsent cycle-save broadcast: it would otherwise fire unsolicited on the next activation.
    pendingCycleSaveBroadcast = false;
    SPDLOG_INFO("[MMAnchor] deactivated");
}

bool MMAnchor::IsSaveLoaded() {
    return gPlayState != nullptr && GET_PLAYER(gPlayState) != nullptr;
}

bool MMAnchor::HasLoadedRandoSave() {
    return gSaveContext.fileNum >= 0 && gSaveContext.fileNum <= 2 && IS_RANDO;
}

void MMAnchor::RegisterHooks() {
    if (hooksRegistered) {
        return;
    }
    hooksRegistered = true;

    // Hooks are registered once and gated by isActive (rather than COND_HOOK's connect/disconnect
    // re-registration), since MM's connection lifecycle is the launcher's, not ours.

    // Re-announce presence + request puppet refresh whenever a scene loads.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneInit>([this](s8 sceneId, s8 spawnNum) {
        if (!isActive) {
            return;
        }
        SendUpdateClientState();
        shouldRefreshActors = true;
    });

    // Disguise hook: a freshly spawned ACTOR_PLAYER during RefreshClientActors becomes a puppet.
    GameInteractor::Instance->RegisterGameHookForID<GameInteractor::ShouldActorInit>(
        ACTOR_PLAYER, [this](Actor* actor, bool* should) {
            if (!isActive || !refreshingActors) {
                return;
            }
            // The actor was already added to the ACTORCAT_PLAYER list; move it to NPC and rebind.
            Actor_ChangeCategory(gPlayState, &gPlayState->actorCtx, actor, ACTORCAT_NPC);
            actor->id = ACTOR_ITEM_INBOX;
            actor->category = ACTORCAT_NPC;
            actor->init = DummyPlayer_Init;
            actor->update = DummyPlayer_Update;
            actor->draw = DummyPlayer_Draw;
            actor->destroy = DummyPlayer_Destroy;
        });

    // Local-player update: broadcast our pose and service a pending puppet refresh. Puppets are
    // ACTOR_ITEM_INBOX, so this ACTOR_PLAYER-filtered hook only fires for the real local player.
    GameInteractor::Instance->RegisterGameHookForID<GameInteractor::OnActorUpdate>(ACTOR_PLAYER, [this](Actor* actor) {
        if (!isActive) {
            return;
        }
        SendPlayerUpdate();
        if (shouldRefreshActors) {
            shouldRefreshActors = false;
            RefreshClientActors();
        }
    });

    // Drain inbound packets on the MM game thread (the launcher's receive thread only enqueues).
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameStateUpdate>([this]() {
        if (isActive) {
            ProcessIncomingPacketQueue();
            // A6: while MM is foreground, drive the dormant sibling's dormant-safe apply on this thread.
            if (gMMComboPumpDormant) {
                gMMComboPumpDormant();
            }
            // GameState_Update runs main (where Sram_SaveEndOfCycle and every AfterEndOfCycleSave hook
            // complete) before this hook, so the deferred send lands the same frame, after the restore.
            if (pendingCycleSaveBroadcast && gPlayState != nullptr) {
                pendingCycleSaveBroadcast = false;
                SendPacket_UpdateTeamState(CVarGetString(kCvarTeamId, "default"));
            }
            PollOwlStatues(); // block 3: owl activations have no hook
        }
    });

    // Block 3: flag sync. Both hooks fire only on a real off -> on change made by the game; remote
    // flags are written straight into the save (HandlePacket_MmFlag), so they never come back here.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneFlagSet>([this](s16 scene, FlagType t, u32 f) {
        if (isActive) {
            OnLocalSceneFlagSet(scene, t, f);
        }
    });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnFlagSet>([this](FlagType flagType, u32 flag) {
        if (isActive && flagType == FLAG_WEEK_EVENT_REG) {
            OnLocalWeekEventSet(flag);
        }
    });

    // Phase 2d: catch up to the team's progression when loading a file; push our state when we save.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSaveLoad>([this](s16 fileNum) {
        if (isActive) {
            SendPacket_RequestTeamState();
        }
    });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::AfterEndOfCycleSave>([this]() {
        if (isActive && gPlayState != nullptr) {
            // Defer: the rando key/check restore is another AfterEndOfCycleSave hook and may not have run
            // yet. Losing this on a same-frame quit is harmless — peers re-request on connect.
            pendingCycleSaveBroadcast = true;
        }
    });
}

// MARK: - Transport

void MMAnchor::SendJson(nlohmann::json payload) {
    if (!isActive || gMMComboAnchorSend == nullptr) {
        return;
    }
    if (ownClientId == 0) {
        ownClientId = (uint32_t)CVarGetInteger(kCvarLastClientId, 0); // late-cache fallback
    }
    payload["clientId"] = ownClientId;
    // Routing tag for the shared socket; NOT the cross-item "srcGame" int field (don't clobber it).
    payload["originGame"] = "mm";
    gMMComboAnchorSend(payload.dump().c_str());
}

void MMAnchor::OnIncomingJson(const std::string& payload) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[MMAnchor] failed to parse json: {}", e.what());
        return;
    }
    if (!j.contains("type")) {
        return;
    }
    // Drop OOT-originated packets — their shapes collide with ours (GIVE_ITEM, UPDATE_TEAM_STATE...).
    // Exceptions: cross-game item delivery and the room roster. No originGame (server) passes through.
    std::string type = j["type"].get<std::string>();
    if (j.value("originGame", "mm") != "mm" && type != PKT_CROSS_ITEM && type != PKT_UPDATE_CLIENT_STATE &&
        type != PKT_PARTNER_FIND) {
        return;
    }
    std::lock_guard<std::mutex> lock(incomingMutex);
    incomingQueue.push(std::move(j));
}

void MMAnchor::ProcessIncomingPacketQueue() {
    std::queue<nlohmann::json> toProcess;
    {
        std::lock_guard<std::mutex> lock(incomingMutex);
        toProcess.swap(incomingQueue);
    }
    while (!toProcess.empty()) {
        nlohmann::json payload = toProcess.front();
        toProcess.pop();
        std::string type = payload["type"].get<std::string>();
        try {
            if (type == PKT_ALL_CLIENT_STATE) {
                HandlePacket_AllClientState(payload);
            } else if (type == PKT_UPDATE_CLIENT_STATE) {
                HandlePacket_UpdateClientState(payload);
            } else if (type == PKT_PLAYER_UPDATE) {
                HandlePacket_PlayerUpdate(payload);
            } else if (type == PKT_DAMAGE_PLAYER) {
                HandlePacket_DamagePlayer(payload);
            } else if (type == PKT_GIVE_ITEM) {
                HandlePacket_GiveItem(payload);
            } else if (type == PKT_CROSS_ITEM) {
                HandlePacket_CrossItem(payload);
            } else if (type == PKT_PARTNER_FIND) {
                HandlePacket_PartnerFind(payload);
            } else if (type == PKT_UPDATE_TEAM_STATE) {
                HandlePacket_UpdateTeamState(payload);
            } else if (type == PKT_REQUEST_TEAM_STATE) {
                HandlePacket_RequestTeamState(payload);
            } else if (type == PKT_UPDATE_ROOM_STATE) {
                HandlePacket_UpdateRoomState(payload);
            } else if (type == PKT_MM_FLAG) {
                HandlePacket_MmFlag(payload);
            } else if (type == PKT_REQUEST_TELEPORT) {
                HandlePacket_RequestTeleport(payload);
            } else if (type == PKT_TELEPORT_TO) {
                HandlePacket_TeleportTo(payload);
            }
            // Flag sync intentionally deferred (mirrors canonical, which stubs it).
        } catch (const std::exception& e) {
            SPDLOG_ERROR("[MMAnchor] exception handling packet {}: {}", type, e.what());
        }
    }
}

// A6: called on the ACTIVE sibling's game thread while MM is dormant. Drain the queue and apply only
// save-data-only, dormant-safe packets (co-op GIVE_ITEM, team-state resync). Presence/puppet packets
// are dropped. gSaveContext here is MM's frozen save; MM isn't ticking, so no concurrent writer.
void MMAnchor::PumpDormant() {
    std::queue<nlohmann::json> toProcess;
    {
        std::lock_guard<std::mutex> lock(incomingMutex);
        toProcess.swap(incomingQueue);
    }
    dormantDidApply = false; // reset once per pump, not per-packet — mirrors soh's Anchor
    // Exception-safe: the surrounding try/catch alone would skip a plain reset-after-call line.
    struct DormantApplyGuard {
        MMAnchor* self;
        explicit DormantApplyGuard(MMAnchor* s) : self(s) {
            self->isDormantApply = true;
        }
        ~DormantApplyGuard() {
            self->isDormantApply = false;
        }
    };
    while (!toProcess.empty()) {
        nlohmann::json payload = toProcess.front();
        toProcess.pop();
        try {
            if (payload.value("type", std::string()) == PKT_UPDATE_ROOM_STATE) {
                // Keep roomState current while dormant: syncItemsAndFlags gates the dormant apply below,
                // and a client that stays in OOT never handles this packet in MM's foreground.
                HandlePacket_UpdateRoomState(payload);
            } else if (payload.value("type", std::string()) == PKT_GIVE_ITEM && payload.contains("randoCheckId")) {
                if (!roomState.syncItemsAndFlags) {
                    continue; // the room has item sync off (e.g. the public soh-global room)
                }
                SPDLOG_INFO("[MMAnchor] dormant GIVE_ITEM received: {}", payload.dump());
                ApplyDormantGiveItem(payload);
            } else if (payload.value("type", std::string()) == PKT_REQUEST_TEAM_STATE) {
                // Answering is dormant-safe (read-only over the frozen save); only APPLYING a
                // received team state is not. Without this, a teammate's resync silently gets
                // nothing whenever this client is in the other game.
                SPDLOG_INFO("[MMAnchor] dormant REQUEST_TEAM_STATE: answering from frozen save");
                SendTeamStateFromSave(payload.value("targetTeamId", std::string("default")));
            } else if (payload.value("type", std::string()) == PKT_UPDATE_TEAM_STATE) {
                // Bug: previously dropped entirely while dormant. The merge itself only touches
                // gSaveContext.save, so it's dormant-safe once the scene-bound post-steps are skipped
                // (guarded by isDormantApply inside the handler).
                SPDLOG_INFO("[MMAnchor] dormant UPDATE_TEAM_STATE received");
                DormantApplyGuard guard(this);
                HandlePacket_UpdateTeamState(payload); // sets dormantDidApply only on a true commit
            } else if (payload.value("type", std::string()) == PKT_MM_FLAG) {
                // Block 3: save-only writes (no gPlayState needed); sets dormantDidApply on a change.
                DormantApplyGuard guard(this);
                HandlePacket_MmFlag(payload);
            }
        } catch (const std::exception& e) { SPDLOG_ERROR("[MMAnchor] dormant apply exception: {}", e.what()); }
    }
    // Persist iff something actually merged AND a real slot is loaded — not merely "looked applyable".
    if (dormantDidApply && HasLoadedRandoSave()) {
        SaveManager_SaveCurrentForCombo();
        SPDLOG_INFO("[MMAnchor] dormant MM save persisted after team-state apply");
    }
}

// A6: dormant-safe variant of HandlePacket_GiveItem — marks the check obtained and grants via
// Combo_MM_GiveDormantResolved -> Rando::GiveItem (gPlayState==NULL). Junk resolves normally.
void MMAnchor::ApplyDormantGiveItem(const nlohmann::json& payload) {
    if (!payload.contains("randoCheckId")) {
        return; // reject soh's GIVE_ITEM (modId shape, different item-id space)
    }
    if (payload.value("targetTeamId", std::string("default")) != CVarGetString(kCvarTeamId, "default")) {
        SPDLOG_INFO("[MMAnchor] dormant GIVE_ITEM dropped: team mismatch");
        return;
    }
    uint32_t clientId = payload.value("clientId", (uint32_t)0);
    // Only a NONZERO match is our own echo — dormant MM may still have ownClientId 0, and a sender
    // that hasn't been assigned an id yet also stamps 0; 0==0 must not drop a teammate's item.
    if (clientId != 0 && clientId == ownClientId) {
        return;
    }
    int32_t checkId = payload.value("randoCheckId", -1);
    RandoCheckId rc = (checkId >= 0 && checkId < RC_MAX) ? (RandoCheckId)checkId : RC_UNKNOWN;
    if (rc == RC_UNKNOWN) {
        SPDLOG_INFO("[MMAnchor] dormant GIVE_ITEM dropped: bad check id {}", checkId);
        return; // no check to mark obtained -> no idempotency; a real sender always has one
    }
    if (RANDO_SAVE_CHECKS[rc].obtained) {
        SPDLOG_INFO("[MMAnchor] dormant GIVE_ITEM dropped: check {} already obtained", checkId);
        return; // idempotent: already collected locally
    }
    RandoItemId rid = Rando::ConvertItem((RandoItemId)payload.value("getItemId", (int)RI_JUNK), rc);
    if (rid == RI_JUNK) {
        // Co-op MM pickup: resolve real rotating junk (CurrentJunkItem is dormant-safe).
        rid = Rando::CurrentJunkItem(rc);
    }
    RANDO_SAVE_CHECKS[rc].obtained = true;
    RANDO_SAVE_CHECKS[rc].cycleObtained = true;
    RANDO_SAVE_CHECKS[rc].eligible = false;
    Combo_MM_GiveDormantResolved(rid);
    SPDLOG_INFO("[MMAnchor] dormant GIVE_ITEM applied: check={} item={} (persisted)", checkId, (int)rid);
}

// MARK: - Client state

nlohmann::json MMAnchor::PrepClientState() {
    nlohmann::json p;
    p["name"] = CVarGetString(kCvarName, "");
    Color_RGB8 c = CVarGetColor24(kCvarColor, { 100, 255, 100 });
    p["color"] = { { "r", c.r }, { "g", c.g }, { "b", c.b } };
    p["clientVersion"] = (char*)gGitCommitHash;
    p["teamId"] = CVarGetString(kCvarTeamId, "default");
    p["online"] = true;

    const bool saveLoaded = IsSaveLoaded();
    if (saveLoaded) {
        s16 rawScene = gPlayState->sceneId;
        p["seed"] = 0; // TODO Phase 3: MM rando seed
        p["isSaveLoaded"] = true;
        p["isGameComplete"] = false;
        // Two scene fields: namespaced "sceneNum" for OOT's roster display (so OOT shows MM peers as
        // "Majora's Mask"), and raw "sceneId" for MM peers' same-scene puppet matching.
        p["sceneNum"] = MM_ANCHOR_SCENE_NAMESPACE + (int32_t)rawScene;
        p["sceneId"] = rawScene;
        p["curRoomNum"] = (int32_t)gPlayState->roomCtx.curRoom.num;
        p["entrance"] = gSaveContext.save.entrance;
        p["entranceIndex"] = MM_ANCHOR_SCENE_NAMESPACE;
    } else {
        p["seed"] = 0;
        p["isSaveLoaded"] = false;
        p["isGameComplete"] = false;
        p["sceneNum"] = MM_ANCHOR_SCENE_NAMESPACE;
        p["sceneId"] = (int32_t)SCENE_MAX;
        p["curRoomNum"] = -1;
        p["entrance"] = 0;
        p["entranceIndex"] = MM_ANCHOR_SCENE_NAMESPACE;
    }
    return p;
}

void MMAnchor::SendUpdateClientState() {
    nlohmann::json payload;
    payload["type"] = PKT_UPDATE_CLIENT_STATE;
    payload["state"] = PrepClientState();
    SendJson(payload);
}

static void ReadClientStateInto(MMAnchorClient& c, uint32_t clientId, const nlohmann::json& s) {
    c.clientId = clientId;
    c.name = s.value("name", c.name.empty() ? std::string("???") : c.name);
    c.clientVersion = s.value("clientVersion", c.clientVersion);
    c.teamId = s.value("teamId", c.teamId);
    c.online = s.value("online", c.online);
    c.seed = s.value("seed", c.seed);
    c.isSaveLoaded = s.value("isSaveLoaded", c.isSaveLoaded);
    c.isGameComplete = s.value("isGameComplete", c.isGameComplete);
    c.sceneId = (s16)s.value("sceneId", (int32_t)c.sceneId); // RAW scene for puppet matching
    c.entrance = s.value("entrance", c.entrance);
}

// ComboShip: game a loaded client state reports (0 OOT, 1 MM), or -1 when no save is loaded.
static int8_t ClientStateGame(const nlohmann::json& s) {
    if (!s.value("isSaveLoaded", false)) {
        return -1;
    }
    return (int8_t)(s.value("sceneNum", 0) >= MM_ANCHOR_SCENE_NAMESPACE ? 1 : 0);
}

void MMAnchor::HandlePacket_AllClientState(const nlohmann::json& payload) {
    if (!payload.contains("state") || !payload["state"].is_array()) {
        return;
    }
    for (const auto& entry : payload["state"]) {
        uint32_t clientId = entry.value("clientId", (uint32_t)0);
        MMAnchorClient& c = clients[clientId];
        ReadClientStateInto(c, clientId, entry);
        if (int8_t game = ClientStateGame(entry); game != -1) {
            c.lastGame = game; // seed only — connecting never toasts
        }
        c.self = entry.value("self", false);
        if (c.self) {
            ownClientId = clientId;
        }
    }
    shouldRefreshActors = true;
    SPDLOG_INFO("[MMAnchor] ALL_CLIENT_STATE: {} client(s), ownClientId={}", clients.size(), ownClientId);
    // Re-announce now that we know our client id + the roster. This is what makes presence work when
    // Anchor is enabled while already in MM (no transition/scene-load to trigger the announce, and the
    // initial Activate announce ran before we had a client id).
    SendUpdateClientState();
}

void MMAnchor::HandlePacket_UpdateClientState(const nlohmann::json& payload) {
    uint32_t clientId = payload.value("clientId", (uint32_t)0);
    if (!payload.contains("state")) {
        return;
    }
    MMAnchorClient& c = clients[clientId];
    ReadClientStateInto(c, clientId, payload["state"]);

    // ComboShip: game-switch toast (2026-11-07). A peer's two games share one clientId, so a loaded
    // state from the other game than last time means the teammate crossed over.
    int8_t game = ClientStateGame(payload["state"]);
    if (game == -1) {
        return;
    }
    if (c.lastGame != -1 && c.lastGame != game && !c.self && clientId != ownClientId &&
        c.teamId == CVarGetString(kCvarTeamId, "default") &&
        CVarGetInteger("gRemote.Anchor.PartnerFeed.GameSwitch", 1)) {
        Notification::Emit({
            .prefix = c.name,
            .message = "is now in",
            .suffix = game == 1 ? "Majora's Mask" : "Ocarina of Time",
        });
    }
    c.lastGame = game;
}

// MARK: - Player update (per-frame pose; canonical field set from 2S2H PR #1349)

void MMAnchor::SendPlayerUpdate() {
    if (!IsSaveLoaded()) {
        return;
    }
    // Only send to peers in the SAME raw scene (matches canonical; avoids spamming cross-scene /
    // cross-game peers). Bail early if there are none.
    uint32_t sameScene = 0;
    for (auto& [clientId, client] : clients) {
        if (client.sceneId == gPlayState->sceneId && client.online && client.isSaveLoaded && !client.self) {
            sameScene++;
        }
    }
    if (sameScene == 0) {
        return;
    }

    Player* player = GET_PLAYER(gPlayState);
    nlohmann::json payload;
    payload["type"] = PKT_PLAYER_UPDATE;
    payload["sceneId"] = gPlayState->sceneId;
    payload["entrance"] = gSaveContext.save.entrance;
    payload["roomIndex"] = gPlayState->roomCtx.curRoom.num;
    payload["transformation"] = player->transformation;
    payload["posRot"]["pos"] = player->actor.world.pos;
    payload["posRot"]["rot"] = player->actor.shape.rot;
    // Serialize the 159-byte joint buffers as plain int arrays (nlohmann reserves std::vector<u8>
    // for its binary type, so a u8 array/vector round-trip fails to compile). Wire shape is an
    // array of numbers either way.
    {
        const u8* jt = (const u8*)&player->jointTableBuffer;
        const u8* ujt = (const u8*)&player->jointTableUpperBuffer;
        nlohmann::json jtArr = nlohmann::json::array();
        nlohmann::json ujtArr = nlohmann::json::array();
        for (int i = 0; i < 159; i++) {
            jtArr.push_back((int)jt[i]);
            ujtArr.push_back((int)ujt[i]);
        }
        payload["jointTable"] = jtArr;
        payload["upperJointTable"] = ujtArr;
    }
    payload["currentMask"] = player->currentMask;
    payload["rightHandType"] = player->rightHandType;
    payload["leftHandType"] = player->leftHandType;
    payload["currentShield"] = player->currentShield;
    payload["sheathType"] = player->sheathType;
    payload["heldItemAction"] = player->heldItemAction;
    payload["heldItemId"] = player->heldItemId;
    payload["itemAction"] = player->itemAction;
    payload["stateFlags1"] = player->stateFlags1;
    payload["stateFlags2"] = player->stateFlags2;
    payload["stateFlags3"] = player->stateFlags3;
    payload["unk_B0C"] = player->unk_B0C;
    payload["unk_B28"] = player->unk_B28;
    payload["unk_ACC"] = player->unk_ACC;
    payload["invincibilityTimer"] = player->invincibilityTimer;
    payload["quiet"] = true;

    for (auto& [clientId, client] : clients) {
        if (client.sceneId == gPlayState->sceneId && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJson(payload);
        }
    }
}

void MMAnchor::HandlePacket_PlayerUpdate(const nlohmann::json& payload) {
    uint32_t clientId = payload.value("clientId", (uint32_t)0);
    if (clientId == 0 || clientId == ownClientId || !clients.contains(clientId)) {
        return;
    }
    auto& client = clients[clientId];

    if (client.transformation != payload.value("transformation", (uint8_t)0)) {
        shouldRefreshActors = true; // form changed -> skeleton must be reallocated (respawn)
    }

    client.sceneId = payload.value("sceneId", (s16)SCENE_MAX);
    client.entrance = payload.value("entrance", (s32)0);
    client.transformation = payload.value("transformation", (uint8_t)0);
    // No from_json<PosRot> in this project's BenJsonConversions — read pos/rot via the Vec3f/Vec3s
    // converters that do exist.
    if (payload.contains("posRot")) {
        const auto& pr = payload["posRot"];
        if (pr.contains("pos")) {
            client.posRot.pos = pr["pos"].get<Vec3f>();
        }
        if (pr.contains("rot")) {
            client.posRot.rot = pr["rot"].get<Vec3s>();
        }
    }
    if (payload.contains("jointTable") && payload["jointTable"].is_array()) {
        const auto& arr = payload["jointTable"];
        for (int i = 0; i < 159 && i < (int)arr.size(); i++) {
            client.jointTable[i] = (u8)arr[i].get<int>();
        }
    }
    if (payload.contains("upperJointTable") && payload["upperJointTable"].is_array()) {
        const auto& arr = payload["upperJointTable"];
        for (int i = 0; i < 159 && i < (int)arr.size(); i++) {
            client.upperJointTable[i] = (u8)arr[i].get<int>();
        }
    }
    client.currentMask = payload.value("currentMask", (uint8_t)0);
    client.rightHandType = payload.value("rightHandType", (uint8_t)0);
    client.leftHandType = payload.value("leftHandType", (uint8_t)0);
    client.currentShield = payload.value("currentShield", (int8_t)0);
    client.sheathType = payload.value("sheathType", (uint8_t)0);
    client.heldItemAction = payload.value("heldItemAction", (int8_t)0);
    client.heldItemId = payload.value("heldItemId", (uint8_t)0);
    client.itemAction = payload.value("itemAction", (int8_t)0);
    client.stateFlags1 = payload.value("stateFlags1", (uint32_t)0);
    client.stateFlags2 = payload.value("stateFlags2", (uint32_t)0);
    client.stateFlags3 = payload.value("stateFlags3", (uint32_t)0);
    client.unk_B0C = payload.value("unk_B0C", 0.0f);
    client.unk_B28 = payload.value("unk_B28", (int16_t)0);
    client.unk_ACC = payload.value("unk_ACC", (int16_t)0);
    client.invincibilityTimer = payload.value("invincibilityTimer", (int8_t)0);
}

// MARK: - PvP damage (stub plumbing; full PvP is a later increment)

void MMAnchor::SendPacket_DamagePlayer(uint32_t clientId, uint8_t damageEffect, uint8_t damage) {
    nlohmann::json payload;
    payload["type"] = PKT_DAMAGE_PLAYER;
    payload["targetClientId"] = clientId;
    payload["damageEffect"] = damageEffect;
    payload["damage"] = damage;
    SendJson(payload);
}

void MMAnchor::HandlePacket_DamagePlayer(const nlohmann::json& payload) {
    // Applying damage to the local player is a later increment; plumbing kept for parity.
}

// MARK: - Item sync (shared-progression co-op; Phase 2c)

void MMAnchor::SendPacket_GiveItem(int16_t randoItemId, int32_t randoCheckId) {
    if (!isActive || !roomState.syncItemsAndFlags) {
        SPDLOG_INFO("[MMAnchor] GIVE_ITEM not sent: active={} syncItems={}", isActive, roomState.syncItemsAndFlags);
        return;
    }
    nlohmann::json payload;
    payload["type"] = PKT_GIVE_ITEM;
    payload["targetTeamId"] = CVarGetString(kCvarTeamId, "default");
    payload["getItemId"] = randoItemId;     // RAW rando item id; receiver ConvertItems for its own state
    payload["randoCheckId"] = randoCheckId; // so receivers can mark the check obtained (no double-collect)
    // Co-op hardening (2026-11-13): server-queue it for a teammate who is offline right now, like soh's
    // GIVE_ITEM. Safe to replay: the receiver skips a check it already has marked obtained.
    payload["addToQueue"] = true;
    SendJson(payload);
    SPDLOG_INFO("[MMAnchor] GIVE_ITEM sent: check={} item={}", randoCheckId, randoItemId);

    // ComboShip: live partner feed — MM's GIVE_ITEM receive is silent, and a teammate in OOT would never
    // see this find at all. Non-junk only; the name is the RAW item's (progressive items read as such).
    auto it = Rando::StaticData::Items.find((RandoItemId)randoItemId);
    if (it != Rando::StaticData::Items.end() && it->second.randoItemType != RITYPE_JUNK && it->second.name) {
        SendPacket_PartnerFind(it->second.name, "mm", "item");
    }
}

void MMAnchor::SendPacket_PartnerFind(const std::string& itemName, const char* itemGame, const char* kind) {
    if (!isActive || itemName.empty()) {
        return;
    }
    nlohmann::json payload;
    payload["type"] = PKT_PARTNER_FIND;
    payload["targetTeamId"] = CVarGetString(kCvarTeamId, "default");
    payload["itemName"] = itemName;
    payload["itemGame"] = itemGame;
    payload["kind"] = kind;
    payload["quiet"] = true; // never addToQueue: a late joiner shouldn't get a burst of stale toasts
    SendJson(payload);
}

// Runs only while MM is the active game (PumpDormant drops this type). Shows every teammate find no
// other MM toast covers: same-game MM GIVE_ITEMs apply silently, OOT finds never reach MM otherwise,
// and cross items for MM already toast "Received:" in HandlePacket_CrossItem.
void MMAnchor::HandlePacket_PartnerFind(const nlohmann::json& payload) {
    uint32_t clientId = payload.value("clientId", (uint32_t)0);
    if (clientId == ownClientId) {
        return;
    }
    if (payload.value("targetTeamId", std::string("default")) != CVarGetString(kCvarTeamId, "default")) {
        return;
    }
    std::string itemName = payload.value("itemName", std::string());
    std::string kind = payload.value("kind", std::string("item"));
    if (itemName.empty() || (kind == "cross" && payload.value("itemGame", std::string()) != "oot") ||
        !CVarGetInteger("gRemote.Anchor.PartnerFeed.Finds", 1)) {
        return;
    }
    bool otherGame = kind == "cross" || payload.value("originGame", std::string("mm")) == "oot";
    std::string who = "Teammate";
    auto it = clients.find(clientId);
    if (it != clients.end() && !it->second.name.empty() && it->second.name != "???") {
        who = it->second.name;
    }
    Notification::Emit({ .prefix = who, .message = "found", .suffix = otherGame ? itemName + " (OoT)" : itemName });
}

void MMAnchor::HandlePacket_GiveItem(const nlohmann::json& payload) {
    if (!roomState.syncItemsAndFlags || !IsSaveLoaded()) {
        return;
    }
    // ComboShip: the shared socket also carries soh's GIVE_ITEM (modId shape, no randoCheckId). Its
    // item id is in OOT's space — skip it so we don't misgrant an unrelated MM item.
    if (!payload.contains("randoCheckId")) {
        return;
    }
    uint32_t clientId = payload.value("clientId", (uint32_t)0);
    // Co-op next (2026-11-13): only a NONZERO match is our own echo, like the dormant path
    // (ApplyDormantGiveItem). A sender that has no id yet stamps 0, and so do we until the roster
    // arrives; 0 == 0 used to drop a teammate's item here. An echo of our own find that slips through
    // with id 0 is still harmless: its check is already obtained (next test).
    if (clientId != 0 && clientId == ownClientId) {
        return; // never re-apply our own broadcast
    }
    // Shared progression is per-team; ignore items for other teams.
    if (payload.value("targetTeamId", std::string("default")) != CVarGetString(kCvarTeamId, "default")) {
        return;
    }

    int32_t checkId = payload.value("randoCheckId", -1);
    RandoCheckId rc = (checkId >= 0 && checkId < RC_MAX) ? (RandoCheckId)checkId : RC_UNKNOWN;
    // Co-op next (2026-11-13): no usable check id means nothing to mark obtained, so a replay (server
    // queue, the launcher's outbox) would grant the item again each time. The dormant path already
    // refused these; a real sender always has a check (see MMAnchor_BroadcastCheckItem).
    if (rc == RC_UNKNOWN) {
        SPDLOG_INFO("[MMAnchor] GIVE_ITEM dropped: bad check id {}", checkId);
        return;
    }
    // Idempotent per check: if we already collected this check locally (both players reached it, or a
    // duplicate/late broadcast), don't grant its item again.
    if (rc != RC_UNKNOWN && RANDO_SAVE_CHECKS[rc].obtained) {
        return;
    }
    RandoItemId raw = (RandoItemId)payload.value("getItemId", (int)RI_JUNK);
    RandoItemId rid = Rando::ConvertItem(raw, rc);
    if (rid == RI_JUNK) {
        rid = Rando::CurrentJunkItem(rc);
    }

    // Guard against the grant path re-triggering a broadcast.
    applyingRemoteItem = true;
    Rando::GiveItem(rid);
    applyingRemoteItem = false;

    // Mark the originating check obtained so this client won't also collect it and double-grant.
    if (rc != RC_UNKNOWN) {
        RANDO_SAVE_CHECKS[rc].obtained = true;
        RANDO_SAVE_CHECKS[rc].cycleObtained = true;
        RANDO_SAVE_CHECKS[rc].eligible = false;
    }
}

// Co-op next (2026-11-13): MM never read the room settings, so `syncItemsAndFlags` stayed at its
// default (on) whatever the owner chose — including the public soh-global room, which hardcodes item
// sync OFF. Only that one field is taken: pvpMode stays at its default on purpose, because the PvP
// receive side (HandlePacket_DamagePlayer) is still a stub and the puppet would start reporting hits.
// A field of the wrong type throws; both callers catch and keep the previous value.
void MMAnchor::HandlePacket_UpdateRoomState(const nlohmann::json& payload) {
    auto state = payload.find("state");
    if (state == payload.end() || !state->is_object()) {
        return;
    }
    const int sync = state->value("syncItemsAndFlags", (int)roomState.syncItemsAndFlags);
    if ((sync != 0) != (roomState.syncItemsAndFlags != 0)) {
        SPDLOG_INFO("[MMAnchor] room item/flag sync is now {}", sync != 0 ? "on" : "off");
    }
    roomState.syncItemsAndFlags = static_cast<uint8_t>(sync != 0 ? 1 : 0); // explicit: MSVC C4244 under /WX
}

// Called from the rando check-obtain seam (CheckQueue.cpp) under COMBO_BUILD when the LOCAL player
// collects a check. Broadcasts the raw item to teammates; no-op when Anchor is inactive or we're
// currently applying a remotely-received item (prevents a re-broadcast loop).
extern "C" void MMAnchor_BroadcastCheckItem(int randoCheckId, int randoItemId) {
    if (MMAnchor::Instance && MMAnchor::Instance->isActive && !MMAnchor::Instance->applyingRemoteItem) {
        MMAnchor::Instance->SendPacket_GiveItem((int16_t)randoItemId, randoCheckId);
    }
}

// MARK: - Cross-game item delivery (issue #3)

void MMAnchor::SendPacket_CrossItem(int targetGame, const char* itemName, const char* srcCheckName) {
    if (!isActive || !roomState.syncItemsAndFlags || !itemName || !srcCheckName) {
        return;
    }
    nlohmann::json payload;
    payload["type"] = PKT_CROSS_ITEM;
    payload["targetTeamId"] = CVarGetString(kCvarTeamId, "default");
    payload["targetGame"] = targetGame; // 0 = OOT, 1 = MM (item's home game)
    payload["srcGame"] = 1;             // collected in MM
    payload["itemName"] = itemName;     // neutral CrossForeign name, in the target game's namespace
    payload["srcCheckName"] = srcCheckName;
    // Co-op hardening (2026-11-13): server-queue for an offline teammate (receivers dedup on srcCheckName).
    payload["addToQueue"] = true;
    SendJson(payload);
}

void MMAnchor::HandlePacket_CrossItem(const nlohmann::json& payload) {
    if (!roomState.syncItemsAndFlags) {
        return;
    }
    uint32_t clientId = payload.value("clientId", (uint32_t)0);
    if (clientId == ownClientId) {
        return; // never re-apply our own broadcast
    }
    // Shared progression is per-team; ignore items for other teams.
    if (payload.value("targetTeamId", std::string("default")) != CVarGetString(kCvarTeamId, "default")) {
        return;
    }
    int targetGame = payload.value("targetGame", 0);
    int srcGame = payload.value("srcGame", 0);
    std::string itemName = payload.value("itemName", std::string());
    std::string srcCheckName = payload.value("srcCheckName", std::string());
    if (itemName.empty()) {
        return;
    }
    // Grant into the TARGET game's save (routes through the launcher to whichever DLL owns it; the
    // grant export bypasses the check-collect path, so this won't re-broadcast).
    if (gMMComboCrossDeliver) {
        gMMComboCrossDeliver(targetGame, itemName.c_str(), srcCheckName.c_str());
    }
    // Mark the source check obtained so we won't physically collect it later and double-deliver.
    if (gMMComboMarkForeignObtained && !srcCheckName.empty()) {
        gMMComboMarkForeignObtained(srcGame, srcCheckName.c_str());
    }
    // This handler only runs while MM is the active game, so a targetGame==1 item is one the MM
    // player just received — announce it (the save-only grant is otherwise silent).
    if (targetGame == 1) {
        Notification::Emit({ .message = "Received:", .suffix = itemName });
    }
}

// Called from the rando foreign-check seam (CheckQueue.cpp) under COMBO_BUILD when the LOCAL player
// collects a check whose item belongs to the OTHER game. Shares it with teammates; no-op when Anchor
// is inactive.
extern "C" void MMAnchor_BroadcastCrossItem(int targetGame, const char* itemName, const char* srcCheckName) {
    if (MMAnchor::Instance && MMAnchor::Instance->isActive) {
        MMAnchor::Instance->SendPacket_CrossItem(targetGame, itemName, srcCheckName);
        // Live partner feed: a teammate currently in MM gets this item silently into their dormant OOT.
        if (itemName) {
            MMAnchor::Instance->SendPacket_PartnerFind(itemName, targetGame == 0 ? "oot" : "mm", "cross");
        }
    }
}

// MARK: - Puppet refresh (spawn ACTOR_PLAYER per peer; ShouldActorInit re-tags them)

void MMAnchor::RefreshClientActors() {
    if (!IsSaveLoaded()) {
        return;
    }

    // Kill existing puppets.
    Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].first;
    while (actor != NULL) {
        Actor* next = actor->next;
        if (actor->id == ACTOR_ITEM_INBOX && actor->update == DummyPlayer_Update) {
            NameTag_RemoveAllForActor(actor);
            Actor_Kill(actor);
        }
        actor = next;
    }

    actorIndexToClientId.clear();
    refreshingActors = true;
    for (auto& [clientId, client] : clients) {
        if (!client.online || client.self) {
            continue;
        }
        actorIndexToClientId.push_back(clientId);
        // params = index into actorIndexToClientId; DummyPlayer_Init reads it back to find the client.
        Actor* dummy = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_PLAYER, client.posRot.pos.x,
                                   client.posRot.pos.y, client.posRot.pos.z, client.posRot.rot.x, client.posRot.rot.y,
                                   client.posRot.rot.z, (int)actorIndexToClientId.size() - 1);
        client.player = (Player*)dummy;
    }
    refreshingActors = false;
}

// MARK: - Team state resync (late-join / reconnect; Phase 2d, ported from 2S2H PR #1349)

void MMAnchor::SendPacket_RequestTeamState() {
    if (!isActive || !roomState.syncItemsAndFlags) {
        return;
    }
    nlohmann::json payload;
    payload["type"] = PKT_REQUEST_TEAM_STATE;
    payload["targetTeamId"] = CVarGetString(kCvarTeamId, "default");
    SendJson(payload);
}

// Bug 2: launcher-orchestrated resync (auto on connect + combo menu button). Bypasses SendJson's
// isActive gate — the whole point is that a dormant MM must also ask teammates for a fresh state,
// which SendPacket_RequestTeamState (isActive-gated) can't do.
void MMAnchor::RequestResyncDormantSafe() {
    if (gMMComboAnchorSend == nullptr || !roomState.syncItemsAndFlags) {
        return;
    }
    nlohmann::json payload;
    payload["type"] = PKT_REQUEST_TEAM_STATE;
    payload["targetTeamId"] = CVarGetString(kCvarTeamId, "default");
    payload["clientId"] = ownClientId;
    payload["originGame"] = "mm";
    gMMComboAnchorSend(payload.dump().c_str());
}

void MMAnchor::HandlePacket_RequestTeamState(const nlohmann::json& payload) {
    if (!IsSaveLoaded() || !roomState.syncItemsAndFlags) {
        return;
    }
    SendPacket_UpdateTeamState(CVarGetString(kCvarTeamId, "default"));
}

void MMAnchor::SendPacket_UpdateTeamState(const std::string& targetTeamId) {
    if (!isActive || !roomState.syncItemsAndFlags) {
        return;
    }
    SendTeamStateFromSave(targetTeamId);
}

// Read-only over gSaveContext, so safe both foreground and dormant (no isActive gate).
void MMAnchor::SendTeamStateFromSave(const std::string& targetTeamId) {
    // Bug 2: IsSaveLoaded() requires gPlayState (foreground only) — a dormant MM has none, so the
    // dormant answer path silently dropped every request. Judge by the resident save instead
    // (mirrors OOT's isDormantApply branch of Anchor::IsSaveLoaded).
    if (!HasLoadedRandoSave() || !roomState.syncItemsAndFlags) {
        return;
    }
    nlohmann::json payload;
    payload["type"] = PKT_UPDATE_TEAM_STATE;
    payload["targetTeamId"] = targetTeamId;
    payload["queue"] = nlohmann::json::array(); // assume our team queue is now empty
    {
        // Block 3: the game copies scene flags into the Save only when it saves, so gSaveContext.save
        // alone would report every chest opened since then as closed. Send a copy with the persistent
        // part brought up to date (heap: a Save holds the whole rando check table).
        auto snapshot = std::make_unique<Save>(gSaveContext.save);
        OverlayPersistentSceneFlags(*snapshot);
        payload["state"] = *snapshot; // to_json(Save) from BenJsonConversions
    }

    // Byte-reduction: replace the rando check array with a compact array-of-arrays. 7 fields here —
    // ComboShip's RandoSaveCheck has no multiWorldTeamIndex (shared-progression, not multiworld).
    if (IS_RANDO) {
        auto& rando = payload["state"]["shipSaveInfo"]["rando"];
        rando.erase("randoSaveChecks");
        rando["randoSaveChecksCopy"] = nlohmann::json::array();
        for (int i = 0; i < RC_MAX; i++) {
            nlohmann::json e = nlohmann::json::array();
            e[0] = RANDO_SAVE_CHECKS[i].randoItemId;
            e[1] = (u8)RANDO_SAVE_CHECKS[i].shuffled;
            e[2] = (u8)RANDO_SAVE_CHECKS[i].eligible;
            e[3] = (u8)RANDO_SAVE_CHECKS[i].cycleObtained;
            e[4] = (u8)RANDO_SAVE_CHECKS[i].obtained;
            e[5] = (u8)RANDO_SAVE_CHECKS[i].skipped;
            e[6] = RANDO_SAVE_CHECKS[i].price;
            rando["randoSaveChecksCopy"][i] = e;
        }
    }
    SendJson(payload);
}

// Co-op next block 2 (2026-11-14): replay the packets the server queued for the team (they ride in an
// UPDATE_TEAM_STATE's "queue") through the normal incoming path. Split out of the handler below so the
// queue is no longer lost whenever the STATE next to it is not usable: the server has one state slot
// per team, so after a teammate's OOT save every reply carries an OOT-shaped state (or none at all,
// before anyone saved), and the old early returns dropped the queued MM finds with it.
void MMAnchor::ReplayTeamStateQueue(const nlohmann::json& payload) {
    auto queue = payload.find("queue");
    if (queue == payload.end() || !queue->is_array()) {
        return;
    }
    std::lock_guard<std::mutex> lock(incomingMutex);
    for (const auto& item : *queue) {
        try {
            incomingQueue.push(nlohmann::json::parse(item.get<std::string>()));
        } catch (const std::exception& e) {
            SPDLOG_ERROR("[MMAnchor] failed to parse queued team-state packet: {}", e.what());
        }
    }
}

void MMAnchor::HandlePacket_UpdateTeamState(nlohmann::json& payload) {
    if (!roomState.syncItemsAndFlags) {
        return;
    }
    if (!payload.contains("state") || !payload["state"].contains("shipSaveInfo")) {
        // No state, or a soh-shaped one (from_json(Save) would throw). The queue is still ours to
        // replay — into a real save only (the dormant GIVE_ITEM apply has its own gates).
        if (HasLoadedRandoSave()) {
            ReplayTeamStateQueue(payload);
        }
        return;
    }
    // ComboShip: both guards must hold on the dormant path too (PumpDormant persists right after), so
    // neither may be conditioned on IS_RANDO. No vanilla mode here: a non-rando local save means nothing
    // usable is loaded, and preserving that saveType through the merge is the original key-eating bug.
    if (!HasLoadedRandoSave()) {
        SPDLOG_WARN("[MMAnchor] dropping team state: no loaded rando save (fileNum={}, IS_RANDO={})",
                    gSaveContext.fileNum, IS_RANDO);
        return;
    }
    // A non-rando peer serializes a zeroed rando struct, and the wholesale shipSaveInfo assign below
    // would wipe every RANDO_SAVE_CHECKS entry. No merge can recover that.
    if (!payload["state"]["shipSaveInfo"].contains("rando")) {
        SPDLOG_WARN("[MMAnchor] dropping team state with no rando block (peer is not in a rando save)");
        return;
    }

    // Unpack the compact rando check array back into the shape from_json(Save) expects.
    // Both conditions are guaranteed by the guards above.
    {
        auto stuff =
            payload["state"]["shipSaveInfo"]["rando"]["randoSaveChecksCopy"].get<std::vector<std::vector<s32>>>();
        for (int i = 0; i < RC_MAX; i++) {
            payload["state"]["shipSaveInfo"]["rando"]["randoSaveChecks"][i] = RandoSaveCheck{
                (RandoItemId)stuff[i][0], (bool)stuff[i][1], (bool)stuff[i][2], (bool)stuff[i][3],
                (bool)stuff[i][4],        (bool)stuff[i][5], (u16)stuff[i][6],
            };
        }
    }

    Save loadedData = payload["state"].get<Save>();

    // ComboShip (bug 3): union not replace — a teammate's team-state can be stale/incomplete (they
    // haven't reached a check we already have). Remember our permanently-obtained checks so the
    // wholesale shipSaveInfo assignment below can't clear one.
    bool localObtained[RC_MAX];
    for (int i = 0; i < RC_MAX; i++) {
        localObtained[i] = RANDO_SAVE_CHECKS[i].obtained;
    }

    // ComboShip (finding 1): mirror OOT's OR-merge (eventChkInf/randomizerInf/gsFlags) — snapshot
    // permanent progress so the wholesale saveInfo replace below can't regress an ahead player.
    u8 localWeekEventReg[100];
    memcpy(localWeekEventReg, gSaveContext.save.saveInfo.weekEventReg, sizeof(localWeekEventReg));
    u8 localMasks[24];
    memcpy(localMasks, &gSaveContext.save.saveInfo.inventory.items[24], sizeof(localMasks));
    u32 localQuestItems = gSaveContext.save.saveInfo.inventory.questItems;
    u32 localUpgrades = gSaveContext.save.saveInfo.inventory.upgrades;
    s16 localHealthCapacity = gSaveContext.save.saveInfo.playerData.healthCapacity;
    // Block 3: owl statues are permanent too — the playerData replace must not deactivate one.
    u16 localOwlFlags = gSaveContext.save.saveInfo.playerData.owlActivationFlags;
    // Bug 4/5: rupees/magic regress on the wholesale saveInfo replace — snapshot to re-max after.
    s16 localRupees = gSaveContext.save.saveInfo.playerData.rupees;
    s8 localMagic = gSaveContext.save.saveInfo.playerData.magic;
    // Bug 5: saveInfo replace clobbers permanentSceneFlags[120] — snapshot to OR back after the assign.
    PermanentSceneFlags localPermSceneFlags[120];
    memcpy(localPermSceneFlags, gSaveContext.save.saveInfo.permanentSceneFlags, sizeof(localPermSceneFlags));
#ifdef COMBO_BUILD
    // #136: the shipSaveInfo replace would regress the Triforce count against a stale peer.
    u16 localTriforcePieces = gSaveContext.save.shipSaveInfo.rando.foundTriforcePieces;
#endif
    // Issue #130: randoInf (obtained trade items/souls/purchases) is monotonic — snapshot to OR back.
    u16 localRandoInf[ARRAY_COUNT(gSaveContext.save.shipSaveInfo.rando.randoInf)];
    memcpy(localRandoInf, gSaveContext.save.shipSaveInfo.rando.randoInf, sizeof(localRandoInf));
    // Issue #130: trade slots hold the locally-selected deed/key/letter — a stale snapshot must not empty them.
    u8 localTradeItems[3] = { gSaveContext.save.saveInfo.inventory.items[SLOT_TRADE_DEED],
                              gSaveContext.save.saveInfo.inventory.items[SLOT_TRADE_KEY_MAMA],
                              gSaveContext.save.saveInfo.inventory.items[SLOT_TRADE_COUPLE] };
    // ComboShip: key/fairy/dungeon-item counters are monotonic too — a stale peer (or one mid-Song-of-Time,
    // whose keys are momentarily zero) would otherwise truncate ours. -1 is the fresh sentinel; signed max.
    s8 localDungeonKeys[ARRAY_COUNT(gSaveContext.save.saveInfo.inventory.dungeonKeys)];
    memcpy(localDungeonKeys, gSaveContext.save.saveInfo.inventory.dungeonKeys, sizeof(localDungeonKeys));
    s8 localFoundDungeonKeys[ARRAY_COUNT(gSaveContext.save.shipSaveInfo.rando.foundDungeonKeys)];
    memcpy(localFoundDungeonKeys, gSaveContext.save.shipSaveInfo.rando.foundDungeonKeys, sizeof(localFoundDungeonKeys));
    s8 localStrayFairies[ARRAY_COUNT(gSaveContext.save.saveInfo.inventory.strayFairies)];
    memcpy(localStrayFairies, gSaveContext.save.saveInfo.inventory.strayFairies, sizeof(localStrayFairies));
    u8 localDungeonItems[ARRAY_COUNT(gSaveContext.save.saveInfo.inventory.dungeonItems)];
    memcpy(localDungeonItems, gSaveContext.save.saveInfo.inventory.dungeonItems, sizeof(localDungeonItems));

    // Restore bottle contents (unless Deku Princess).
    for (int i = 0; i < 6; i++) {
        if (gSaveContext.save.saveInfo.inventory.items[SLOT_BOTTLE_1 + i] != ITEM_NONE &&
            gSaveContext.save.saveInfo.inventory.items[SLOT_BOTTLE_1 + i] != ITEM_DEKU_PRINCESS) {
            loadedData.saveInfo.inventory.items[SLOT_BOTTLE_1 + i] =
                gSaveContext.save.saveInfo.inventory.items[SLOT_BOTTLE_1 + i];
        }
    }
    // Restore non-zero ammo, except beans.
    for (int i = 0; i < ARRAY_COUNT(gSaveContext.save.saveInfo.inventory.ammo); i++) {
        if (gSaveContext.save.saveInfo.inventory.ammo[i] != 0 && i != SLOT(ITEM_MAGIC_BEANS)) {
            loadedData.saveInfo.inventory.ammo[i] = gSaveContext.save.saveInfo.inventory.ammo[i];
        }
    }
    // Restore receiver-local fields that shouldn't be synced.
    loadedData.saveInfo.checksum = gSaveContext.save.saveInfo.checksum;
    loadedData.shipSaveInfo.fileCreatedAt = gSaveContext.save.shipSaveInfo.fileCreatedAt;
    // ComboShip: taking a peer's saveType would flip IS_RANDO and silently unregister every rando hook.
    // Safe to preserve unconditionally: the guard above already refused a non-rando local save.
    loadedData.shipSaveInfo.saveType = gSaveContext.save.shipSaveInfo.saveType;
    memcpy(loadedData.saveInfo.playerData.newf, gSaveContext.save.saveInfo.playerData.newf,
           sizeof(loadedData.saveInfo.playerData.newf));
    memcpy(&loadedData.shipSaveInfo.dpadEquips, &gSaveContext.save.shipSaveInfo.dpadEquips,
           sizeof(loadedData.shipSaveInfo.dpadEquips));
    memcpy(loadedData.saveInfo.equips.cButtonSlots, gSaveContext.save.saveInfo.equips.cButtonSlots,
           sizeof(loadedData.saveInfo.equips.cButtonSlots));
    memcpy(loadedData.saveInfo.equips.buttonItems, gSaveContext.save.saveInfo.equips.buttonItems,
           sizeof(loadedData.saveInfo.equips.buttonItems));
    memcpy(loadedData.saveInfo.playerData.playerName, gSaveContext.save.saveInfo.playerData.playerName,
           sizeof(loadedData.saveInfo.playerData.playerName));
#ifdef COMBO_BUILD
    // Pending cross-world traps are receiver-local; a teammate's count must not clobber ours.
    loadedData.shipSaveInfo.rando.pendingTrapCount = gSaveContext.save.shipSaveInfo.rando.pendingTrapCount;
#endif

    // Commit only the two progression sub-structs; top-level Save fields (scene/entrance/time/day/
    // playerForm/cycle) are intentionally left untouched so the receiver isn't relocated.
    gSaveContext.save.saveInfo = loadedData.saveInfo;
    gSaveContext.save.shipSaveInfo = loadedData.shipSaveInfo;

    // Restore permanently-obtained checks the incoming state didn't have.
    for (int i = 0; i < RC_MAX; i++) {
        if (localObtained[i]) {
            RANDO_SAVE_CHECKS[i].obtained = true;
        }
    }

    // Finding 1: OR/max-merge permanent progress back in — resync can only ADD, never remove.
    for (int i = 0; i < 100; i++) {
        gSaveContext.save.saveInfo.weekEventReg[i] |= localWeekEventReg[i];
    }
    for (int i = 0; i < ARRAY_COUNT(gSaveContext.save.shipSaveInfo.rando.randoInf); i++) {
        gSaveContext.save.shipSaveInfo.rando.randoInf[i] |= localRandoInf[i];
    }
    for (int i = 0; i < 24; i++) {
        if (localMasks[i] != ITEM_NONE) {
            gSaveContext.save.saveInfo.inventory.items[24 + i] = localMasks[i];
        }
    }
    const u8 tradeSlots[3] = { SLOT_TRADE_DEED, SLOT_TRADE_KEY_MAMA, SLOT_TRADE_COUPLE };
    for (int i = 0; i < 3; i++) {
        if (localTradeItems[i] != ITEM_NONE) {
            gSaveContext.save.saveInfo.inventory.items[tradeSlots[i]] = localTradeItems[i];
        }
    }
    gSaveContext.save.saveInfo.inventory.questItems |= localQuestItems;
    for (int i = 0; i < 8; i++) {
        u32 mask = gUpgradeMasks[i];
        u8 shift = gUpgradeShifts[i];
        u32 localVal = (localUpgrades & mask) >> shift;
        u32 curVal = (gSaveContext.save.saveInfo.inventory.upgrades & mask) >> shift;
        if (localVal > curVal) {
            gSaveContext.save.saveInfo.inventory.upgrades =
                (gSaveContext.save.saveInfo.inventory.upgrades & ~mask) | (localVal << shift);
        }
    }
    if (localHealthCapacity > gSaveContext.save.saveInfo.playerData.healthCapacity) {
        gSaveContext.save.saveInfo.playerData.healthCapacity = localHealthCapacity;
    }
    gSaveContext.save.saveInfo.playerData.owlActivationFlags =
        static_cast<u16>(gSaveContext.save.saveInfo.playerData.owlActivationFlags | localOwlFlags);
    owlPollPrimed = false; // the merged statues are the new baseline, not something to announce
    // Bug 4/5: take the higher rupees/magic (magic only once a meter exists; don't set it barefoot).
    if (localRupees > gSaveContext.save.saveInfo.playerData.rupees) {
        gSaveContext.save.saveInfo.playerData.rupees = localRupees;
    }
    if (gSaveContext.save.saveInfo.playerData.magicLevel > 0 &&
        localMagic > gSaveContext.save.saveInfo.playerData.magic) {
        gSaveContext.save.saveInfo.playerData.magic = localMagic;
    }
    // Bug 5: OR permanent scene flags back — resync can only ADD explored/cleared state, never clear it.
    for (int i = 0; i < 120; i++) {
        PermanentSceneFlags& cur = gSaveContext.save.saveInfo.permanentSceneFlags[i];
        cur.chest |= localPermSceneFlags[i].chest;
        cur.switch0 |= localPermSceneFlags[i].switch0;
        cur.switch1 |= localPermSceneFlags[i].switch1;
        cur.clearedRoom |= localPermSceneFlags[i].clearedRoom;
        cur.collectible |= localPermSceneFlags[i].collectible;
        cur.unk_14 |= localPermSceneFlags[i].unk_14; // dungeon floors visited
        cur.rooms |= localPermSceneFlags[i].rooms;
    }
    // Block 3: the merge above only reaches the Save. The game works on cycleSceneFlags (loaded from
    // the Save once, copied back over it on every save) and on the live scene, so without this the
    // team's opened chests stayed closed here and were overwritten by our next save. Persistent bits
    // only: a teammate's cycle-bound flags must not leak into our cycle.
    for (int i = 0; i < SCENE_MAX; i++) {
        const PermanentSceneFlags& merged = gSaveContext.save.saveInfo.permanentSceneFlags[i];
        ApplyPersistentSceneBits(i, merged.chest, merged.switch0, merged.switch1, merged.collectible);
    }
    // Max/OR-merge the key + fairy + dungeon-item counters back (OOT precedent: UpdateTeamState.cpp).
    // Accepted: a stale peer resync can re-grant a locally-spent small key.
    for (int i = 0; i < ARRAY_COUNT(localDungeonKeys); i++) {
        if (localDungeonKeys[i] > gSaveContext.save.saveInfo.inventory.dungeonKeys[i]) {
            gSaveContext.save.saveInfo.inventory.dungeonKeys[i] = localDungeonKeys[i];
        }
    }
    for (int i = 0; i < ARRAY_COUNT(localFoundDungeonKeys); i++) {
        if (localFoundDungeonKeys[i] > gSaveContext.save.shipSaveInfo.rando.foundDungeonKeys[i]) {
            gSaveContext.save.shipSaveInfo.rando.foundDungeonKeys[i] = localFoundDungeonKeys[i];
        }
    }
    for (int i = 0; i < ARRAY_COUNT(localStrayFairies); i++) {
        if (localStrayFairies[i] > gSaveContext.save.saveInfo.inventory.strayFairies[i]) {
            gSaveContext.save.saveInfo.inventory.strayFairies[i] = localStrayFairies[i];
        }
    }
    for (int i = 0; i < ARRAY_COUNT(localDungeonItems); i++) {
        gSaveContext.save.saveInfo.inventory.dungeonItems[i] |= localDungeonItems[i];
    }
#ifdef COMBO_BUILD
    if (localTriforcePieces > gSaveContext.save.shipSaveInfo.rando.foundTriforcePieces) {
        gSaveContext.save.shipSaveInfo.rando.foundTriforcePieces = localTriforcePieces;
    }
#endif
    // top-level cycleSceneFlags (Save, not saveInfo) is intentionally NOT touched — it must keep
    // resetting on Song of Time.

    // ComboShip (bug 4): backfill checks the team obtained but we missed. Runs AFTER the flag/inventory
    // unions so item side-effects land on the merged base. localObtained[] was snapshotted pre-assign.
    for (int i = 0; i < RC_MAX; i++) {
        if (!loadedData.shipSaveInfo.rando.randoSaveChecks[i].obtained || localObtained[i]) {
            continue;
        }
        RandoCheckId rc = (RandoCheckId)i;
        // Native same-game items are recovered by the saveInfo union above; only foreign checks (item
        // belongs to OOT, absent from this snapshot) need cross-game delivery. Granting native here too
        // would double-credit additive items. Dedup-guarded; no broadcast.
        if (loadedData.shipSaveInfo.rando.randoSaveChecks[i].randoItemId != RI_COMBO_FOREIGN) {
            continue;
        }
        const ComboRando::ForeignItem* fi = Rando::MiscBehavior::MM_LookupForeign(rc);
        if (fi && gMMComboCrossDeliver) {
            std::string checkName = Rando::StaticData::GetCheckDisplayName(rc);
            gMMComboCrossDeliver((int)fi->itemGame, fi->itemName.c_str(), checkName.c_str());
        }
        // Mark obtained so the local player can't re-collect + double-deliver (idempotent next resync).
        RANDO_SAVE_CHECKS[rc].obtained = true;
        RANDO_SAVE_CHECKS[rc].cycleObtained = true;
        RANDO_SAVE_CHECKS[rc].eligible = false;
    }

    // ComboShip: dormant apply has no gPlayState/scene — CheckTracker/ActorBehavior/ShipInit re-derive
    // scene-bound state and aren't dormant-safe, and a backgrounded apply shouldn't toast. MM's own
    // OnSaveLoad re-requests a resync on activation, which re-runs this block in the foreground.
    if (isDormantApply) {
        dormantDidApply = true; // let PumpDormant persist; scene-bound re-derivation below is skipped
    } else {
        if (ComboAnchor_ShouldToastResync()) {
            Notification::Emit({
                .message = "Save updated from team",
            });
        }
        // Rando::MiscBehavior::OnFileLoad() is deliberately NOT called: it runs CheckQueueReset(), which
        // would drop in-flight grants. Preserving the local saveType above keeps its hooks valid anyway.
        Rando::CheckTracker::OnFileLoad();
        Rando::ActorBehavior::OnFileLoad();
        ShipInit::Init("IS_RANDO");
    }

    // ComboShip (#136): a teammate's pieces can cross the combined goal for us too — re-evaluate.
    if (gMMComboTriforceProgress != NULL) {
        gMMComboTriforceProgress(1, gSaveContext.fileNum);
    }
    // ComboShip: Shared Items — the inventory union above bypasses the grant path, so poke directly.
    if (gMMComboSharedChanged != NULL) {
        gMMComboSharedChanged(1, gSaveContext.fileNum);
    }

    // Replay any packets queued on the server while we were away, through the normal incoming path.
    ReplayTeamStateQueue(payload);
}

// MARK: - Same-game teleport (mirrors soh's REQUEST_TELEPORT -> TELEPORT_TO handshake)

// Gate mirrors soh's CanTeleportTo minus OOT's problematic-scene list (MM has none). TeleportMode is
// read from the shared CVar (owner-authored, process-global store both games read).
bool MMAnchor::CanTeleportTo(uint32_t clientId) {
    int teleportMode = CVarGetInteger(kCvarTeleportMode, 1);
    if (teleportMode == 0 || !IsSaveLoaded()) {
        return false;
    }
    auto it = clients.find(clientId);
    if (it == clients.end()) {
        return false;
    }
    MMAnchorClient& client = it->second;
    if (client.self || !client.online || !client.isSaveLoaded) {
        return false;
    }
    if (teleportMode == 1 && client.teamId != CVarGetString(kCvarTeamId, "default")) {
        return false;
    }
    return true;
}

void MMAnchor::SendPacket_RequestTeleport(uint32_t clientId) {
    if (!CanTeleportTo(clientId)) {
        return;
    }
    nlohmann::json payload;
    payload["type"] = PKT_REQUEST_TELEPORT;
    payload["targetClientId"] = clientId;
    SendJson(payload);
}

void MMAnchor::HandlePacket_RequestTeleport(const nlohmann::json& payload) {
    if (!IsSaveLoaded() || payload.value("targetClientId", (uint32_t)0) != ownClientId) {
        return; // only the requested target answers (server directs, but double-check)
    }
    uint32_t requester = payload.value("clientId", (uint32_t)0);
    if (requester != 0) {
        SendPacket_TeleportTo(requester);
    }
}

void MMAnchor::SendPacket_TeleportTo(uint32_t clientId) {
    if (!IsSaveLoaded()) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    nlohmann::json payload;
    payload["type"] = PKT_TELEPORT_TO;
    payload["targetClientId"] = clientId;
    payload["entranceId"] = gSaveContext.save.entrance;
    payload["roomNum"] = (int32_t)gPlayState->roomCtx.curRoom.num;
    payload["pos"] = player->actor.world.pos; // Vec3f via BenJsonConversions
    payload["rotY"] = (int32_t)player->actor.shape.rot.y;
    SendJson(payload);
}

void MMAnchor::HandlePacket_TeleportTo(const nlohmann::json& payload) {
    if (!IsSaveLoaded() || payload.value("targetClientId", (uint32_t)0) != ownClientId) {
        return;
    }
    s32 entranceId = payload.value("entranceId", (s32)-1);
    s8 roomNum = (s8)payload.value("roomNum", (int32_t)-1);
    if (entranceId < 0 || roomNum < 0) {
        return;
    }
    Vec3f pos = payload.contains("pos") ? payload["pos"].get<Vec3f>() : Vec3f{ 0, 0, 0 };
    s16 rotY = (s16)payload.value("rotY", (int32_t)0);
    // Warp mirrors DeveloperTools/WarpPoint.cpp Warp() (gPlayState != NULL branch).
    gPlayState->nextEntrance = Entrance_Create(entranceId >> 9, 0, entranceId & 0xF);
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_INSTANT;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].entrance = Entrance_Create(entranceId >> 9, 0, entranceId & 0xF);
    gSaveContext.respawn[RESPAWN_MODE_DOWN].roomIndex = roomNum;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].pos = pos;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].yaw = rotY;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = PLAYER_PARAMS(0xFF, PLAYER_START_MODE_D);
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
    gSaveContext.respawnFlag = -8;
}

// MARK: - Flag sync (co-op next block 3, 2026-11-14)
//
// What is shared, and why only that, is described in combo/net/ComboAnchorMmFlags.h: the flags the
// game itself keeps across a Song of Time reset. Set-only, so a replay (outbox, server queue) is
// harmless and nothing here can undo a teammate's progress.
//
// Sending: the game's own flag hooks (OnSceneFlagSet / OnFlagSet fire on a real off -> on change) and
// a per-frame poll for owl statues, which have no hook.
// Receiving: written straight into the save, never through Flags_Set*: the rando marks a check
// "eligible" from those hooks and would grant the teammate's check a second time here. The item
// itself arrives through GIVE_ITEM, which also marks the check obtained.

// The game's persistence tables (mm/src/code/z_sram_NES.c). They are plain globals there but have no
// header, so they are declared here with the same layout; no vendored edit needed.
extern "C" {
typedef struct PersistentCycleSceneFlags {
    /* 0x0 */ u32 switch0;
    /* 0x4 */ u32 switch1;
    /* 0x8 */ u32 chest;
    /* 0xC */ u32 collectible;
} PersistentCycleSceneFlags; // size = 0x10
extern PersistentCycleSceneFlags sPersistentCycleSceneFlags[SCENE_MAX];
extern u16 sPersistentCycleWeekEventRegs[ARRAY_COUNT(gSaveContext.save.saveInfo.weekEventReg)];
}

// Collectible flags of the Inverted Stone Tower Temple are stored under its own scene id; everything
// else under the "original" scene (Play_SaveCycleSceneFlags / Actor_InitContext).
static int MmFlagSceneIndex(s16 sceneId, bool collectible) {
    if (collectible && sceneId == SCENE_INISIE_R) {
        return sceneId;
    }
    return Play_GetOriginalSceneId(sceneId);
}

bool MMAnchor::CanSendProgress() {
    return isActive && roomState.syncItemsAndFlags && IsSaveLoaded() && HasLoadedRandoSave();
}

void MMAnchor::OnLocalSceneFlagSet(s16 sceneId, FlagType flagType, u32 flag) {
    if (!CanSendProgress() || sceneId < 0 || sceneId >= SCENE_MAX || flag > 127) {
        return;
    }
    const int flagId = static_cast<int>(flag);
    u32 bit = 0;
    if (flagType == FLAG_CYCL_SCENE_CHEST) {
        const int idx = MmFlagSceneIndex(sceneId, false);
        if (ComboAnchorCore::ChestFlagBit(flagId, bit) && (sPersistentCycleSceneFlags[idx].chest & bit) != 0) {
            SendPacket_MmFlag(ComboAnchorCore::kMmFlagChest, idx, flagId);
        }
    } else if (flagType == FLAG_CYCL_SCENE_SWITCH) {
        const int idx = MmFlagSceneIndex(sceneId, false);
        int word = 0;
        if (ComboAnchorCore::SwitchFlagBit(flagId, word, bit)) {
            const PersistentCycleSceneFlags& keep = sPersistentCycleSceneFlags[idx];
            if (((word == 0 ? keep.switch0 : keep.switch1) & bit) != 0) {
                SendPacket_MmFlag(ComboAnchorCore::kMmFlagSwitch, idx, flagId);
            }
        }
    } else if (flagType == FLAG_CYCL_SCENE_COLLECTIBLE) {
        const int idx = MmFlagSceneIndex(sceneId, true);
        if (ComboAnchorCore::CollectibleFlagBit(flagId, bit) &&
            (sPersistentCycleSceneFlags[idx].collectible & bit) != 0) {
            SendPacket_MmFlag(ComboAnchorCore::kMmFlagCollectible, idx, flagId);
        }
    }
    // FLAG_CYCL_SCENE_CLEARED_ROOM: reset with every cycle, never shared.
}

void MMAnchor::OnLocalWeekEventSet(u32 flag) {
    if (!CanSendProgress()) {
        return;
    }
    const u32 index = flag >> 8;
    if (index >= ARRAY_COUNT(gSaveContext.save.saveInfo.weekEventReg)) {
        return;
    }
    const u8 persistent = ComboAnchorCore::WeekEventPersistMask(sPersistentCycleWeekEventRegs[index]);
    if ((flag & 0xFF & persistent) == 0) {
        return; // a cycle-bound flag: this player's three days only
    }
    SendPacket_MmFlag(ComboAnchorCore::kMmFlagWeekEventReg, 0, static_cast<int>(flag));
}

// Owl statue activation is a bare macro in the game (SET_OWL_STATUE_ACTIVATED), so there is nothing to
// hook. Compare against the last announced value once per frame instead. The baseline is retaken
// (nothing announced) whenever no save is in play, after a team-state merge and after a remote flag.
void MMAnchor::PollOwlStatues() {
    if (!IsSaveLoaded() || !HasLoadedRandoSave()) {
        owlPollPrimed = false;
        return;
    }
    const u16 statueMask = static_cast<u16>((1 << (OWL_WARP_STONE_TOWER + 1)) - 1);
    const u16 current = static_cast<u16>(gSaveContext.save.saveInfo.playerData.owlActivationFlags & statueMask);
    if (!owlPollPrimed) {
        lastOwlFlags = current;
        owlPollPrimed = true;
        return;
    }
    const u16 fresh = static_cast<u16>(current & ~lastOwlFlags);
    lastOwlFlags = current;
    if (fresh == 0 || !roomState.syncItemsAndFlags) {
        return;
    }
    for (int owl = 0; owl <= OWL_WARP_STONE_TOWER; owl++) {
        if ((fresh & (1 << owl)) != 0) {
            SendPacket_MmFlag(ComboAnchorCore::kMmFlagOwl, 0, owl);
        }
    }
}

void MMAnchor::SendPacket_MmFlag(const char* kind, int scene, int flag) {
    nlohmann::json payload;
    payload["type"] = PKT_MM_FLAG;
    payload["targetTeamId"] = CVarGetString(kCvarTeamId, "default");
    payload["addToQueue"] = true; // set-only and idempotent, so a server-queue replay is safe
    payload["kind"] = kind;
    payload["scene"] = scene;
    payload["flag"] = flag;
    payload["quiet"] = true;
    SendJson(payload);
}

// OR `bits` into `word`; true if that changed it.
static bool MmOrBits(u32& word, u32 bits) {
    if ((word | bits) == word) {
        return false;
    }
    word |= bits;
    return true;
}

bool MMAnchor::ApplyPersistentSceneBits(int sceneIdx, u32 chest, u32 switch0, u32 switch1, u32 collectible) {
    if (sceneIdx < 0 || sceneIdx >= SCENE_MAX) {
        return false;
    }
    const PersistentCycleSceneFlags& keep = sPersistentCycleSceneFlags[sceneIdx];
    chest &= keep.chest;
    switch0 &= keep.switch0;
    switch1 &= keep.switch1;
    collectible &= keep.collectible;
    if ((chest | switch0 | switch1 | collectible) == 0) {
        return false;
    }

    bool changed = false;
    // The working copy the game loads a scene from, and the Save it is copied into on every save.
    CycleSceneFlags& cycle = gSaveContext.cycleSceneFlags[sceneIdx];
    PermanentSceneFlags& perm = gSaveContext.save.saveInfo.permanentSceneFlags[sceneIdx];
    changed |= MmOrBits(cycle.chest, chest);
    changed |= MmOrBits(cycle.switch0, switch0);
    changed |= MmOrBits(cycle.switch1, switch1);
    changed |= MmOrBits(cycle.collectible, collectible);
    changed |= MmOrBits(perm.chest, chest);
    changed |= MmOrBits(perm.switch0, switch0);
    changed |= MmOrBits(perm.switch1, switch1);
    changed |= MmOrBits(perm.collectible, collectible);

    // Foreground: the scene the player stands in works on its own copy, which is written back over
    // cycleSceneFlags when the scene is left. Dormant MM has no play state.
    if (!isDormantApply && gPlayState != nullptr) {
        ActorContextSceneFlags& live = gPlayState->actorCtx.sceneFlags;
        if (MmFlagSceneIndex(gPlayState->sceneId, false) == sceneIdx) {
            changed |= MmOrBits(live.chest, chest);
            changed |= MmOrBits(live.switches[0], switch0);
            changed |= MmOrBits(live.switches[1], switch1);
        }
        if (MmFlagSceneIndex(gPlayState->sceneId, true) == sceneIdx) {
            changed |= MmOrBits(live.collectible[0], collectible);
        }
    }
    return changed;
}

void MMAnchor::OverlayPersistentSceneFlags(Save& out) {
    for (int i = 0; i < SCENE_MAX; i++) {
        const PersistentCycleSceneFlags& keep = sPersistentCycleSceneFlags[i];
        const CycleSceneFlags& cycle = gSaveContext.cycleSceneFlags[i];
        PermanentSceneFlags& perm = out.saveInfo.permanentSceneFlags[i];
        perm.chest |= cycle.chest & keep.chest;
        perm.switch0 |= cycle.switch0 & keep.switch0;
        perm.switch1 |= cycle.switch1 & keep.switch1;
        perm.collectible |= cycle.collectible & keep.collectible;
    }
    if (gPlayState == nullptr || GET_PLAYER(gPlayState) == nullptr) {
        return; // dormant: the working copy is all there is
    }
    const ActorContextSceneFlags& live = gPlayState->actorCtx.sceneFlags;
    const int idx = MmFlagSceneIndex(gPlayState->sceneId, false);
    if (idx >= 0 && idx < SCENE_MAX) {
        PermanentSceneFlags& perm = out.saveInfo.permanentSceneFlags[idx];
        perm.chest |= live.chest & sPersistentCycleSceneFlags[idx].chest;
        perm.switch0 |= live.switches[0] & sPersistentCycleSceneFlags[idx].switch0;
        perm.switch1 |= live.switches[1] & sPersistentCycleSceneFlags[idx].switch1;
    }
    const int collectIdx = MmFlagSceneIndex(gPlayState->sceneId, true);
    if (collectIdx >= 0 && collectIdx < SCENE_MAX) {
        out.saveInfo.permanentSceneFlags[collectIdx].collectible |=
            live.collectible[0] & sPersistentCycleSceneFlags[collectIdx].collectible;
    }
}

// Foreground and dormant (PumpDormant sets isDormantApply around it). Every value is re-validated
// here against this client's own persistence tables: a packet can never set a cycle-bound flag.
void MMAnchor::HandlePacket_MmFlag(const nlohmann::json& payload) {
    if (!roomState.syncItemsAndFlags || !HasLoadedRandoSave()) {
        return;
    }
    const uint32_t clientId = payload.value("clientId", (uint32_t)0);
    if (clientId != 0 && clientId == ownClientId) {
        return; // our own broadcast
    }
    if (payload.value("targetTeamId", std::string("default")) != CVarGetString(kCvarTeamId, "default")) {
        return;
    }
    const std::string kind = payload.value("kind", std::string());
    const int scene = payload.value("scene", -1);
    const int flag = payload.value("flag", -1);
    bool changed = false;
    u32 bit = 0;

    if (kind == ComboAnchorCore::kMmFlagChest) {
        if (ComboAnchorCore::ChestFlagBit(flag, bit)) {
            changed = ApplyPersistentSceneBits(scene, bit, 0, 0, 0);
        }
    } else if (kind == ComboAnchorCore::kMmFlagSwitch) {
        int word = 0;
        if (ComboAnchorCore::SwitchFlagBit(flag, word, bit)) {
            changed = word == 0 ? ApplyPersistentSceneBits(scene, 0, bit, 0, 0)
                                : ApplyPersistentSceneBits(scene, 0, 0, bit, 0);
        }
    } else if (kind == ComboAnchorCore::kMmFlagCollectible) {
        if (ComboAnchorCore::CollectibleFlagBit(flag, bit)) {
            changed = ApplyPersistentSceneBits(scene, 0, 0, 0, bit);
        }
    } else if (kind == ComboAnchorCore::kMmFlagWeekEventReg) {
        const int index = flag >> 8;
        if (flag >= 0 && index < (int)ARRAY_COUNT(gSaveContext.save.saveInfo.weekEventReg)) {
            const u8 persistent = ComboAnchorCore::WeekEventPersistMask(sPersistentCycleWeekEventRegs[index]);
            const u8 bits = static_cast<u8>(flag & 0xFF & persistent);
            u8& reg = gSaveContext.save.saveInfo.weekEventReg[index];
            if (bits != 0 && (reg & bits) != bits) {
                reg = static_cast<u8>(reg | bits);
                changed = true;
            }
        }
    } else if (kind == ComboAnchorCore::kMmFlagOwl) {
        if (flag >= 0 && flag <= OWL_WARP_STONE_TOWER) {
            u16& owls = gSaveContext.save.saveInfo.playerData.owlActivationFlags;
            const u16 owlBit = static_cast<u16>(1 << flag);
            if ((owls & owlBit) == 0) {
                owls = static_cast<u16>(owls | owlBit);
                changed = true;
            }
            owlPollPrimed = false; // new baseline: don't announce a teammate's statue as ours
        }
    }

    if (changed && isDormantApply) {
        dormantDidApply = true; // PumpDormant persists once after draining its queue
    }
}

// MARK: - Launcher-facing C ABI (mirrors soh's SOH_Anchor_* exports)

extern "C" COMBO_EXPORT void MM_SetAnchorSend(void (*cb)(const char*)) {
    gMMComboAnchorSend = cb;
    // Create the adapter now (launcher startup, pre-connect). It used to be created on first
    // Activate, so a client that never entered MM had no instance and RecvJson/PumpDormant dropped
    // every packet — teammate MM items never reached the dormant MM save.
    if (MMAnchor::Instance == nullptr) {
        MMAnchor::Instance = new MMAnchor();
    }
}

// A6: launcher registers its per-frame dormant-pump fn; MM calls it each active frame (see hook).
extern "C" COMBO_EXPORT void MM_SetPumpDormant(void (*cb)()) {
    gMMComboPumpDormant = cb;
}

// A6: launcher calls this (on the active sibling's thread) when MM is the dormant game.
extern "C" COMBO_EXPORT void MM_Anchor_PumpDormant(void) {
    if (MMAnchor::Instance) {
        MMAnchor::Instance->PumpDormant();
    }
}

// Bug 2: launcher-orchestrated resync (auto on connect + combo menu button), dormant-safe.
// Finding 3: never let an exception unwind across this extern "C" boundary.
extern "C" COMBO_EXPORT void MM_Anchor_RequestResync(void) {
    try {
        if (MMAnchor::Instance) {
            MMAnchor::Instance->RequestResyncDormantSafe();
        }
    } catch (const std::exception& e) { SPDLOG_ERROR("[MM_Anchor_RequestResync] {}", e.what()); } catch (...) {
        SPDLOG_ERROR("[MM_Anchor_RequestResync] unknown exception");
    }
}

// Co-op hardening (2026-11-13): launcher link/presence toasts. Callable from the launcher's Anchor
// thread (EmitThreadSafe queues it for the UI frame). gateCvar: a default-on display toggle, or null.
extern "C" COMBO_EXPORT void MM_ComboToast(const char* prefix, const char* message, const char* suffix,
                                           const char* gateCvar) {
    try {
        if (gateCvar && gateCvar[0] != '\0' && !CVarGetInteger(gateCvar, 1)) {
            return;
        }
        Notification::EmitThreadSafe(
            { .prefix = prefix ? prefix : "", .message = message ? message : "", .suffix = suffix ? suffix : "" });
    } catch (...) {} // never unwind into the launcher
}

extern "C" COMBO_EXPORT void MM_Anchor_RecvJson(const char* json) {
    if (MMAnchor::Instance && json) {
        MMAnchor::Instance->OnIncomingJson(json);
    }
}

extern "C" COMBO_EXPORT void MM_Anchor_Activate(void) {
    if (MMAnchor::Instance == nullptr) {
        MMAnchor::Instance = new MMAnchor();
    }
    MMAnchor::Instance->Activate();
}

extern "C" COMBO_EXPORT void MM_Anchor_Deactivate(void) {
    if (MMAnchor::Instance) {
        MMAnchor::Instance->Deactivate();
    }
}

// ComboShip: stateless MM scene-name lookup for the combo room window. The launcher owns the roster
// now; comboui resolves each MM peer's area name from its raw scene id via this (works while dormant).
extern "C" COMBO_EXPORT const char* MM_Anchor_ResolveScene(int rawScene) {
    static std::string cached;
    cached = Ship_GetSceneName((s16)rawScene);
    return cached.c_str();
}

// Same-game teleport trigger for the combo room window (MM active + MM peer). Wraps
// SendPacket_RequestTeleport, which re-validates via CanTeleportTo and no-ops if disallowed.
extern "C" COMBO_EXPORT void MM_Anchor_RequestTeleport(uint32_t clientId) {
    try {
        if (MMAnchor::Instance) {
            MMAnchor::Instance->SendPacket_RequestTeleport(clientId);
        }
    } catch (const std::exception& e) { SPDLOG_ERROR("[MM_Anchor_RequestTeleport] {}", e.what()); } catch (...) {
        SPDLOG_ERROR("[MM_Anchor_RequestTeleport] unknown exception");
    }
}

#endif // COMBO_BUILD
