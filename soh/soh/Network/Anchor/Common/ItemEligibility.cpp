#include "ItemEligibility.h"

#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
}

namespace ItemEligibility {

bool CanPlayerCollectItem00(s16 item00Type, bool walletCapAware) {
    switch (item00Type) {
        // Rupees — capacity-capped at wallet level. AI Player Follower (#172) leaves
        // these always-collect because vanilla truncates surplus silently and
        // the follower acts in the local player's stead. MP item-drop sync
        // (#193) gates on wallet to keep capped surplus available for a
        // teammate.
        case ITEM00_RUPEE_GREEN:
        case ITEM00_RUPEE_BLUE:
        case ITEM00_RUPEE_RED:
        case ITEM00_RUPEE_ORANGE:
        case ITEM00_RUPEE_PURPLE:
            if (walletCapAware) {
                return (s16)gSaveContext.rupees < CUR_CAPACITY(UPG_WALLET);
            }
            return true;

        // Need-gated recovery.
        case ITEM00_HEART:
            return gSaveContext.health < gSaveContext.healthCapacity;
        case ITEM00_MAGIC_SMALL:
        case ITEM00_MAGIC_LARGE:
            return gSaveContext.isMagicAcquired &&
                   gSaveContext.magic < gSaveContext.magicCapacity;

        // Consumable ammo. Gate on (a) player owns the weapon/upgrade
        // (CUR_CAPACITY > 0 ⇒ bag acquired) AND (b) ammo < capacity (vanilla
        // silently discards drops on full bags — picking them up would
        // deprive a teammate for no gain).
        case ITEM00_STICK:
            return CUR_CAPACITY(UPG_STICKS) > 0 &&
                   AMMO(ITEM_STICK) < CUR_CAPACITY(UPG_STICKS);
        case ITEM00_NUTS:
            return CUR_CAPACITY(UPG_NUTS) > 0 &&
                   AMMO(ITEM_NUT) < CUR_CAPACITY(UPG_NUTS);
        case ITEM00_SEEDS:
            return CUR_CAPACITY(UPG_BULLET_BAG) > 0 &&
                   AMMO(ITEM_SLINGSHOT) < CUR_CAPACITY(UPG_BULLET_BAG);
        case ITEM00_ARROWS_SINGLE:
        case ITEM00_ARROWS_SMALL:
        case ITEM00_ARROWS_MEDIUM:
        case ITEM00_ARROWS_LARGE:
            return CUR_CAPACITY(UPG_QUIVER) > 0 &&
                   AMMO(ITEM_BOW) < CUR_CAPACITY(UPG_QUIVER);
        case ITEM00_BOMBS_A:
        case ITEM00_BOMBS_B:
        case ITEM00_BOMBS_SPECIAL:
            return CUR_CAPACITY(UPG_BOMB_BAG) > 0 &&
                   AMMO(ITEM_BOMB) < CUR_CAPACITY(UPG_BOMB_BAG);

        // Bombchus have no upgrade slot (fixed 50-cap). Gate on "player has
        // bombchus in inventory at all".
        case ITEM00_BOMBCHU:
            return INV_CONTENT(ITEM_BOMBCHU) != ITEM_NONE &&
                   AMMO(ITEM_BOMBCHU) < 50;

        // Reserved for progression: heart pieces, heart containers, small
        // keys, shields, tunics, FLEXIBLE (should never reach here post-Init),
        // song chest items, anything else not explicitly listed.
        default:
            return false;
    }
}

uint32_t EligibilityBitForItem00(s16 item00Type) {
    switch (item00Type) {
        case ITEM00_RUPEE_GREEN:  return 1u << 0;
        case ITEM00_RUPEE_BLUE:   return 1u << 1;
        case ITEM00_RUPEE_RED:    return 1u << 2;
        case ITEM00_RUPEE_ORANGE: return 1u << 3;
        case ITEM00_RUPEE_PURPLE: return 1u << 4;
        case ITEM00_HEART:        return 1u << 5;
        case ITEM00_MAGIC_SMALL:  return 1u << 6;
        case ITEM00_MAGIC_LARGE:  return 1u << 7;
        case ITEM00_STICK:        return 1u << 8;
        case ITEM00_NUTS:         return 1u << 9;
        case ITEM00_SEEDS:        return 1u << 10;
        case ITEM00_ARROWS_SINGLE:
        case ITEM00_ARROWS_SMALL:
        case ITEM00_ARROWS_MEDIUM:
        case ITEM00_ARROWS_LARGE:
            return 1u << 11;
        case ITEM00_BOMBS_A:
        case ITEM00_BOMBS_B:
        case ITEM00_BOMBS_SPECIAL:
            return 1u << 12;
        case ITEM00_BOMBCHU:      return 1u << 13;
        default:
            return 0;
    }
}

uint32_t ComputeLocalEligibilityBitmap() {
    // Walks each canonical ITEM00_* representative and ORs in the bit
    // when CanPlayerCollectItem00 returns true. Arrows / bombs use
    // their canonical bag-cap-driven representative (small variant of
    // each); the same eligibility bit covers all variants of that
    // family per EligibilityBitForItem00's collapsed mapping.
    static const s16 kRepresentatives[] = {
        ITEM00_RUPEE_GREEN,
        ITEM00_RUPEE_BLUE,
        ITEM00_RUPEE_RED,
        ITEM00_RUPEE_ORANGE,
        ITEM00_RUPEE_PURPLE,
        ITEM00_HEART,
        ITEM00_MAGIC_SMALL,
        ITEM00_MAGIC_LARGE,
        ITEM00_STICK,
        ITEM00_NUTS,
        ITEM00_SEEDS,
        ITEM00_ARROWS_SMALL,  // any arrow variant
        ITEM00_BOMBS_A,       // any bomb variant
        ITEM00_BOMBCHU,
    };

    uint32_t bitmap = 0;
    for (s16 t : kRepresentatives) {
        if (CanPlayerCollectItem00(t, /*walletCapAware=*/true)) {
            bitmap |= EligibilityBitForItem00(t);
        }
    }

    // Diagnostic for the "STICK bit never sets" bug (log 302 follow-up).
    // Dumps the raw save-context values the STICK eligibility check reads,
    // so we can tell whether Item_Give actually committed UPG_STICKS /
    // AMMO(ITEM_STICK) by the time this hook runs. Same for NUTS — both
    // share the "give upgrade on first pickup" code path. Strip after the
    // root cause is identified.
    SPDLOG_INFO(
        "[Eligibility-Diag] bitmap=0x{:08X} | "
        "UPG_STICKS={} CUR_CAPACITY(UPG_STICKS)={} AMMO(ITEM_STICK)={} "
        "INV_CONTENT(ITEM_STICK)={} | "
        "UPG_NUTS={} CUR_CAPACITY(UPG_NUTS)={} AMMO(ITEM_NUT)={} "
        "INV_CONTENT(ITEM_NUT)={}",
        bitmap,
        (int)CUR_UPG_VALUE(UPG_STICKS), (int)CUR_CAPACITY(UPG_STICKS),
        (int)AMMO(ITEM_STICK), (int)INV_CONTENT(ITEM_STICK),
        (int)CUR_UPG_VALUE(UPG_NUTS), (int)CUR_CAPACITY(UPG_NUTS),
        (int)AMMO(ITEM_NUT), (int)INV_CONTENT(ITEM_NUT));

    return bitmap;
}

}  // namespace ItemEligibility
