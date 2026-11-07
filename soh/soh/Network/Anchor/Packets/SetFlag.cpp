#include <nlohmann/json.hpp>

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Enhancements/game-interactor/GameInteractionEffect.h"

extern "C" {
#include "functions.h"
extern PlayState* gPlayState;
}

#ifdef COMBO_BUILD
#include "soh/Enhancements/game-interactor/GameInteractor.h"
extern "C" {
#include "include/macros.h" // ARRAY_COUNT
}

// ComboShip: background world sync (2026-11-07) — dormant-safe save writes, used by SET_FLAG/UNSET_FLAG
// while OOT is backgrounded (declared in Anchor.h). Mirrors the save half of
// GameInteractor::RawAction::SetSceneFlag/UnsetSceneFlag: switch flags >= 0x20 and collectible flag 0
// are temp/unsaved there too, so they are skipped here.
void Anchor_DormantWriteSceneFlag(s16 sceneNum, s16 flagType, s16 flag, bool set) {
    if (sceneNum < 0 || sceneNum >= SCENE_ID_MAX || flag < 0 || flag >= 0x20) {
        return;
    }
    u32* field = nullptr;
    switch (flagType) {
        case FLAG_SCENE_SWITCH:
            field = &gSaveContext.sceneFlags[sceneNum].swch;
            break;
        case FLAG_SCENE_CLEAR:
            field = &gSaveContext.sceneFlags[sceneNum].clear;
            break;
        case FLAG_SCENE_TREASURE:
            field = &gSaveContext.sceneFlags[sceneNum].chest;
            break;
        case FLAG_SCENE_COLLECTIBLE:
            if (flag == 0) {
                return;
            }
            field = &gSaveContext.sceneFlags[sceneNum].collect;
            break;
        default:
            return;
    }
    if (set) {
        *field |= (1u << flag);
    } else {
        *field &= ~(1u << flag);
    }
}

// Inventory_ReplaceItem minus its Interface_LoadItemIcon1(play, ...) — play is null while dormant, and
// the C-button icons are reloaded on the next scene load anyway.
void Anchor_DormantReplaceItem(u16 oldItem, u16 newItem) {
    for (s32 i = 0; i < ARRAY_COUNT(gSaveContext.inventory.items); i++) {
        if (gSaveContext.inventory.items[i] == oldItem) {
            gSaveContext.inventory.items[i] = (u8)newItem;
            for (s32 b = 1; b < ARRAY_COUNT(gSaveContext.equips.buttonItems); b++) {
                if (gSaveContext.equips.buttonItems[b] == oldItem) {
                    gSaveContext.equips.buttonItems[b] = (u8)newItem;
                    break;
                }
            }
            return;
        }
    }
}
#endif

/**
 * SET_FLAG
 *
 * Fired when a flag is set in the save context
 */

void Anchor::SendPacket_SetFlag(s16 sceneNum, s16 flagType, s16 flag) {
    if (!IsSaveLoaded() || !roomState.syncItemsAndFlags) {
        return;
    }
#ifdef COMBO_BUILD
    // ComboShip: loop prevention. A flag set as a side effect of applying a remote packet (e.g.
    // Randomizer_Item_Give setting a RAND_INF inside HandlePacket_GiveItem) fires OnFlagSet, which
    // would echo it back to the team. Every teammate already derives it from the same packet, and the
    // original sender broadcasts it from its own local grant — same guard GIVE_ITEM/ENTRANCE_DISCOVERED use.
    if (isProcessingIncomingPacket) {
        return;
    }
#endif

    nlohmann::json payload;
    payload["type"] = SET_FLAG;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["addToQueue"] = true;
    payload["sceneNum"] = sceneNum;
    payload["flagType"] = flagType;
    payload["flag"] = flag;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_SetFlag(nlohmann::json payload) {
    if (!IsSaveLoaded() || !roomState.syncItemsAndFlags) {
        return;
    }

    s16 sceneNum = payload.at("sceneNum").get<s16>();
    s16 flagType = payload.at("flagType").get<s16>();
    s16 flag = payload.at("flag").get<s16>();

    // sceneNum == SCENE_ID_MAX is a sentinel meaning "global flag" (handled below); only larger
    // values would index gSaveContext.sceneFlags out of bounds.
    if (sceneNum < 0 || sceneNum > SCENE_ID_MAX) {
        SPDLOG_ERROR("[Anchor] SET_FLAG: sceneNum {} out of range", sceneNum);
        return;
    }

    if (sceneNum == SCENE_ID_MAX) {
#ifdef COMBO_BUILD
        // ComboShip: background world sync — the effect refuses without gPlayState, so write the save
        // directly (RawAction::SetFlag touches only gSaveContext) and skip the HUD icon reload.
        if (isDormantApply) {
            GameInteractor::RawAction::SetFlag(flagType, flag);
            if (flagType == FLAG_EVENT_CHECK_INF && flag == EVENTCHKINF_KING_ZORA_MOVED &&
                Inventory_HasSpecificBottle(ITEM_LETTER_RUTO)) {
                Anchor_DormantReplaceItem(ITEM_LETTER_RUTO, ITEM_BOTTLE);
            }
            return;
        }
#endif
        auto effect = new GameInteractionEffect::SetFlag();
        effect->parameters[0] = flagType;
        effect->parameters[1] = flag;
        effect->Apply();
#ifdef COMBO_BUILD
        delete effect; // ComboShip: Apply() is synchronous and nothing keeps the effect; upstream leaked it per packet.
#endif

        // Special case: If King Zora moved, and the player has Ruto's Letter, convert it to an empty bottle
        if (flagType == FLAG_EVENT_CHECK_INF && flag == EVENTCHKINF_KING_ZORA_MOVED &&
            Inventory_HasSpecificBottle(ITEM_LETTER_RUTO)) {
            Inventory_ReplaceItem(gPlayState, ITEM_LETTER_RUTO, ITEM_BOTTLE);
        }
    } else {
        // Special case: Ignore water temple water level flags, stored at 0x1C, 0x1D, 0x1E.
        if (sceneNum == SCENE_WATER_TEMPLE && flagType == FLAG_SCENE_SWITCH &&
            (flag == 0x1C || flag == 0x1D || flag == 0x1E)) {
            return;
        }

        // Special case: Ignore forest temple elevator flag, stored at 0x1B.
        if (sceneNum == SCENE_FOREST_TEMPLE && flagType == FLAG_SCENE_SWITCH && flag == 0x1B) {
            return;
        }

        // Special case: Ignore tower collapse timer start, stored 0x36.
        if (sceneNum == SCENE_GANONS_TOWER_COLLAPSE_EXTERIOR && flagType == FLAG_SCENE_SWITCH && flag == 0x36) {
            return;
        }

        // Special case: Ignore Great Fairy cutscenes, stored 0x38.
        if ((sceneNum == SCENE_GREAT_FAIRYS_FOUNTAIN_MAGIC || sceneNum == SCENE_GREAT_FAIRYS_FOUNTAIN_SPELLS) &&
            flagType == FLAG_SCENE_SWITCH && flag == 0x38) {
            return;
        }

#ifdef COMBO_BUILD
        // ComboShip: background world sync — RawAction::SetSceneFlag reads gPlayState->sceneNum (null
        // while dormant). The save copy is authoritative while OOT is backgrounded, so write it directly.
        if (isDormantApply) {
            Anchor_DormantWriteSceneFlag(sceneNum, flagType, flag, true);
            return;
        }
#endif
        auto effect = new GameInteractionEffect::SetSceneFlag();
        effect->parameters[0] = sceneNum;
        effect->parameters[1] = flagType;
        effect->parameters[2] = flag;
        effect->Apply();
#ifdef COMBO_BUILD
        delete effect; // ComboShip: see above.
#endif
    }
}
