#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include "soh/OTRGlobals.h"
#include "soh/Enhancements/randomizer/SeedContext.h"
#ifdef COMBO_BUILD
#include "soh/Notification/Notification.h" // ComboShip: game-switch toast
#endif

extern "C" {
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * UPDATE_CLIENT_STATE
 *
 * Contains a small subset of data that is cached on the server and important for the client to know for various reasons
 *
 * Sent on various events, such as changing scenes, soft resetting, finishing the game, opening file select, etc.
 *
 * Note: This packet should be cross version compatible, so if you add anything here don't assume all clients will be
 * providing it, consider doing a `contains` check before accessing any version specific data
 */

nlohmann::json Anchor::PrepClientState() {
    nlohmann::json payload;
    payload["name"] = CVarGetString(CVAR_REMOTE_ANCHOR("Name"), "");
    payload["color"] = CVarGetColor24(CVAR_REMOTE_ANCHOR("Color.Value"), { 100, 255, 100 });
    payload["clientVersion"] = clientVersion;
    payload["teamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["online"] = true;

    if (IsSaveLoaded()) {
        payload["seed"] = IS_RANDO ? Rando::Context::GetInstance()->GetSeed() : 0;
        payload["isSaveLoaded"] = true;
        payload["isGameComplete"] = gSaveContext.ship.stats.gameComplete;
        payload["sceneNum"] = gPlayState->sceneNum;
        payload["curRoomNum"] = gPlayState->roomCtx.curRoom.num;
        payload["entranceIndex"] = gSaveContext.entranceIndex;
    } else {
        payload["seed"] = 0;
        payload["isSaveLoaded"] = false;
        payload["isGameComplete"] = false;
        payload["sceneNum"] = SCENE_ID_MAX;
        payload["curRoomNum"] = -1;
        payload["entranceIndex"] = 0x00;
    }

    return payload;
}

void Anchor::SendPacket_UpdateClientState() {
    nlohmann::json payload;
    payload["type"] = UPDATE_CLIENT_STATE;
    payload["state"] = PrepClientState();

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_UpdateClientState(nlohmann::json payload) {
    uint32_t clientId = payload.at("clientId").get<uint32_t>();

    if (clients.contains(clientId)) {
        AnchorClient client = payload["state"].get<AnchorClient>();
#ifdef COMBO_BUILD
        // ComboShip: game-switch toast (2026-11-07). A peer's two games share one clientId; MM reports a
        // namespaced sceneNum (>= 1000, MMAnchor's MM_ANCHOR_SCENE_NAMESPACE), so a flip across 1000
        // between two loaded states means the teammate crossed between the games. The first state we see
        // (ALL_CLIENT_STATE) only seeds the value, so connecting never toasts.
        {
            const AnchorClient& prev = clients[clientId];
            bool wasMM = prev.sceneNum >= 1000;
            bool isMM = client.sceneNum >= 1000;
            if (!prev.self && clientId != ownClientId && prev.isSaveLoaded && client.isSaveLoaded && wasMM != isMM &&
                client.teamId == CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default") &&
                CVarGetInteger(CVAR_REMOTE_ANCHOR("PartnerFeed.GameSwitch"), 1)) {
                Notification::Emit({
                    .prefix = client.name,
                    .message = "is now in",
                    .suffix = isMM ? "Majora's Mask" : "Ocarina of Time",
                });
            }
        }
#endif
        clients[clientId].clientId = clientId;
        clients[clientId].name = client.name;
        clients[clientId].color = client.color;
        clients[clientId].clientVersion = client.clientVersion;
        clients[clientId].teamId = client.teamId;
        clients[clientId].online = client.online;
        clients[clientId].seed = client.seed;
        clients[clientId].isSaveLoaded = client.isSaveLoaded;
        clients[clientId].isGameComplete = client.isGameComplete;
        clients[clientId].sceneNum = client.sceneNum;
        clients[clientId].curRoomNum = client.curRoomNum;
        clients[clientId].entranceIndex = client.entranceIndex;
    }
}
