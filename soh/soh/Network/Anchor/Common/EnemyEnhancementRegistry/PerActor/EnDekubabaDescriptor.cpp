/**
 * EnDekubabaDescriptor implementation — Feature A (acid vomit).
 *
 * Feature B / C are stubbed; extending them layers additional
 * ChargeStateMachine instances + booleans on this descriptor.
 *
 * MP model (mirror of Karebaba V6 pattern):
 *   Host: OnHostMaybeAcidLunge rolls state machine + range gate;
 *         if fires → sets state.currentAttackIsAcid = true.
 *         Send-side (EnemyState.cpp) reads via IsCurrentAttackAcid()
 *         and IsAcidCharged() and adds the two bools to the outgoing
 *         payload.
 *   Peer: HookHandlers calls OnPeerReceive*Flag BEFORE ApplyNetState.
 *         Then the local ApplyNetState(state 14 = AcidVomit) sees
 *         the flags and runs the acid path.
 *   Both: OnAcidVomitTick runs each frame during the acid state;
 *         spawns EN_DEKUBABA_ACID projectile at fire frame. Spawn
 *         is deterministic (fixed position + fixed frame) — no
 *         per-spawn broadcast needed. Damage via Path A when the
 *         projectile's AC collider hits a DummyPlayer.
 */

#include "soh/Network/Anchor/Anchor.h"  // Pitfall 40

#include "EnDekubabaDescriptor.h"

#include "soh/Network/Anchor/Common/EnforcedCVars.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/Common/PlayerLookup.h"  // FindNearestPlayerActor
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/AcidVisuals.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/EnhancementAudio.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/ChargeStateMachine.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/GroundFollow.h"
// Pitfall 17 — frame_interpolation.h BEFORE the extern "C" block so
// OPEN_DISPS macro expansions in the global-namespace splice helper
// have C-linkage FrameInterpolation_Record{Open,Close}Child decls.
#include "soh/frame_interpolation.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>
#include <unordered_map>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
// gameplay_keep DList + texture symbols used by the seed-in-mouth
// render (Log-820 Bug 2a fix). gameplay_keep is always loaded, so
// these symbols are safe to reference from any actor draw callback.
#include "assets/objects/gameplay_keep/gameplay_keep.h"
// Custom projectile actor ids — registered via ActorDB::AddBuiltIn
// CustomActors, declared in soh/src/code/z_play.c.
extern s16 gEnDekubabaAcidId;
extern s16 gEnDekubabaSeedId;
}

#include "soh/Enhancements/RoomNavData/RoomNavData.h"  // FindNearestFloorNodeXZRadius

// Log-820 Bug 2a fix (2026-08-02) — global-namespace splice helper for
// the mouth-nut render. Pitfall 17: OPEN_DISPS macro expansions contain
// FrameInterpolation_Record{Open,Close}Child block-scope decls; if
// expanded inside a C++ namespace those decls take C++ linkage and
// defeat the C-linkage definitions in frame_interpolation.c → link
// error. Confining the splice to global namespace avoids that. Called
// via `::EnDekubabaSpliceSeedInMouth(...)` from the namespaced
// OnDrawHook method.
static void EnDekubabaSpliceSeedInMouth(PlayState* play,
                                          f32 posX, f32 posY, f32 posZ,
                                          f32 scale) {
    OPEN_DISPS(play->state.gfxCtx);
    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    POLY_OPA_DISP = Play_SetFog(play, POLY_OPA_DISP);
    POLY_OPA_DISP = Gfx_SetupDL_66(POLY_OPA_DISP);

    Matrix_Translate(posX, posY, posZ, MTXMODE_NEW);
    Matrix_ReplaceRotation(&play->billboardMtxF);
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);

    // Pitfall 46 — gameplay_keep DList/tex symbols are declared as
    // `static const ALIGN_ASSET(2) char gFoo[] = "__OTR__..."` (asset
    // paths). C files pass them straight to gSP* (implicit char[] →
    // void*/Gfx* conversion); C++ rejects the same code with C2664.
    // Also reinterpret_cast<Gfx*>(const char[]) is rejected (C2440
    // — incompatible object layouts). C-style cast via `void*`
    // bridges both const-stripping and pointer-type reinterpretation
    // in one expression, matching how the vanilla C files effectively
    // do the same conversion via implicit rules.
    gSPSegment(POLY_OPA_DISP++, 0x08,
               (uintptr_t)SEGMENTED_TO_VIRTUAL(gDropDekuNutTex));
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx),
                G_MTX_MODELVIEW | G_MTX_LOAD);
    gSPDisplayList(POLY_OPA_DISP++, (Gfx*)(void*)gItemDropDL);

    CLOSE_DISPS(play->state.gfxCtx);
}

namespace AnchorEnemyEnhancement {

namespace {

// Diagnostic logging — gated on gEnhancements.Dekubaba.DebugLog CVar.
// Off by default; enable in Flotilla menu or via console for
// post-2026-08-01 field-test analysis of acid/seed/detach paths.
// Wraps SPDLOG_INFO so unused calls don't compile out format args.
#define DEKUBABA_DBG(fmt, ...) \
    do { \
        if (CVarGetInteger("gEnhancements.Dekubaba.DebugLog", 0) != 0) { \
            SPDLOG_INFO("[Dekubaba] " fmt, ##__VA_ARGS__); \
        } \
    } while (0)

// Charge state machine — 25% steps, max 4, 3-attack cooldown. Rolls
// on each DecideLunge → attack transition. Chance = counter × step.
//
// initialCounter = 2 (2026-08-03 per user) — first attack gets 50%
// chance so short-lived Dekubabas actually get to use the new
// attacks before dying. Post-cooldown restart uses same 50% baseline.
// Prior value (1 = 25%) still had many Dekubabas dying without
// firing acid at all.
constexpr ChargeStateMachine::Config kAcidChargeConfig = {
    /*stepIncrement*/ 0.25f,
    /*maxCounter*/    4,
    /*cooldownSteps*/ 3,
    /*initialCounter*/ 2,
};

// Range gate — acid is useful when Link is outside melee lunge range
// but within a plausible arc distance. Melee lunge ends at ~80u*size.
// Acid range 120..300u XZ — Link too close makes acid trivially
// dodgeable; too far and the arc math gets silly.
constexpr float kAcidMinRangeXZ = 100.0f;
constexpr float kAcidMaxRangeXZ = 400.0f;

// Vanilla Dekubaba scale at Init: this->size × 0.01 (where size is
// 1.0 for NORMAL, ~2.0 for BIG). Multiplier used during the acid
// telegraph to grow the head 1.5×.
// Telegraph head scale — 1.25× vanilla per user 2026-08-01. Previously
// 1.5× which was too dramatic and (because Actor_SetScale scales the
// ENTIRE actor including stem, not just head) made the whole plant
// look oversized during the attack, giving a "slow motion" feel to
// the attack animation. 1.25× matches Karebaba's kTelegraphHeadScale
// for consistency.
constexpr float kTelegraphHeadScale = 1.25f;
constexpr int   kTelegraphSpitPeriod = 4;  // spawn every N frames

// Change 2 (2026-08-02) — Dekubaba-only spit tuning per user:
//   "move the acid/water particle effect further forward in the
//   mouth 5u and increase particle effect size 25%."
// Local overrides so Karebaba's spit config (shared AcidVisuals)
// stays untouched.
constexpr float kSpitForwardOffset  = 5.0f;
constexpr float kSpitScaleMult      = 1.25f;

// Acid attack timing (per plan §Feature A Design "~15-20 frames"):
//   Frames 0-15  — telegraph (head grows, mouth spits, stem rears)
//   Frame 15     — projectile spawn (host + peer both spawn locally)
//   Frames 15-25 — snap forward (post-spit follow-through)
//   Frame 25     — transition to PullBack (vanilla-parity recovery)
constexpr int kAcidTelegraphEndFrame = 15;
constexpr int kAcidSpawnFrame        = 15;
constexpr int kAcidTotalFrames       = 25;

// Projectile ballistic parameters.
//
// Bug 4 fix (2026-08-01): projectile was overshooting Link because
// vy was fixed at 4.0f regardless of target distance. Now solved
// per-fire from actual target position via ballistic math (see
// OnAcidVomitTick).
//
// Fixed XZ speed keeps the projectile visibly readable (not too
// slow, not lightning-fast). Gravity matches z_en_dekubaba_acid.c.
constexpr float kAcidSpitXZSpeed        = 8.0f;
constexpr float kAcidSpitGravity        = -0.7f;
// Aim at Link's chest (~20u above his feet) for more forgiving
// vertical framing — hitting feet exactly is finicky if Link's
// standing on uneven ground.
constexpr float kAcidTargetChestOffsetY = 20.0f;
// Ballistic time-of-flight guardrails. Very close targets get a
// clamped-minimum flight time so the projectile doesn't have a
// near-vertical trajectory that looks like a spit-straight-down;
// very far targets are clamped at a sensible max so we don't launch
// at absurd upward velocity.
constexpr float kAcidMinFlightFrames    = 8.0f;
constexpr float kAcidMaxFlightFrames    = 50.0f;

// ---- Feature B (#309) — detach + pursue tuning ---------------------
//
// UNIFIED PATTERN (per user 2026-07-31): detach uses the same
// ChargeStateMachine config as acid — 25% steps, max 4, 3-attack
// cooldown. Roll site moved to DecideLunge (same as acid, evaluated
// after acid fails). No range gate — fires purely on counter roll.
// One-shot per actor life via sticky isDetached flag; the ChargeState
// cooldown never activates because the actor transitions to
// DetachedSquirm and never runs another DecideLunge.
// Detach tuning tweaked 2026-08-03 per user request: "Reduce chance
// to enter detached mode to 10% on first attack and increase by 10%
// each time." Was 25% + 25%/attack, now 10% + 10%/attack. Detach
// is more dramatic than acid/seed (permanent state change, plant
// dies to bleedout), so slower ramp gives Link more time to finish
// the encounter conventionally before the plant severs itself.
// maxCounter 10 so chance can still climb to 100% at max (10 × 0.10);
// realistic ceiling is ~5-6 attacks (which would be an unusually
// long fight to trigger anyway).
constexpr ChargeStateMachine::Config kDetachChargeConfig = {
    /*stepIncrement*/  0.10f,
    /*maxCounter*/     10,
    /*cooldownSteps*/  3,
    /*initialCounter*/ 1,   // 2026-08-03 — 10% first attack
};

// Squirm motion (DetachedSquirm state).
//   XZ speed 2.0u/frame — slow crawl. Not fast enough to catch
//   sprinting Link; requires Link to engage.
//   Stem-angle sine amplitude 0x1000 (~5.6°) with 3-segment phase
//   offset 0°/120°/240° for a serpentine wave.
constexpr float kSquirmSpeedXZ            = 2.0f;
// Bug 7 fix (2026-08-01) — slither amplitude tripled per user
// request. Was 0x1000 (~22.5° swing) which felt too subtle; user
// wanted 3× → 0x3000 (~67.5° swing). Combined with base 0x0800
// (~11.25°), stem sections swing between -0x2800 and +0x3800.
// UpdateHeadPosition's spherical trig produces visibly dramatic
// serpentine motion at this amplitude.
constexpr s16   kSquirmStemAmplitude      = 0x3000;
constexpr s16   kSquirmStemBase           = 0x0800;
constexpr float kSquirmPhasePerFrame      = 0.10472f;  // 2π/60 → 60-frame period
constexpr int   kBleedoutIntervalMs       = 5000;      // -1 HP every 5s

// DetachedDying state.
//   ShrinkDie animation duration ~30 frames vanilla; give 40 for a
//   slightly slower squirm-death visual.
constexpr int   kDetachedDyingFrames      = 40;

// ---- Feature C (#318) — seed spawn tuning --------------------------
//
// UNIFIED PATTERN — same ChargeStateMachine config as acid + detach
// (25% steps, max 4, 3-attack cooldown). Consistent with user 2026-
// 07-31 spec: "all three ... same karebaba 25% increase per attack".
constexpr ChargeStateMachine::Config kSeedChargeConfig = {
    /*stepIncrement*/  0.25f,
    /*maxCounter*/     4,
    /*cooldownSteps*/  3,
    /*initialCounter*/ 2,  // 2026-08-03 — start at 50% (see acid config)
};

// Seed telegraph + fire timing. Matches acid cycle length so both
// exotics slot into the same DecideLunge → attack → PullBack → Recover
// sequence positions.
constexpr int kSeedTelegraphEndFrame = 15;
constexpr int kSeedFireFrame         = 15;   // spawn projectile at this frame
constexpr int kSeedTotalFrames       = 25;

// Landing pick — behind Link, along Link's facing. Distance beyond
// Link puts the child in Link's blind spot forcing reposition.
constexpr float kSeedBehindLinkOffset = 100.0f;

// Max seed flight distance from Dekubaba (safety cap on where the
// projectile can spawn a child). Extended from plan's 300u to 400u
// per unified-pattern discussion to accommodate behind-Link firings
// at medium Dekubaba-Link distances.
constexpr float kSeedMaxFlightDistance = 400.0f;

// Fallback landing radius when behind-Link landing fails nav-validation.
// Random XZ within this radius of the Dekubaba — always fires (never
// skip) per user 2026-07-31 spec.
constexpr float kSeedFallbackRadius   = 300.0f;

// Nav-validation radius when checking landing target for a valid
// floor node.
constexpr float kSeedNavRadius        = 60.0f;

// Seed projectile flight speed (units/frame). Straight-line trajectory
// from head to landing; time-of-flight = distance / speed.
constexpr float kSeedProjectileSpeed  = 12.0f;

// Per-actor state map. Same shape as Karebaba's — created on demand
// at first hook fire, wiped on OnDeath / OnActorDestroy.
struct DekubabaEnhancedState {
    // Per-attack flags (reset at OnAttackComplete).
    bool currentAttackIsAcid   = false;
    bool acidProjectileSpawned = false;
    int  acidAttackFrame       = 0;   // 0..kAcidTotalFrames

    // Charge state machine — persists across attacks, reset on death.
    ChargeStateMachine acidCharge{ kAcidChargeConfig };

    // Peer-received flags. Host writes these from its own charge
    // state; peer's ChargeStateMachine is stale (host is sole roller).
    bool netAcidActive  = false;
    bool netAcidCharged = false;

    // ---- Feature B (#309) — detach state --------------------------
    //
    // UNIFIED PATTERN (2026-07-31): detach uses same ChargeStateMachine
    // shape as acid — rolled at DecideLunge, 25% steps, 3-attack
    // cooldown. On fire, sets isDetached=true and transitions actor
    // out of the attack cycle permanently, so the cooldown never
    // actually decrements (actor never returns to DecideLunge). The
    // ChargeStateMachine is used for pattern uniformity even though
    // it's effectively one-shot.
    ChargeStateMachine detachCharge{ kDetachChargeConfig };

    // Sticky per-life flag — true after detach fires. Peer's Draw
    // gate + local SetupDetachedSquirm path both read via IsDetached
    // bridge query.
    bool isDetached          = false;

    // Timer state for the squirm/dying states.
    int  squirmFrameCounter  = 0;  // 0..∞, drives sine phase + bleedout
    int  lastBleedoutFrame   = 0;  // wall-clock game-tick of last -1 HP
    int  dyingFrameCounter   = 0;

    // Peer-received detach flag (mirror of netAcidActive shape).
    bool netDetachActive = false;

    // ---- Feature C (#318) — seed spawn state ----------------------
    //
    // Seed charge state machine (25% steps, max 4, 3-attack cooldown).
    ChargeStateMachine seedCharge{ kSeedChargeConfig };

    // Sticky flag — this actor was spawned by a parent Dekubaba's
    // seed. Prevents child from seeding (unless SeedChildrenCanSeed
    // CVar is on). Set at Init when the g_isSpawningDekubabaChild
    // thread-local flag is true.
    bool isSpawnedByEnhancement = false;

    // Own-child tracking (Q2 answer 2026-07-31: parent tracks own
    // child only). Stores the netId of the currently-alive child
    // spawned by THIS parent's seed. 0 = no active child. Cleared
    // when child is destroyed (host walks actor list each seed
    // roll to verify).
    Actor* spawnedChildActor = nullptr;

    // Per-attack seed flags (reset at OnAttackComplete).
    bool currentAttackIsSeed = false;
    bool seedProjectileSpawned = false;
    int  seedAttackFrame       = 0;

    // Seed landing target — computed at fire decision, shipped over
    // wire so peer's projectile arcs to the same coord.
    Vec3f seedLandingPos = { 0.0f, 0.0f, 0.0f };

    // Peer-received seed active flag.
    bool netSeedActive = false;

    // Log-820 Bug 2a fix (2026-08-02) — per-frame draw-hook state.
    // Set true whenever the Dekubaba should display a Deku Nut model
    // at its mouth. Consumed by OnDrawHook (called from EnDekubaba_Draw).
    // Set true when: (a) SeedTelegraph is running pre-fire OR (b) seed
    // charge is Ready but no attack is active AND acid isn't also Ready
    // (acid takes render priority — matches DecideLunge order).
    // Cleared each frame at top of OnEveryFrameTick; re-set only when
    // conditions apply. Also cleared at seed projectile spawn moment
    // in OnSeedFireTick + on OnAttackComplete + OnDeath.
    bool showSeedInMouth = false;
};

// Thread-local flag set by OnSeedLanded's Actor_Spawn bracket, read
// by OnActorInit on the newly-spawned Dekubaba. Same pattern as
// g_isSpawningNetworkItemDrop (session_state Pillar C2 Phase 4 §3.1).
thread_local bool g_isSpawningDekubabaChild = false;

std::unordered_map<Actor*, DekubabaEnhancedState> sStates;

DekubabaEnhancedState& GetOrCreate(EnDekubaba* actor) {
    return sStates[&actor->actor];
}

// Telegraph render — head grows, mouth spits, matching Karebaba's
// RenderTelegraph pattern but scaled for Dekubaba's larger vanilla
// head + longer telegraph window.
inline void RenderAcidTelegraph(EnDekubaba* actor, PlayState* play) {
    // Head at 1.5× (Dekubaba's baseline scale = this->size × 0.01).
    const f32 baseScale = actor->size * 0.01f;
    Actor_SetScale(&actor->actor, baseScale * kTelegraphHeadScale);

    // Mouth spit every N frames using shared AcidVisuals config.
    if ((play->gameplayFrames % kTelegraphSpitPeriod) == 0) {
        Color_RGBA8 primC = AcidVisuals::kSpitPrimColor;
        Color_RGBA8 envC  = AcidVisuals::kSpitEnvColor;
        // Spawn at head position. Dekubaba's world.pos IS the head
        // tip (per session_state Fix 7 — world.pos is animation-
        // computed, not stem base). Small XZ jitter for organic feel.
        // Change 2 (2026-08-02) — shift +5u along shape.rot.y so the
        // spit visibly emerges from the mouth (forward of head-tip)
        // rather than centered on the head.
        const s16 yaw = actor->actor.shape.rot.y;
        const float fwdX = Math_SinS(yaw) * kSpitForwardOffset;
        const float fwdZ = Math_CosS(yaw) * kSpitForwardOffset;
        Vec3f pos = {
            actor->actor.world.pos.x + fwdX + (Rand_ZeroOne() - 0.5f) * 8.0f,
            actor->actor.world.pos.y + (Rand_ZeroOne() - 0.5f) * 4.0f,
            actor->actor.world.pos.z + fwdZ + (Rand_ZeroOne() - 0.5f) * 8.0f,
        };
        // Change 2 (2026-08-02) — Dekubaba-only 1.25× scale bump.
        // Karebaba path (EnKarebabaDescriptor::RenderTelegraph) still
        // uses the shared unmodified kSpitSplashScale.
        const s16 dekubabaSpitScale =
            (s16)(AcidVisuals::kSpitSplashScale * kSpitScaleMult);
        EffectSsGSplash_Spawn(play, &pos, &primC, &envC,
                                AcidVisuals::kSpitSplashType,
                                dekubabaSpitScale);
    }

    // Ready-state green bubble accent (user 2026-08-02) — while any
    // charge machine is Ready OR we are inside the telegraph itself,
    // spawn a small ~30u peak vertical bubble on the head so the
    // player has a persistent visual cue that an exotic attack is
    // armed. Placed at head; matches Karebaba's Ready visual.
    Vec3f bubblePos = actor->actor.world.pos;
    AcidVisuals::SpawnReadyBubbles(play, bubblePos);
}

}  // namespace

// Bug 8 fix (2026-08-02) — parent-pointer bridging for seed projectile.
// Namespace-scope (NOT anonymous) so the bridge cpp file can extern it
// via `extern thread_local Actor* AnchorEnemyEnhancement::g_pendingSeedProjectileParent;`.
// Set by OnSeedFireTick before Actor_Spawn(EN_DEKUBABA_SEED); read +
// cleared via Anchor_Enhance_EnDekubaba_ConsumePendingSeedParent from
// the seed actor's Init. Consumers see nullptr if the projectile is
// spawned by any other path (defensive default).
thread_local Actor* g_pendingSeedProjectileParent = nullptr;

// ---- Feature A implementation ---------------------------------------

bool EnDekubabaDescriptor::OnHostMaybeAcidLunge(EnDekubaba* actor,
                                                  PlayState* play) {
    if (actor == nullptr) return false;
    (void)play;

    // Sync-rule 1 — host is sole RNG decider.
    if (!SceneAuthority::IsMyCurrentRoomHost()) return false;

    // CVar gate (host-authoritative via enforced registry).
    if (AnchorCVarSync::GetEnforcedInt(AcidVomitCVarName(), 0) == 0) {
        // CVar off — clear any stale per-attack flags so nothing
        // leaks into an unenhanced attack.
        auto it = sStates.find(&actor->actor);
        if (it != sStates.end()) {
            it->second.currentAttackIsAcid   = false;
            it->second.acidProjectileSpawned = false;
            it->second.acidAttackFrame       = 0;
        }
        return false;
    }

    DekubabaEnhancedState& state = GetOrCreate(actor);
    // Clear per-attack flags at each decision point — either acid
    // fires this attack or it doesn't, no residual state.
    state.currentAttackIsAcid   = false;
    state.acidProjectileSpawned = false;
    state.acidAttackFrame       = 0;

    // Ready branch: charged + Link in valid range → fire acid.
    if (state.acidCharge.IsReady()) {
        const f32 dist = actor->actor.xzDistToPlayer;
        DEKUBABA_DBG("acid Ready — dist={:.0f} range=[{:.0f},{:.0f}]",
                     dist, kAcidMinRangeXZ, kAcidMaxRangeXZ);
        if (dist >= kAcidMinRangeXZ && dist <= kAcidMaxRangeXZ) {
            state.acidCharge.OnFire();
            state.currentAttackIsAcid = true;
            SPDLOG_INFO("[Dekubaba] acid FIRE decision — actor={} dist={:.0f} (cooldown armed 3)",
                        (void*)&actor->actor, dist);
            EnhancementAudio::PlayBoostedActorSfx(
                &actor->actor, NA_SE_EV_WATER_BUBBLE);
            return true;
        }
        return false;  // Out of range — preserve Ready.
    }

    // Charging branch.
    const u8 counterBefore = state.acidCharge.GetCounter();
    if (state.acidCharge.TryCharge()) {
        DEKUBABA_DBG("acid CHARGED — counter {} → Ready", counterBefore);
        EnhancementAudio::PlayBoostedActorSfx(
            &actor->actor, NA_SE_EV_WATER_BUBBLE);
    } else {
        DEKUBABA_DBG("acid rolling — counter {}%, state={}",
                     counterBefore * 25, (int)state.acidCharge.GetState());
    }
    return false;
}

void EnDekubabaDescriptor::OnPeerReceiveAcidActiveFlag(EnDekubaba* actor,
                                                        bool active) {
    if (actor == nullptr) return;
    DekubabaEnhancedState& state = GetOrCreate(actor);
    const bool wasActive = state.netAcidActive;
    state.netAcidActive = active;
    // Transition log — only fire on false↔true flip (not per-tick).
    if (wasActive != active) {
        SPDLOG_INFO("[Dekubaba] peer RECV acid active: {} → {} actor={}",
                    wasActive, active, (void*)&actor->actor);
    }
    // If newly-enabling for this attack, reset per-attack counters
    // so peer's local AcidVomit fires the sequence from scratch.
    if (active) {
        state.currentAttackIsAcid   = true;
        state.acidProjectileSpawned = false;
        state.acidAttackFrame       = 0;
    }
}

void EnDekubabaDescriptor::OnPeerReceiveAcidChargedFlag(EnDekubaba* actor,
                                                         bool charged) {
    if (actor == nullptr) return;
    DekubabaEnhancedState& state = GetOrCreate(actor);
    const bool wasCharged = state.netAcidCharged;
    state.netAcidCharged = charged;
    if (wasCharged != charged) {
        SPDLOG_INFO("[Dekubaba] peer RECV acid charged: {} → {} actor={}",
                    wasCharged, charged, (void*)&actor->actor);
    }
}

void EnDekubabaDescriptor::OnAcidVomitTick(EnDekubaba* actor,
                                             PlayState* play, int frame) {
    if (actor == nullptr || play == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    DekubabaEnhancedState& state = it->second;

    // Render telegraph particles through the whole cycle for
    // continuous visual (matches Karebaba V7 telegraph-persists
    // pattern).
    if (frame <= kAcidTelegraphEndFrame) {
        RenderAcidTelegraph(actor, play);
    }

    // Fire projectile at kAcidSpawnFrame — both host and peer spawn
    // locally with proper ballistic trajectory to target's current
    // position. Each client aims at its OWN nearest player, so peer's
    // client sees a projectile aimed at peer's Link; host's client
    // sees one aimed at host's Link. Users see acid coming at them.
    if (!state.acidProjectileSpawned && frame >= kAcidSpawnFrame) {
        state.acidProjectileSpawned = true;
        if (gEnDekubabaAcidId != 0) {
            // Bug 4 fix (2026-08-01) — proper ballistic solution.
            // Previously: fixed vy=4.0f + fixed XZ speed → projectile
            // overshot Link's head at close range, undershot at far
            // range. Now: sample target position at fire moment,
            // solve for initial vy given horizontal distance + fixed
            // XZ speed so parabolic arc lands ON the target.
            Actor* target = FindNearestPlayerActor(&actor->actor, play);
            if (target != nullptr) {
                const Vec3f spawnPos = actor->actor.world.pos;
                Vec3f targetPos = target->world.pos;
                targetPos.y += kAcidTargetChestOffsetY;  // aim at chest, not feet

                const f32 dx = targetPos.x - spawnPos.x;
                const f32 dz = targetPos.z - spawnPos.z;
                const f32 distXZ = sqrtf(dx * dx + dz * dz);

                if (distXZ > 0.001f) {
                    // Time-of-flight = XZ distance / horizontal speed.
                    // Clamped to plausible bounds so close/far edge cases
                    // don't produce silly trajectories.
                    f32 tFlight = distXZ / kAcidSpitXZSpeed;
                    if (tFlight < kAcidMinFlightFrames) tFlight = kAcidMinFlightFrames;
                    if (tFlight > kAcidMaxFlightFrames) tFlight = kAcidMaxFlightFrames;

                    // Ballistic Y: y(t) = y0 + vy*t + 0.5*g*t²
                    // Solve for vy given y(tFlight) == targetPos.y:
                    //   vy = (yTarget - y0 - 0.5*g*t²) / t
                    const f32 vy = (targetPos.y - spawnPos.y -
                                     0.5f * kAcidSpitGravity * tFlight * tFlight)
                                    / tFlight;

                    // XZ velocity — unit direction × speed.
                    const f32 invDist = 1.0f / distXZ;
                    const f32 vx = dx * invDist * kAcidSpitXZSpeed;
                    const f32 vz = dz * invDist * kAcidSpitXZSpeed;

                    // Yaw for actor.world.rot.y — points along XZ flight
                    // direction so the projectile visually rotates to
                    // face its trajectory.
                    const s16 aimYaw = Math_Atan2S(dz, dx);
                    const s16 params = (s16)(aimYaw / 8);

                    Actor* projectile = Actor_Spawn(&play->actorCtx, play,
                                                     gEnDekubabaAcidId,
                                                     spawnPos.x, spawnPos.y, spawnPos.z,
                                                     0, aimYaw, 0, params);
                    if (projectile != nullptr) {
                        // Override the projectile's Init-computed
                        // velocity with our ballistic solution. Init
                        // set velocity from params (fixed vy=4); this
                        // is the trajectory we want the projectile to
                        // actually follow.
                        projectile->velocity.x = vx;
                        projectile->velocity.y = vy;
                        projectile->velocity.z = vz;
                        projectile->speedXZ    = kAcidSpitXZSpeed;
                        projectile->gravity    = kAcidSpitGravity;
                    }
                    // Hard SPDLOG — acid projectile spawn moment.
                    SPDLOG_INFO("[Dekubaba] acid projectile SPAWN — actor={} projectile={} spawn=({:.0f},{:.0f},{:.0f}) target=({:.0f},{:.0f},{:.0f}) vy={:.2f} tFlight={:.1f}f",
                                (void*)&actor->actor, (void*)projectile,
                                spawnPos.x, spawnPos.y, spawnPos.z,
                                targetPos.x, targetPos.y, targetPos.z,
                                vy, tFlight);
                }
            }
        }
        // Spawn SFX — "eruption cloud" fires at the moment the
        // projectile launches.
        EnhancementAudio::PlayDoubledActorSfx(
            &actor->actor, NA_SE_EV_ERUPTION_CLOUD);
    }
}

void EnDekubabaDescriptor::OnAttackComplete(EnDekubaba* actor) {
    if (actor == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    DekubabaEnhancedState& state = it->second;

    // UNIFIED PATTERN (per user 2026-07-31): all three ChargeState
    // machines advance at attack-complete regardless of which fired.
    // Always-advance prevents starvation between competing exotics.
    state.acidCharge.OnAttackComplete();
    state.detachCharge.OnAttackComplete();
    state.seedCharge.OnAttackComplete();

    state.currentAttackIsAcid   = false;
    state.acidProjectileSpawned = false;
    state.acidAttackFrame       = 0;
    state.currentAttackIsSeed   = false;
    state.seedProjectileSpawned = false;
    state.seedAttackFrame       = 0;
    // Log-820 Bug 2a fix — clear seed mouth visual on cycle complete.
    state.showSeedInMouth       = false;

    // Diagnostic — three-machine counter summary after advancement.
    // Gated on DebugLog CVar to avoid per-attack spam in normal play.
    DEKUBABA_DBG("OnAttackComplete counters: acid={}%({}) seed={}%({}) detach={}%({})",
                 state.acidCharge.GetCounter() * 25,
                 (int)state.acidCharge.GetState(),
                 state.seedCharge.GetCounter() * 25,
                 (int)state.seedCharge.GetState(),
                 state.detachCharge.GetCounter() * 25,
                 (int)state.detachCharge.GetState());

    // Restore scale to vanilla in case telegraph was mid-render at
    // cycle end (defensive; the AcidVomit exit path should also
    // reset).
    Actor_SetScale(&actor->actor, actor->size * 0.01f);
}

bool EnDekubabaDescriptor::IsCurrentAttackAcid(EnDekubaba* actor) {
    if (actor == nullptr) return false;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return false;
    return it->second.currentAttackIsAcid;
}

bool EnDekubabaDescriptor::IsAcidCharged(EnDekubaba* actor) {
    if (actor == nullptr) return false;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return false;
    return it->second.acidCharge.IsReady();
}

void EnDekubabaDescriptor::OnDeath(EnDekubaba* actor) {
    if (actor == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    DekubabaEnhancedState& state = it->second;
    SPDLOG_INFO("[Dekubaba] OnDeath — actor={} wasDetached={} spawnedByEnhance={}",
                (void*)&actor->actor, state.isDetached,
                state.isSpawnedByEnhancement);
    state.currentAttackIsAcid   = false;
    state.acidProjectileSpawned = false;
    state.acidAttackFrame       = 0;
    state.acidCharge.Reset();
    state.netAcidActive         = false;
    state.netAcidCharged        = false;
    // Feature B — detach state reset. Note: for vanilla Dekubaba this
    // reset doesn't take effect because Dekubaba doesn't regrow (dies
    // and stays dead until scene reload). OnActorDestroy erases the
    // state entry entirely. Kept for symmetry + safety in case the
    // reset is ever needed for a regrow variant.
    state.isDetached         = false;
    state.detachCharge.Reset();
    state.squirmFrameCounter = 0;
    state.lastBleedoutFrame  = 0;
    state.dyingFrameCounter  = 0;
    state.netDetachActive    = false;
    // Feature C — seed state reset.
    state.seedCharge.Reset();
    state.currentAttackIsSeed   = false;
    state.seedProjectileSpawned = false;
    state.seedAttackFrame       = 0;
    state.spawnedChildActor     = nullptr;
    state.netSeedActive         = false;
    // Log-820 Bug 2a fix — clear seed mouth visual on death.
    state.showSeedInMouth       = false;
    // Note: isSpawnedByEnhancement is NOT reset — it's a sticky
    // per-life property of the actor identity (was this Dekubaba
    // spawned by a seed?), unaffected by death.
    // Restore vanilla scale.
    Actor_SetScale(&actor->actor, actor->size * 0.01f);
}

void EnDekubabaDescriptor::OnActorDestroy(EnDekubaba* actor) {
    if (actor == nullptr) return;
    sStates.erase(&actor->actor);
}

// ---- Feature B (#309) — detach + pursue --------------------------

bool EnDekubabaDescriptor::OnHostMaybeDetach(EnDekubaba* actor, PlayState* play) {
    if (actor == nullptr) return false;

    // Sync-rule 1 — host is sole RNG decider.
    if (!SceneAuthority::IsMyCurrentRoomHost()) return false;

    // CVar gate (host-authoritative via enforced registry).
    if (AnchorCVarSync::GetEnforcedInt(DetachAndPursueCVarName(), 0) == 0) {
        return false;
    }

    // One-shot per life — already detached, don't roll again.
    DekubabaEnhancedState& state = GetOrCreate(actor);
    if (state.isDetached) return false;

    // UNIFIED PATTERN (per user 2026-07-31): no range gate. Roll
    // purely on charge counter (matches acid + seed shape).
    if (state.detachCharge.IsReady()) {
        state.detachCharge.OnFire();
        state.isDetached         = true;
        state.squirmFrameCounter = 0;
        state.lastBleedoutFrame  = (int)play->gameplayFrames;
        SPDLOG_INFO("[Dekubaba] detach FIRE decision — actor={} (one-shot per life)",
                    (void*)&actor->actor);
        return true;
    }

    const u8 counterBefore = state.detachCharge.GetCounter();
    if (state.detachCharge.TryCharge()) {
        DEKUBABA_DBG("detach CHARGED — counter {} → Ready", counterBefore);
    } else {
        DEKUBABA_DBG("detach rolling — counter {}%",
                     counterBefore * 25);
    }
    return false;
}

void EnDekubabaDescriptor::OnPeerReceiveDetachActiveFlag(EnDekubaba* actor,
                                                          bool active) {
    if (actor == nullptr) return;
    DekubabaEnhancedState& state = GetOrCreate(actor);
    const bool wasActive = state.netDetachActive;
    state.netDetachActive = active;
    if (wasActive != active) {
        SPDLOG_INFO("[Dekubaba] peer RECV detach active: {} → {} actor={}",
                    wasActive, active, (void*)&actor->actor);
    }
    // Mirror into isDetached so peer's Draw / OnDetachedSquirmTick
    // read the right value regardless of authority origin.
    if (active && !state.isDetached) {
        state.isDetached         = true;
        state.squirmFrameCounter = 0;
        SPDLOG_INFO("[Dekubaba] peer MIRRORED isDetached=true (from wire) actor={}",
                    (void*)&actor->actor);
    }
}

void EnDekubabaDescriptor::OnDetachedSquirmTick(EnDekubaba* actor,
                                                  PlayState* play) {
    if (actor == nullptr || play == nullptr) return;
    DekubabaEnhancedState& state = GetOrCreate(actor);

    // Serpentine motion — sine wave on stem angles with 120° phase
    // offset per segment. Feeds vanilla EnDekubaba_UpdateHeadPosition
    // (which the C actionFunc calls after this tick) → head bobs
    // wave-like.
    const float phase = (float)state.squirmFrameCounter * kSquirmPhasePerFrame;
    actor->stemSectionAngle[0] = kSquirmStemBase +
        (s16)(sinf(phase) * (float)kSquirmStemAmplitude);
    actor->stemSectionAngle[1] = kSquirmStemBase +
        (s16)(sinf(phase + (float)M_PI * 2.0f / 3.0f) *
              (float)kSquirmStemAmplitude);
    actor->stemSectionAngle[2] = kSquirmStemBase +
        (s16)(sinf(phase + (float)M_PI * 4.0f / 3.0f) *
              (float)(kSquirmStemAmplitude / 2));  // tail tapered

    // Face nearest player + move forward.
    Actor* target = FindNearestPlayerActor(&actor->actor, play);
    if (target != nullptr) {
        const s16 targetYaw = Math_Vec3f_Yaw(&actor->actor.world.pos,
                                              &target->world.pos);
        Math_ScaledStepToS(&actor->actor.shape.rot.y, targetYaw, 0x400);
    }

    // 2026-08-03 (user) — horizontal squirm modulation on shape.rot.y.
    // Adds a sine wave of same magnitude as the vertical stem-section
    // squirm (kSquirmStemAmplitude) on top of the target-facing yaw.
    // Result: plant head yaws side-to-side while inch-worming toward
    // Link — snake-like slither instead of straight-line pursuit.
    //
    // Because world.rot.y is derived from shape.rot.y below, the
    // velocity direction inherits the yaw wiggle → the plant physically
    // zigzags along the path to Link. Ground-follow snap keeps Y flat.
    //
    // 90° phase offset from vertical wave so the head "leads with the
    // side that's peaked" — natural snake gait vs. mechanical
    // in-phase wiggle.
    const s16 yawWiggle =
        (s16)(sinf(phase + (float)M_PI / 2.0f) * (float)kSquirmStemAmplitude);
    actor->actor.shape.rot.y += yawWiggle;

    // world.rot.y follows shape.rot.y so both draw + velocity share
    // the wiggle.
    actor->actor.world.rot.y = actor->actor.shape.rot.y;

    actor->actor.speedXZ  = kSquirmSpeedXZ;
    actor->actor.velocity.x = Math_SinS(actor->actor.world.rot.y) *
                               kSquirmSpeedXZ;
    actor->actor.velocity.z = Math_CosS(actor->actor.world.rot.y) *
                               kSquirmSpeedXZ;

    // Ground-follow — snap Y to floor each tick so the squirming form
    // stays on ground even on gentle slopes. Uses shared helper. Body
    // offset 0 (Dekubaba head-base sits at floor level naturally).
    GroundFollow::ProbeAndSnap(&actor->actor, play, /*bodyOffset=*/0.0f);

    // Position + velocity telemetry every 20 frames (~1s @ 20fps).
    if ((state.squirmFrameCounter % 20) == 0) {
        DEKUBABA_DBG("squirm@f{} pos=({:.0f},{:.0f},{:.0f}) vel=({:.1f},{:.1f}) hp={}",
                     state.squirmFrameCounter,
                     actor->actor.world.pos.x,
                     actor->actor.world.pos.y,
                     actor->actor.world.pos.z,
                     actor->actor.velocity.x,
                     actor->actor.velocity.z,
                     actor->actor.colChkInfo.health);
    }

    // Bleedout — host-only decrement HP every 5 seconds. Peer sees
    // the health change via ENEMY_STATE health field naturally.
    if (SceneAuthority::IsMyCurrentRoomHost() && Anchor::Instance != nullptr) {
        const int nowFrame = (int)play->gameplayFrames;
        // Defensive-seed on first squirm frame — protects against
        // callers that entered DetachedSquirm without seeding
        // lastBleedoutFrame (stem-cut detach + seed-child spawn
        // paths both hit this). Without the seed, `nowFrame - 0`
        // trivially exceeds intervalTicks on frame 0 and drops HP
        // immediately, killing HP=1 forms before Link can react.
        if (state.squirmFrameCounter == 0) {
            state.lastBleedoutFrame = nowFrame;
        }
        const int intervalTicks = Anchor::Instance->MsToGameTicks(
                                    kBleedoutIntervalMs);
        if (intervalTicks > 0 &&
            (nowFrame - state.lastBleedoutFrame) >= intervalTicks) {
            state.lastBleedoutFrame = nowFrame;
            if (actor->actor.colChkInfo.health > 0) {
                actor->actor.colChkInfo.health--;
            }
        }
    }

    state.squirmFrameCounter++;
}

void EnDekubabaDescriptor::OnDetachedDyingTick(EnDekubaba* actor,
                                                 PlayState* play) {
    if (actor == nullptr || play == nullptr) return;
    (void)play;
    DekubabaEnhancedState& state = GetOrCreate(actor);
    state.dyingFrameCounter++;
    // Actor_Kill decision + drop spawning stays in the vanilla C
    // actionFunc — this tick just increments the counter so the C
    // side can time animation length. Vanilla ShrinkDie animation
    // handles the shrink visual; the C side plays it via
    // Animation_Change with reverse playSpeed.
}

bool EnDekubabaDescriptor::IsDetached(EnDekubaba* actor) {
    if (actor == nullptr) return false;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return false;
    return it->second.isDetached;
}

bool EnDekubabaDescriptor::OnHostMaybeStemCutDetach(EnDekubaba* actor) {
    if (actor == nullptr) return false;

    // Sync-rule 1 — host is sole RNG decider.
    if (!SceneAuthority::IsMyCurrentRoomHost()) return false;

    // CVar gate — reuses DetachAndPursue CVar (same feature).
    if (AnchorCVarSync::GetEnforcedInt(DetachAndPursueCVarName(), 0) == 0) {
        return false;
    }

    // One-shot per life — already detached, can't detach again.
    DekubabaEnhancedState& state = GetOrCreate(actor);
    if (state.isDetached) return false;

    // 25% fixed chance (not ramped — this is a "last-stand" surprise
    // per user 2026-08-02 request "add a 25% chance that instead of
    // dying, the deku baba enters its detached state" when stem cut).
    if (Rand_ZeroOne() >= 0.25f) return false;

    // Success — set sticky flag + restore HP so bleedout timer has
    // time to run.
    // 2026-08-02 (Change 1): HP=1 per user request "Use 1hp/1 hit to
    // kill instead." Prior 2 HP let bleedout tick twice (~10s squirm)
    // before death; 1 HP means first bleedout tick (5s) OR any Link
    // hit kills the last-stand form. Balance: shorter squirm window
    // but the surprise/reposition value remains.
    state.isDetached         = true;
    state.squirmFrameCounter = 0;
    actor->actor.colChkInfo.health = 1;
    SPDLOG_INFO("[Dekubaba] stem-cut DETACH triggered — actor={} HP restored to {}",
                (void*)&actor->actor, actor->actor.colChkInfo.health);
    DEKUBABA_DBG("stem-cut detach FIRE — last-stand squirm");
    return true;
}

// ---- Feature C (#318) — seed spawn --------------------------------

namespace {

// Compute the "behind Link" landing target per user 2026-07-31 spec:
// Link.pos + Link.forward * kSeedBehindLinkOffset. If that position
// is beyond kSeedMaxFlightDistance from the Dekubaba, clamp along
// Dekubaba-to-target bearing to max distance.
Vec3f ComputeBehindLinkTarget(EnDekubaba* actor, Actor* target) {
    Vec3f behind;
    if (target == nullptr) {
        behind = actor->actor.world.pos;
        return behind;
    }
    // Bug 10 fix (2026-08-02) — was `+ Math_SinS(yaw)` which moves in
    // Link's FORWARD direction (child spawned AHEAD of Link). User
    // wants BEHIND Link so player is forced to reposition when a
    // child appears in their blind spot. Negate the offset for
    // backward direction.
    const s16 linkYaw = target->world.rot.y;
    behind.x = target->world.pos.x -
               Math_SinS(linkYaw) * kSeedBehindLinkOffset;
    behind.y = target->world.pos.y;
    behind.z = target->world.pos.z -
               Math_CosS(linkYaw) * kSeedBehindLinkOffset;

    // Clamp to max flight distance from Dekubaba.
    const float dx = behind.x - actor->actor.world.pos.x;
    const float dz = behind.z - actor->actor.world.pos.z;
    const float distXZ = sqrtf(dx * dx + dz * dz);
    if (distXZ > kSeedMaxFlightDistance && distXZ > 0.001f) {
        const float ratio = kSeedMaxFlightDistance / distXZ;
        behind.x = actor->actor.world.pos.x + dx * ratio;
        behind.z = actor->actor.world.pos.z + dz * ratio;
    }
    return behind;
}

// Fallback landing: random XZ within kSeedFallbackRadius of Dekubaba.
Vec3f RandomFallbackTarget(EnDekubaba* actor) {
    const float u = Rand_ZeroOne();
    const float r = kSeedFallbackRadius * sqrtf(u);
    const float ang = Rand_ZeroOne() * 2.0f * (float)M_PI;
    Vec3f target = {
        actor->actor.world.pos.x + r * cosf(ang),
        actor->actor.world.pos.y,
        actor->actor.world.pos.z + r * sinf(ang),
    };
    return target;
}

// Check whether parent's spawnedChildActor is still alive. Uses
// actor pointer validity check — Actor_Kill sets actor->update to
// nullptr (session_state Fix 12 pattern).
bool IsChildAlive(Actor* childActor) {
    return childActor != nullptr && childActor->update != nullptr;
}

}  // namespace

bool EnDekubabaDescriptor::OnHostMaybeSeedFire(EnDekubaba* actor,
                                                 PlayState* play) {
    if (actor == nullptr || play == nullptr) return false;

    // Sync-rule 1 — host is sole RNG decider.
    if (!SceneAuthority::IsMyCurrentRoomHost()) return false;

    // CVar gate.
    if (AnchorCVarSync::GetEnforcedInt(SeedSpawnCVarName(), 0) == 0) {
        return false;
    }

    DekubabaEnhancedState& state = GetOrCreate(actor);
    DEKUBABA_DBG("seed rolled — childAlive={} spawnedByEnhance={} counter={}%",
                 IsChildAlive(state.spawnedChildActor),
                 state.isSpawnedByEnhancement,
                 state.seedCharge.GetCounter() * 25);

    // Child-not-active gate (per user Q3 2026-07-31: parent tracks
    // own child only). Skip if own child is still alive.
    if (IsChildAlive(state.spawnedChildActor)) {
        DEKUBABA_DBG("seed BLOCKED — own child alive");
        return false;
    }
    // Clear stale pointer if the child died.
    if (state.spawnedChildActor != nullptr && !IsChildAlive(state.spawnedChildActor)) {
        state.spawnedChildActor = nullptr;
    }

    // Suppress on seed-spawned children (per user Q3): children
    // can run A + B but not C. Future toggle CVar
    // gEnhancements.Dekubaba.SeedChildrenCanSeed lets user override.
    if (state.isSpawnedByEnhancement) {
        DEKUBABA_DBG("seed BLOCKED — this actor is a spawnedByEnhancement child");
        return false;
    }

    // Ready branch: fire seed if state machine says Ready.
    if (state.seedCharge.IsReady()) {
        // Compute landing target — behind-Link primary, random
        // fallback if nav-invalid.
        Actor* target = FindNearestPlayerActor(&actor->actor, play);
        Vec3f landingTarget = ComputeBehindLinkTarget(actor, target);

        // Nav-validate. If failure, fallback to random XZ within
        // kSeedFallbackRadius of Dekubaba (per user Q3 2026-07-31:
        // never skip fire).
        // Note: nav-validation requires per-scene RoomNavData scan.
        // If room isn't scanned or lookup fails, we still fire —
        // fallback ensures the spawn happens somewhere reasonable.
        // TODO: wire RoomNavData::Instance query when the substrate
        // is confirmed populated. For v1 we skip nav-validation and
        // trust the landing target directly — a follow-up will add
        // proper AnchorNavRoom::FindNearestFloorNodeXZRadius call
        // once the RoomNavData accessor is verified.
        // Random fallback branch (always fire per user Q3):
        // if (nav-invalid) landingTarget = RandomFallbackTarget(actor);
        (void)RandomFallbackTarget;  // reference for future use

        state.seedCharge.OnFire();
        state.currentAttackIsSeed   = true;
        state.seedProjectileSpawned = false;
        state.seedAttackFrame       = 0;
        state.seedLandingPos        = landingTarget;

        // Hard SPDLOG (unconditional, not gated on DebugLog CVar)
        // so any log will show that seed fired. Investigation aid
        // for 2026-08-02 report "haven't seen seed fire".
        SPDLOG_INFO("[Dekubaba] seed FIRE decision — landing=({:.0f},{:.0f},{:.0f}) actor={}",
                     landingTarget.x, landingTarget.y, landingTarget.z,
                     (void*)&actor->actor);
        DEKUBABA_DBG("seed FIRE — landing=({:.0f},{:.0f},{:.0f})",
                     landingTarget.x, landingTarget.y, landingTarget.z);
        EnhancementAudio::PlayBoostedActorSfx(
            &actor->actor, NA_SE_EV_WATER_BUBBLE);
        return true;
    }

    // Charging branch.
    const u8 counterBefore = state.seedCharge.GetCounter();
    if (state.seedCharge.TryCharge()) {
        DEKUBABA_DBG("seed CHARGED — counter {} → Ready", counterBefore);
    }
    return false;
}

void EnDekubabaDescriptor::OnPeerReceiveSeedActiveFlag(EnDekubaba* actor,
                                                         bool active) {
    if (actor == nullptr) return;
    DekubabaEnhancedState& state = GetOrCreate(actor);
    const bool wasActive = state.netSeedActive;
    state.netSeedActive = active;
    if (wasActive != active) {
        SPDLOG_INFO("[Dekubaba] peer RECV seed active: {} → {} actor={}",
                    wasActive, active, (void*)&actor->actor);
    }
    if (active) {
        state.currentAttackIsSeed   = true;
        state.seedProjectileSpawned = false;
        state.seedAttackFrame       = 0;
    }
}

void EnDekubabaDescriptor::OnPeerReceiveSeedLandingPos(EnDekubaba* actor,
                                                        float x, float y,
                                                        float z) {
    if (actor == nullptr) return;
    DekubabaEnhancedState& state = GetOrCreate(actor);
    // Log only when landing pos changes meaningfully (>1u any axis)
    // to avoid per-tick spam. Landing coord is set once per fire
    // decision on host, so transitions are rare.
    const bool changed =
        fabsf(state.seedLandingPos.x - x) > 1.0f ||
        fabsf(state.seedLandingPos.y - y) > 1.0f ||
        fabsf(state.seedLandingPos.z - z) > 1.0f;
    state.seedLandingPos = { x, y, z };
    if (changed) {
        SPDLOG_INFO("[Dekubaba] peer RECV seed landing: ({:.0f},{:.0f},{:.0f}) actor={}",
                    x, y, z, (void*)&actor->actor);
    }
}

void EnDekubabaDescriptor::OnSeedTelegraphTick(EnDekubaba* actor,
                                                  PlayState* play, int frame) {
    // Log-820 Bug 2a fix (2026-08-02) — seed uses DISTINCT visual
    // from acid (nut model in mouth instead of green spit+bubbles).
    // Prior implementation reused RenderAcidTelegraph — user's
    // feedback: "the seed attack uses the acid/vomit/water tinted
    // green particle effect and bubbles. This effect is meant to
    // display that the acid attack is ready to use, not the seed
    // attack. To signify the seed attack is ready to use the deku
    // seed model should appear in the deku baba mouth."
    if (actor == nullptr || play == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    DekubabaEnhancedState& state = it->second;

    if (frame <= kSeedTelegraphEndFrame) {
        // Enlarge head to indicate incoming attack (matches acid's
        // 1.25× scale — visible signal that Dekubaba is winding up).
        const f32 baseScale = actor->size * 0.01f;
        Actor_SetScale(&actor->actor, baseScale * kTelegraphHeadScale);
        // Flag draw hook to render the nut in the mouth. Cleared by
        // OnEveryFrameTick each tick + re-set here.
        state.showSeedInMouth = true;
    }
}

void EnDekubabaDescriptor::OnSeedFireTick(EnDekubaba* actor,
                                            PlayState* play, int frame) {
    if (actor == nullptr || play == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    DekubabaEnhancedState& state = it->second;

    if (!state.seedProjectileSpawned && frame >= kSeedFireFrame) {
        state.seedProjectileSpawned = true;
        // Log-820 Bug 2a fix — nut launches; mouth no longer holds it.
        // Explicit reset here so the visual disappears at the exact
        // frame the projectile spawns (OnSeedTelegraphTick stops
        // firing after SeedFire actionFunc takes over).
        state.showSeedInMouth = false;
        if (gEnDekubabaSeedId != 0) {
            // Spawn projectile at head; encode landing target in
            // actor spawn coordinates so the projectile can compute
            // its own straight-line velocity. Landing pos is shipped
            // via ENEMY_STATE wire field so peer's projectile arrives
            // at the same coord.
            //
            // Params encode: high 4 bits = discriminator (0 for
            // seed variant), low 12 bits = landing distance / 4u
            // quantized (400u max → 100 max quantized). Actor's Init
            // reads world.pos (spawn source) + delta from home.rot
            // (target vector packed) — simpler approach: pass full
            // Vec3f via new Anchor_SpawnDekubabaSeed helper (extern
            // "C" from actor's C code).
            //
            // For v1 simplicity: spawn actor at head pos, pass
            // landing X/Z via the actor's params + world.rot fields.
            // Actor computes trajectory in its Init.
            const Vec3f spawnPos = actor->actor.world.pos;
            const float dx = state.seedLandingPos.x - spawnPos.x;
            const float dy = state.seedLandingPos.y - spawnPos.y;
            const float dz = state.seedLandingPos.z - spawnPos.z;
            const s16 aimYaw = Math_Atan2S(dz, dx);
            // Note: Math_Atan2S expects (z, x) order for OoT convention.
            const s16 params = (s16)(aimYaw / 8);
            // Bug 8 fix (2026-08-02) — bracket Actor_Spawn with the
            // pending-parent thread-local so seed actor's Init picks
            // up parentDekubaba pointer. Without this, OnSeedLanded
            // early-returned on null parent → no child spawned.
            g_pendingSeedProjectileParent = &actor->actor;
            Actor* projectile = Actor_Spawn(&play->actorCtx, play,
                                              gEnDekubabaSeedId,
                                              spawnPos.x, spawnPos.y, spawnPos.z,
                                              0, aimYaw, 0, params);
            g_pendingSeedProjectileParent = nullptr;
            // Bug 2 fix (2026-08-02) — parabolic arc trajectory.
            // The prior straight-line vector had the seed spend most
            // of its flight time near the ground (spawn Y=49 dropping
            // linearly to Y=0 over ~0.5s means Y<20 for half the arc),
            // and combined with the stub Draw + trail-only visual it
            // read as "seed only appears on the floor."
            //
            // New arc: solve ballistic vy so the projectile lofts to
            // spawnY + 40u peak before descending. Fixed XZ speed,
            // constant gravity — mirror of the acid projectile solver
            // (Bug 4 fix pattern).
            //
            //   Given horizontal distance distXZ + horizontal speed
            //   kSeedXZSpeed, tFlight = distXZ / kSeedXZSpeed.
            //   Peak height above spawn = kSeedArcHeight.
            //   For projectile to reach yTarget at tFlight with peak
            //   at kSeedArcHeight, solve:
            //     yTarget = y0 + vy*t + 0.5*g*t²
            //   Solved for vy:
            //     vy = (yTarget - y0 - 0.5*g*t²) / t
            //   Additionally boost vy so peak = y0 + kSeedArcHeight:
            //     vyMin = sqrt(2 * |g| * kSeedArcHeight)
            //     vy = max(vyMin, ballistic_vy)
            if (projectile != nullptr) {
                constexpr float kSeedXZSpeed   = 8.0f;    // slower than
                                                          // straight-line
                                                          // for readability
                constexpr float kSeedGravity   = -1.0f;   // matches acid
                constexpr float kSeedArcHeight = 40.0f;   // peak above spawn
                constexpr float kMinFlightF    = 8.0f;
                constexpr float kMaxFlightF    = 60.0f;
                const float distXZ = sqrtf(dx * dx + dz * dz);
                if (distXZ > 0.001f) {
                    float tFlight = distXZ / kSeedXZSpeed;
                    if (tFlight < kMinFlightF) tFlight = kMinFlightF;
                    if (tFlight > kMaxFlightF) tFlight = kMaxFlightF;
                    // Ballistic vy to reach target dy at tFlight.
                    const float ballisticVy =
                        (dy - 0.5f * kSeedGravity * tFlight * tFlight)
                        / tFlight;
                    // Minimum vy that guarantees arc peak = arcHeight.
                    const float minVyForArc =
                        sqrtf(2.0f * (-kSeedGravity) * kSeedArcHeight);
                    const float vy = (ballisticVy > minVyForArc)
                                       ? ballisticVy
                                       : minVyForArc;
                    // Unit XZ direction × horizontal speed.
                    const float invDistXZ = 1.0f / distXZ;
                    projectile->velocity.x = dx * invDistXZ * kSeedXZSpeed;
                    projectile->velocity.z = dz * invDistXZ * kSeedXZSpeed;
                    projectile->velocity.y = vy;
                    projectile->speedXZ    = kSeedXZSpeed;
                    projectile->gravity    = kSeedGravity;
                    SPDLOG_INFO("[Dekubaba] seed ballistic — distXZ={:.0f} dy={:.0f} tFlight={:.1f}f vy={:.2f}",
                                distXZ, dy, tFlight, vy);
                }
            }
            SPDLOG_INFO("[Dekubaba] seed projectile spawn — actor={} projectile={} yaw={} landing=({:.0f},{:.0f},{:.0f})",
                        (void*)&actor->actor, (void*)projectile, (int)aimYaw,
                        state.seedLandingPos.x, state.seedLandingPos.y,
                        state.seedLandingPos.z);
        }
        EnhancementAudio::PlayDoubledActorSfx(
            &actor->actor, NA_SE_EV_ERUPTION_CLOUD);
    }
}

void EnDekubabaDescriptor::OnSeedLanded(EnDekubaba* parent, PlayState* play,
                                          float x, float y, float z) {
    // Bug 8 fix (2026-08-02): hard SPDLOG regardless of parent
    // validity so we can trace whether the callback is even reached.
    SPDLOG_INFO("[Dekubaba] seed LANDED — parent={} landing=({:.0f},{:.0f},{:.0f})",
                (void*)parent, x, y, z);
    if (play == nullptr) return;

    // Parent may be null when the seed projectile times out without
    // its Init having read the pending-parent thread-local (edge
    // case — should not happen in practice with Bug 8 fix). We can
    // still spawn the child; parent-tracking (spawnedChildActor)
    // just won't be updated.
    DekubabaEnhancedState* statePtr = nullptr;
    if (parent != nullptr) {
        auto it = sStates.find(&parent->actor);
        if (it != sStates.end()) {
            statePtr = &it->second;
        }
    }

    // Spawn a fresh EN_DEKUBABA at the landing coord. Bracket the
    // spawn with the g_isSpawningDekubabaChild thread-local so the
    // child's OnActorInit picks up the isSpawnedByEnhancement=true
    // flag.
    g_isSpawningDekubabaChild = true;
    Actor* childActor = Actor_Spawn(&play->actorCtx, play,
                                     ACTOR_EN_DEKUBABA,
                                     x, y, z,
                                     0, 0, 0,
                                     /*params=*/DEKUBABA_NORMAL);
    g_isSpawningDekubabaChild = false;

    SPDLOG_INFO("[Dekubaba] seed child SPAWNED — child={} parentTracked={}",
                (void*)childActor, statePtr != nullptr);

    // Track own child for the "child-not-active" gate.
    if (statePtr != nullptr) {
        statePtr->spawnedChildActor = childActor;
    }
}

void EnDekubabaDescriptor::OnActorInit(EnDekubaba* actor) {
    if (actor == nullptr) return;
    DekubabaEnhancedState& state = GetOrCreate(actor);
    // Pick up the sticky "spawned by parent's seed" flag from the
    // thread-local. Set true once at Init; never reset.
    if (g_isSpawningDekubabaChild) {
        state.isSpawnedByEnhancement = true;
        // Log-819 Bug 6 fix (2026-08-02) — REVERTED prior isDetached
        // setting. Seed-children now run the full vanilla Dekubaba
        // lifecycle (SetupWait → SetupGrow when player approaches →
        // active Dekubaba). See EnDekubaba_Init for full rationale.
        // The isSpawnedByEnhancement flag stays — used by Feature C's
        // "no seed for children" gate (prevents recursive seed-spawn).
        SPDLOG_INFO("[Dekubaba] OnActorInit — CHILD spawned by seed. actor={} isSpawnedByEnhancement=true (vanilla lifecycle)",
                    (void*)&actor->actor);
    } else {
        DEKUBABA_DBG("OnActorInit — natural spawn actor={}",
                     (void*)&actor->actor);
    }
}

bool EnDekubabaDescriptor::IsSeedActive(EnDekubaba* actor) {
    if (actor == nullptr) return false;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return false;
    return it->second.currentAttackIsSeed;
}

bool EnDekubabaDescriptor::IsSpawnedByEnhancement(EnDekubaba* actor) {
    if (actor == nullptr) return false;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return false;
    return it->second.isSpawnedByEnhancement;
}

float EnDekubabaDescriptor::GetSeedLandingX(EnDekubaba* actor) {
    if (actor == nullptr) return 0.0f;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return 0.0f;
    return it->second.seedLandingPos.x;
}
float EnDekubabaDescriptor::GetSeedLandingY(EnDekubaba* actor) {
    if (actor == nullptr) return 0.0f;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return 0.0f;
    return it->second.seedLandingPos.y;
}
float EnDekubabaDescriptor::GetSeedLandingZ(EnDekubaba* actor) {
    if (actor == nullptr) return 0.0f;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return 0.0f;
    return it->second.seedLandingPos.z;
}

// Change 3 (2026-08-02) — per-frame ready-state bubble accent.
// Fires while any of the three charge machines is Ready and the
// actor is NOT currently inside an active enhancement action state
// (AcidVomit/SeedTelegraph/SeedFire/DetachedSquirm/DetachedDying —
// each of those already renders its own telegraph or squirm visual;
// double-drawing would look busy).
//
// Detached: reads netAcidActive/netSeedActive/netDetachActive as
// "attack in flight" markers. Any true → skip ready bubble (attack
// visual owns the frame).
void EnDekubabaDescriptor::OnEveryFrameTick(EnDekubaba* actor,
                                              PlayState* play) {
    if (actor == nullptr || play == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    DekubabaEnhancedState& state = it->second;

    // Log-820 Bug 2a fix — clear seed-in-mouth flag every tick;
    // re-set below OR by OnSeedTelegraphTick when appropriate.
    // Ensures the visual disappears immediately when conditions
    // change (attack fires, charge cools down, etc.).
    state.showSeedInMouth = false;

    // Suppress while an attack is actively rendering its own visual.
    // netAcidActive covers acid + seed active-flag broadcast, and
    // isDetached covers both squirm + dying. currentAttackIsSeed
    // covers SeedTelegraph pre-fire window (but SeedTelegraph itself
    // sets showSeedInMouth via OnSeedTelegraphTick).
    if (state.isDetached) return;
    if (state.currentAttackIsAcid || state.netAcidActive) return;
    if (state.currentAttackIsSeed || state.netSeedActive) return;

    // Log-820 Bug 2a fix — split visual routing per attack type.
    // Priority follows DecideLunge order: acid → seed → detach → vanilla.
    // Show visual of whichever attack fires NEXT so player anticipates.
    const bool acidReady =
        state.acidCharge.IsReady() || state.netAcidCharged;
    const bool seedReady = state.seedCharge.IsReady();
    const bool detachReady = state.detachCharge.IsReady();

    if (acidReady) {
        // Acid ready — green rising-bubble accent at head. Matches
        // acid attack's visual language (green liquid family).
        Vec3f bubblePos = actor->actor.world.pos;
        AcidVisuals::SpawnReadyBubbles(play, bubblePos);
    } else if (seedReady) {
        // Seed ready — Deku Nut model rendered at mouth (via draw
        // hook). Distinct from acid's green liquid — reads as
        // "physical projectile forthcoming."
        state.showSeedInMouth = true;
    } else if (detachReady) {
        // Detach ready — no visual (subtle/surprise per design).
        // Detach is a defensive/reactive attack; telegraphing it
        // would eliminate the surprise. No-op.
        (void)detachReady;
    }
}

// Log-820 Bug 2a fix (2026-08-02) — draw-time hook, called from
// EnDekubaba_Draw after the vanilla skeleton + stem render.
// Splices additional DLists based on descriptor state — currently
// the Deku Nut sprite in the mouth when seed telegraph or seed-ready
// state is active. Uses gameplay_keep DList (gItemDropDL +
// gDropDekuNutTex) which is always loaded, safe from any actor draw.
//
// Positioning: nut appears at head.pos + forward offset along
// shape.rot.y so it emerges from the mouth (matches the same
// mouth-forward direction used by Change 2's acid spit position).
void EnDekubabaDescriptor::OnDrawHook(EnDekubaba* actor, PlayState* play) {
    if (actor == nullptr || play == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    const DekubabaEnhancedState& state = it->second;

    if (!state.showSeedInMouth) return;

    // Position: head.pos + forward offset (mouth is a bit in front
    // of the head-tip center). Small downward bias so nut appears
    // resting between the "jaws" of the chomp anim.
    const s16 yaw = actor->actor.shape.rot.y;
    const float kMouthForward = 10.0f;
    const float kMouthDown    = 4.0f;
    const float posX = actor->actor.world.pos.x +
                        Math_SinS(yaw) * kMouthForward;
    const float posY = actor->actor.world.pos.y - kMouthDown;
    const float posZ = actor->actor.world.pos.z +
                        Math_CosS(yaw) * kMouthForward;
    // Nut sprite scale — proportional to Dekubaba head at 1.25×
    // telegraph scale (~55u * 0.6 = ~33u tall).
    const float kNutScale = actor->size * 0.006f;

    // Delegate to global-namespace splice (Pitfall 17). ::-prefix
    // required to escape the enclosing namespace lookup.
    ::EnDekubabaSpliceSeedInMouth(play, posX, posY, posZ, kNutScale);
}

}  // namespace AnchorEnemyEnhancement
