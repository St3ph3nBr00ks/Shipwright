#include "ItemEligibility.h"

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

}  // namespace ItemEligibility
