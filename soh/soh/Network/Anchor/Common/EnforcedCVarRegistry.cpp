#include "EnforcedCVarRegistry.h"

#include "soh/cvar_prefixes.h"

namespace AnchorCVarSync {

// ============================================================================
// Class A — must-sync (drift causes desync / crash)
// ============================================================================
//
// Class B entries appear in the second block. The split is documentation-
// only; the wrapper treats both identically. The CVar macro expansions
// resolve to strings at compile time (e.g. "gEnhancements.DamageMult") —
// stable identifiers usable as map keys.

const std::vector<IntCVarEntry> kEnforcedInts = {
    // --- v1 Class A: damage / drops / time / global ----------------------------
    { CVAR_ENHANCEMENT("TimeTravel"),                1 /* TIME_TRAVEL_OOT */ },
    { CVAR_ENHANCEMENT("DamageMult"),                0 },
    { CVAR_ENHANCEMENT("FallDamageMult"),            0 },
    { CVAR_ENHANCEMENT("VoidDamageMult"),            0 },
    { CVAR_CHEAT("FreezeTime"),                      0 },
    { CVAR_ENHANCEMENT("RandomizedEnemies"),         0 },
    { CVAR_ENHANCEMENT("NewDrops"),                  0 },
    { CVAR_ENHANCEMENT("HyperEnemies"),              0 },

    // --- New Class A: Difficulty -----------------------------------------------
    { CVAR_ENHANCEMENT("PermanentHeartLoss"),        0 },
    { CVAR_ENHANCEMENT("FullHealthSpawn"),           0 },
    { CVAR_ENHANCEMENT("NoHeartDrops"),              0 },
    { CVAR_ENHANCEMENT("NoRandomDrops"),             0 },
    { CVAR_ENHANCEMENT("EnableBombchuDrops"),        0 },
    { CVAR_ENHANCEMENT("TreesDropSticks"),           0 },
    { CVAR_ENHANCEMENT("DampeDropRate"),             0 },
    { CVAR_ENHANCEMENT("DampeWin"),                  0 },
    { CVAR_ENHANCEMENT("GoronPot"),                  0 },
    { CVAR_ENHANCEMENT("AllDogsRichard"),            0 },
    { CVAR_ENHANCEMENT("CuccoStayDurationMult"),     1 },
    { CVAR_ENHANCEMENT("CuccosToReturn"),            7 /* vanilla */ },
    { CVAR_ENHANCEMENT("LeeverSpawnRate"),           60 },
    { CVAR_ENHANCEMENT("SwitchTimerMultiplier"),     1 },
    { CVAR_ENHANCEMENT("HyperBosses"),               0 },

    // --- New Class A: QoL spawn / scene gating ---------------------------------
    { CVAR_ENHANCEMENT("NightGSAlwaysSpawn"),        0 },
    { CVAR_ENHANCEMENT("DayGravePull"),              0 },
    { CVAR_ENHANCEMENT("DampeAllNight"),             0 },
    { CVAR_ENHANCEMENT("MarketSneak"),               0 },
    { CVAR_ENHANCEMENT("OpenAllHours"),              0 },
    { CVAR_ENHANCEMENT("CowOfTime"),                 0 },
    { CVAR_ENHANCEMENT("TimeSavers.SleepingWaterfall"),    0 },
    { CVAR_ENHANCEMENT("TimeSavers.SkipJabuJabuFish"),     0 },

    // --- New Class A: Skips / speed-ups (state-altering only) ------------------
    { CVAR_ENHANCEMENT("FasterHeavyBlockLift"),      0 },
    { CVAR_ENHANCEMENT("FasterShadowShip"),          0 },
    { CVAR_ENHANCEMENT("FasterBlockPush"),           0 },
    { CVAR_ENHANCEMENT("MweepSpeed"),                1 },
    { CVAR_ENHANCEMENT("InstantScarecrow"),          0 },
    { CVAR_ENHANCEMENT("ForgeTime"),                 0 },
    { CVAR_ENHANCEMENT("FasterBeanSkull"),           0 },
    { CVAR_ENHANCEMENT("TimeSavers.SkipChildStealth"),     0 },
    { CVAR_ENHANCEMENT("TimeSavers.SkipTowerEscape"),      0 },

    // --- New Class A: Items / explosions / song timing -------------------------
    { CVAR_ENHANCEMENT("FastOcarinaPlayback"),       0 },
    { CVAR_ENHANCEMENT("RemoteBombchu"),             0 },
    { CVAR_ENHANCEMENT("NutsExplodeBombs"),          0 },
    { CVAR_ENHANCEMENT("StaticExplosionRadius"),     0 },
    { CVAR_ENHANCEMENT("BetterBombchuShopping"),     0 },
    { CVAR_ENHANCEMENT("BlueFireArrows"),            0 },
    { CVAR_ENHANCEMENT("SunlightArrows"),            0 },
    { CVAR_ENHANCEMENT("BetterFarore"),              0 },
    { CVAR_ENHANCEMENT("FastFarores"),               0 },

    // --- New Class A: Fixes (collision / damage / spawn shape) -----------------
    { CVAR_ENHANCEMENT("FixVineFall"),               0 },
    { CVAR_ENHANCEMENT("BushDropFix"),               0 },
    { CVAR_ENHANCEMENT("AnubisFix"),                 0 },
    { CVAR_ENHANCEMENT("CrouchStabHammerFix"),       0 },
    { CVAR_ENHANCEMENT("CrouchStabFix"),             0 },
    { CVAR_ENHANCEMENT("WideShutterDoorRange"),      0 },
    { CVAR_ENHANCEMENT("BombchusOOB"),               0 },
    { CVAR_ENHANCEMENT("N64WeirdFrames"),            0 },
    { CVAR_ENHANCEMENT("QuickPutaway"),              0 },
    { CVAR_ENHANCEMENT("QuickBongoKill"),            0 },
    { CVAR_ENHANCEMENT("EarlyEyeballFrog"),          0 },
    { CVAR_ENHANCEMENT("GraveHoles"),                0 },
    { CVAR_ENHANCEMENT("EnemySpawnsOverWaterboxes"), 0 },
    { CVAR_ENHANCEMENT("GravediggingTourFix"),       0 },

    // --- New Class A: Extra Modes ---------------------------------------------
    { CVAR_ENHANCEMENT("BounceOffWalls"),            0 },
    { CVAR_ENHANCEMENT("MirroredWorldMode"),         0 },
    { CVAR_ENHANCEMENT("IvanCoopModeEnabled"),       0 },
    { CVAR_ENHANCEMENT("DogFollowsEverywhere"),      0 },
    { CVAR_ENHANCEMENT("RupeeDash"),                 0 },
    { CVAR_ENHANCEMENT("RupeeDashInterval"),         5 },
    { CVAR_ENHANCEMENT("ShadowTag"),                 0 },
    { CVAR_ENHANCEMENT("HurtContainer"),             0 },
    { CVAR_ENHANCEMENT("ExtraTraps.Enabled"),        0 },
    { CVAR_ENHANCEMENT("ExtraTraps.Ice"),            0 },
    { CVAR_ENHANCEMENT("ExtraTraps.Burn"),           0 },
    { CVAR_ENHANCEMENT("ExtraTraps.Shock"),          0 },
    { CVAR_ENHANCEMENT("ExtraTraps.Knockback"),      0 },
    { CVAR_ENHANCEMENT("ExtraTraps.Speed"),          0 },
    { CVAR_ENHANCEMENT("ExtraTraps.Bomb"),           0 },
    { CVAR_ENHANCEMENT("ExtraTraps.Void"),           0 },
    { CVAR_ENHANCEMENT("ExtraTraps.Ammo"),           0 },
    { CVAR_ENHANCEMENT("ExtraTraps.Kill"),           0 },
    { CVAR_ENHANCEMENT("ExtraTraps.Teleport"),       0 },

    // --- New Class A: Cheats (state-altering) ---------------------------------
    { CVAR_CHEAT("DropsDontDie"),                    0 },
    { CVAR_CHEAT("NoFishDespawn"),                   0 },
    { CVAR_CHEAT("NoBugsDespawn"),                   0 },
    { CVAR_CHEAT("NoRedeadFreeze"),                  0 },
    { CVAR_CHEAT("NoKeeseGuayTarget"),               0 },
    { CVAR_CHEAT("DekuStick"),                       0 },
    { CVAR_CHEAT("TimeSync"),                        0 },
    { CVAR_CHEAT("EnableBetaQuest"),                 0 },
    { CVAR_CHEAT("BetaQuestWorld"),                  0xFFEF /* sentinel "none" */ },

    // ========================================================================
    // Class B — should-sync (drift causes UX / fairness divergence)
    // ========================================================================

    // --- v1 Class B ---
    { CVAR_CHEAT("NoRestrictItems"),                 0 },
    { CVAR_ENHANCEMENT("BonkDamageMult"),            0 /* BONK_DAMAGE_NONE */ },
    { CVAR_CHEAT("InfiniteAmmo"),                    0 },
    { CVAR_CHEAT("ClimbEverything"),                 0 },
    { CVAR_CHEAT("HookshotEverything"),              0 },
    { CVAR_CHEAT("SuperTunic"),                      0 },
    { CVAR_CHEAT("TimelessEquipment"),               0 },
    { CVAR_CHEAT("ShieldTwoHanded"),                 0 },
    { CVAR_ENHANCEMENT("RemoveExplosiveLimit"),      0 },
    { CVAR_CHEAT("FireproofDekuShield"),             0 },

    // --- New Class B: more Infinite* + movement bypasses ---
    { CVAR_CHEAT("InfiniteMoney"),                   0 },
    { CVAR_CHEAT("InfiniteHealth"),                  0 },
    { CVAR_CHEAT("InfiniteMagic"),                   0 },
    { CVAR_CHEAT("InfiniteNayru"),                   0 },
    { CVAR_CHEAT("InfiniteEponaBoost"),              0 },
    { CVAR_CHEAT("NoClip"),                          0 },
    { CVAR_CHEAT("MoonJumpOnL"),                     0 },
    { CVAR_CHEAT("EasyISG"),                         0 },
    { CVAR_CHEAT("EasyQPA"),                         0 },
    { CVAR_CHEAT("SpeedModifier.DoesntChangeJump"),  0 },

    // --- New Class B: capabilities ---
    { CVAR_ENHANCEMENT("EquipmentCanBeRemoved"),     0 },
    { CVAR_ENHANCEMENT("ToggleStrength"),            0 },
    { CVAR_ENHANCEMENT("SwordToggle"),               0 },
    { CVAR_ENHANCEMENT("MMBunnyHood"),               0 /* BUNNY_HOOD_VANILLA */ },
    { CVAR_ENHANCEMENT("AdultMasks"),                0 },
    { CVAR_ENHANCEMENT("BowSlingshotAmmoFix"),       0 },
    { CVAR_ENHANCEMENT("SeparateArrows"),            0 },
    { CVAR_ENHANCEMENT("FastBoomerang"),             0 },
    { CVAR_ENHANCEMENT("InstantPutaway"),            0 },
    { CVAR_ENHANCEMENT("PauseWarp"),                 0 },

    // --- Pillar G.ii — non-blocking item-get cutscene skip ---
    // Default 1 (enabled). When on, MP routes non-iconic item-gets
    // through the silent-give path + Notification toast; iconic
    // items (ocarinas / Light Arrows / Great Fairy spells / Ice
    // Trap) keep their vanilla cutscenes. Host-authoritative so all
    // clients in a session experience the same item-pickup cadence.
    { CVAR_ENHANCEMENT("Anchor.NonBlockingItemGet"), 1 },

    // --- Pillar G.ii — non-blocking text-box world-time flip ---
    // Default 1 (enabled). When on in MP, the day/night clock
    // continues advancing on the reading client while an NPC dialog
    // is open (Zora, Mido, shop dialog, etc.). Reader's own view of
    // the actor freeze is unchanged — the reader is still locked in
    // dialog locally; peers see the world keep moving. Sibling to
    // NonBlockingItemGet; consumed via the ShouldAdvanceWorldTime
    // TextBox context in GameTimeController.
    { CVAR_ENHANCEMENT("Anchor.NonBlockingTextBox"), 1 },
};

const std::vector<FloatCVarEntry> kEnforcedFloats = {
    // v1 floats
    { CVAR_CHEAT("SpeedModifier.Value"),             1.0f },
    { CVAR_CHEAT("BombTimerMultiplier"),             1.0f },

    // New Class B float
    { CVAR_CHEAT("HookshotReachMultiplier"),         1.0f },
};

}  // namespace AnchorCVarSync
