#include <nlohmann/json.hpp>

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Enhancements/game-interactor/GameInteractionEffect.h"
#ifdef COMBO_BUILD
#include "soh/Enhancements/game-interactor/GameInteractor.h" // ComboShip: RawAction for the dormant path
#endif

extern "C" {
#include "functions.h"
#include "soh/Enhancements/randomizer/ShuffleTradeItems.h"
extern PlayState* gPlayState;
}

/**
 * UNSET_FLAG
 *
 * Fired when a flag is unset in the save context
 */

void Anchor::SendPacket_UnsetFlag(s16 sceneNum, s16 flagType, s16 flag) {
    if (!IsSaveLoaded() || !roomState.syncItemsAndFlags) {
        return;
    }
#ifdef COMBO_BUILD
    // ComboShip: loop prevention — don't echo unsets caused by applying a remote packet (see SetFlag.cpp).
    if (isProcessingIncomingPacket) {
        return;
    }
#endif

    nlohmann::json payload;
    payload["type"] = UNSET_FLAG;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["addToQueue"] = true;
    payload["sceneNum"] = sceneNum;
    payload["flagType"] = flagType;
    payload["flag"] = flag;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_UnsetFlag(nlohmann::json payload) {
    if (!IsSaveLoaded() || !roomState.syncItemsAndFlags) {
        return;
    }

    s16 sceneNum = payload.at("sceneNum").get<s16>();
    s16 flagType = payload.at("flagType").get<s16>();
    s16 flag = payload.at("flag").get<s16>();

    // sceneNum == SCENE_ID_MAX is a sentinel meaning "global flag" (handled below); only larger
    // values would index gSaveContext.sceneFlags out of bounds.
    if (sceneNum < 0 || sceneNum > SCENE_ID_MAX) {
        SPDLOG_ERROR("[Anchor] UNSET_FLAG: sceneNum {} out of range", sceneNum);
        return;
    }

    if (sceneNum == SCENE_ID_MAX) {
#ifdef COMBO_BUILD
        // ComboShip: background world sync — direct save write while dormant (see SetFlag.cpp).
        if (isDormantApply) {
            GameInteractor::RawAction::UnsetFlag(flagType, flag);
        } else
#endif
        {
            auto effect = new GameInteractionEffect::UnsetFlag();
            effect->parameters[0] = flagType;
            effect->parameters[1] = flag;
            effect->Apply();
#ifdef COMBO_BUILD
            delete effect; // ComboShip: Apply() is synchronous; upstream leaked the effect per packet.
#endif
        }

        // Special case: If an adult trade item flag is unset, replace the item if the player has it equipped
        if (flagType == FLAG_RANDOMIZER_INF &&
            (flag >= RAND_INF_ADULT_TRADES_HAS_POCKET_EGG && flag <= RAND_INF_ADULT_TRADES_HAS_CLAIM_CHECK)) {
            u16 itemToReplace = ITEM_POCKET_EGG;
            switch (flag) {
                case RAND_INF_ADULT_TRADES_HAS_POCKET_EGG:
                    itemToReplace = ITEM_POCKET_EGG;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_POCKET_CUCCO:
                    itemToReplace = ITEM_POCKET_CUCCO;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_COJIRO:
                    itemToReplace = ITEM_COJIRO;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_ODD_MUSHROOM:
                    itemToReplace = ITEM_ODD_MUSHROOM;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_ODD_POTION:
                    itemToReplace = ITEM_ODD_POTION;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_SAW:
                    itemToReplace = ITEM_SAW;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_SWORD_BROKEN:
                    itemToReplace = ITEM_SWORD_BROKEN;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_PRESCRIPTION:
                    itemToReplace = ITEM_PRESCRIPTION;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_FROG:
                    itemToReplace = ITEM_FROG;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_EYEDROPS:
                    itemToReplace = ITEM_EYEDROPS;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_CLAIM_CHECK:
                    itemToReplace = ITEM_CLAIM_CHECK;
                    break;
            }
#ifdef COMBO_BUILD
            // ComboShip: no play while dormant, so swap without the HUD icon reload.
            if (isDormantApply) {
                Anchor_DormantReplaceItem(itemToReplace, Randomizer_GetNextAdultTradeItem());
                return;
            }
#endif
            Inventory_ReplaceItem(gPlayState, itemToReplace, Randomizer_GetNextAdultTradeItem());
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

#ifdef COMBO_BUILD
        // ComboShip: background world sync — RawAction::UnsetSceneFlag reads gPlayState (see SetFlag.cpp).
        if (isDormantApply) {
            Anchor_DormantWriteSceneFlag(sceneNum, flagType, flag, false);
            return;
        }
#endif
        auto effect = new GameInteractionEffect::UnsetSceneFlag();
        effect->parameters[0] = sceneNum;
        effect->parameters[1] = flagType;
        effect->parameters[2] = flag;
        effect->Apply();
#ifdef COMBO_BUILD
        delete effect; // ComboShip: see above.
#endif
    }
}
