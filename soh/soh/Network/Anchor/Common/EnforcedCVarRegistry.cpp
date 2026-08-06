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

    // --- Pillar 5 — Skullwalltula enhancement toggles (GH #210) ---
    // Host-authoritative. Enhanced behaviour is host-driven; peers
    // observe via existing ENEMY_UPDATE sync. Defaults 0 per project
    // policy (`feedback_vanilla_altering_default_off.md`). Enforcement
    // added per `Plans/en_sw_enhanced_state_machine_pilot.md` §8.
    // Widgets moved from "Enemy Enhancements" to Host Settings sidebar
    // in same milestone.
    { CVAR_ENHANCEMENT("Skullwalltula.NavConsume"),        0 },
    { CVAR_ENHANCEMENT("Skullwalltula.GravityAware"),      0 },
    { CVAR_ENHANCEMENT("Skullwalltula.AggressiveAcquire"), 0 },

    // Swap-spawned Skullwalltula HP cap. Scope-narrow: applies ONLY
    // to En_Sw actors that spawn as the result of a Skulltula
    // (En_St) → Skullwalltula (En_Sw) swap in FireSwap. Vanilla-
    // spawned En_Sw retain their vanilla HP (default 1 for combat,
    // 2 for gold-token, plus any modifications from other SoH
    // enhancements like DamageMult / HyperEnemies).
    //
    // The carryover writes newEnSw->colChkInfo.health =
    // min(oldEnSt->colChkInfo.health, SwapMaxHP). Default 2 matches
    // vanilla En_St base HP so a full-HP swap yields a 2-HP En_Sw
    // "for free" (no user opt-in needed to fix the 2→1 HP loss bug).
    // Range 1-20 exposed via slider; reader clamps 1-127 to fit the
    // s8 netHealth MP-sync cache.
    { CVAR_ENHANCEMENT("Skullwalltula.SwapMaxHP"),         2 },

    // --- Pillar 5 Phase 3 — Skulltula → Skullwalltula swap (GH #210) ---
    // Host-authoritative. When a ceiling Skulltula (En_St) descends
    // toward the ground, it may transform into a Skullwalltula (En_Sw)
    // for combat variety. Two triggers:
    //   Skulltula.GroundDrop     — random chance % on ground impact
    //                              (default 30% per user 2026-08-04)
    //   Skulltula.AggressiveDrop — 100% swap if hit while descending
    // Combined with the Skullwalltula.* enhancement toggles above the
    // spawned En_Sw inherits the enhanced-mode nav-consume + gravity-
    // aware + aggressive-acquire capabilities.
    { CVAR_ENHANCEMENT("Skulltula.GroundDrop"),           30 },
    // 2026-08-04 (user) — AggressiveDrop is a percentage slider,
    // not a bool. Default 50 (halfway) so first-time users see
    // moderate swap frequency; 100 was too intrusive per playtest.
    { CVAR_ENHANCEMENT("Skulltula.AggressiveDrop"),       50 },

    // --- Pillar 5 — Karebaba enhancement toggle (GH #310) ---
    // Host-authoritative geyser AoE Spin. Default 0 per project
    // policy. When ON, host rolls 33% chance at each Spin entry;
    // on success, head scales up + a vertical acid plume erupts
    // from actor.home.pos for the spin window. See
    // Plans/en_karebaba_enhanced_plan.md.
    { CVAR_ENHANCEMENT("Karebaba.GeyserSpin"), 0 },

    // Pillar 5 (#308) — Dekubaba acid vomit projectile enhancement.
    // Independent CVar per plan §CVars. When ON, host rolls acid
    // chance at each DecideLunge (25% steps ramp per attack); on
    // success + Link in acid range → transitions to AcidVomit state
    // that spawns EN_DEKUBABA_ACID projectile instead of vanilla
    // lunge. Features B (#309 detach) and C (#318 seed) will be
    // separate CVars.
    { CVAR_ENHANCEMENT("Dekubaba.AcidVomit"), 0 },

    // Pillar 5 (#309) — Dekubaba detach + pursue enhancement.
    // Independent from AcidVomit. When ON, each attack advances a
    // +25% detach counter; on success, Dekubaba severs from stem and
    // squirms toward nearest player. -1 HP every 5s (bleedout); dies
    // at 0 HP (stays dead until scene reload — matches vanilla
    // Dekubaba which doesn't regrow). One-shot per life. Unified
    // pattern per 2026-07-31 user spec — no range gate.
    { CVAR_ENHANCEMENT("Dekubaba.DetachAndPursue"), 0 },

    // Pillar 5 (#318) — Dekubaba seed spawn enhancement.
    // Independent from Acid + Detach. When ON, each attack advances
    // a +25% seed counter; on success, Dekubaba spits a seed
    // projectile behind Link that spawns a child Dekubaba on land.
    // Parent tracks own child — cannot fire another seed while own
    // child is alive. Children CAN acid + detach but NOT seed
    // (unless future SeedChildrenCanSeed override CVar ON).
    { CVAR_ENHANCEMENT("Dekubaba.SeedSpawn"), 0 },

    // Pillar 5 (2026-08-01) — Dekubaba diagnostic logging gate.
    // When ON, SPDLOG_INFO fires at every acid/seed/detach roll
    // decision + charge state transition + squirm position telemetry.
    // Used for field-test analysis of enhancement fire behavior.
    // Host-authoritative so both clients emit the same trace lines.
    { CVAR_ENHANCEMENT("Dekubaba.DebugLog"), 0 },

    // Game Director #311 substrate (2026-08-02) — master gate for the
    // GenericSpawnDescriptor's data-driven scene/room ambient spawns.
    // When ON, host rolls per-tick against RoomSpawnRegistry entries
    // for the local player's (scene, room). Registry is EMPTY at
    // scaffold — consumers (Werewolf #316, Goma egg swarms #323,
    // per-dungeon ambient pools) append entries. Host-authoritative
    // so both clients share the "generic spawns are ON" state.
    // Default OFF per project rule for vanilla-altering features.
    { CVAR_ENHANCEMENT("Director.GenericSpawnsEnabled"), 0 },
};

const std::vector<FloatCVarEntry> kEnforcedFloats = {
    // v1 floats
    { CVAR_CHEAT("SpeedModifier.Value"),             1.0f },
    { CVAR_CHEAT("BombTimerMultiplier"),             1.0f },

    // New Class B float
    { CVAR_CHEAT("HookshotReachMultiplier"),         1.0f },
};

}  // namespace AnchorCVarSync
