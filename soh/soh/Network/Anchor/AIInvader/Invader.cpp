/**
 * Invader — implementation.
 *
 * v1 (step 15a): scaffold + draw-context flag for hostile-black tint.
 *
 * Step 15d (combat clone) — combat state machine layered on top of
 * the scaffold. Cloned from NPC Follower Stage 4
 * (AIFollowerNPC/FollowerNPC.cpp) with these intentional deltas:
 *
 *   - Target is the nearest PLAYER actor (not nearest enemy). Uses
 *     Anchor_GetNearestPlayerActor as a v1 placeholder; Agent 4
 *     will replace with a multi-player picker that respects
 *     director-side state.
 *   - AT collider TYPE is AT_TYPE_ENEMY (Player AC bumpers are
 *     AC_TYPE_PLAYER, which accept ATs of TYPE_ENEMY). The Follower's
 *     equivalent sword AT uses AT_TYPE_PLAYER because Follower
 *     attacks enemies.
 *   - Equipment-visibility swap (Phase B from FollowerNPC) is NOT
 *     replicated here — Agent 1 owns that surface. Invader is
 *     assumed visually-armed.
 *   - Time-based equipment retention / sheathe-delay is omitted —
 *     Invader is always armed, so post-combat cycling does not
 *     produce visible flicker.
 *
 * TODO post-#208: revisit combat state shape against canonical
 * design pass (currently a verbatim clone of NPC Follower Stage 4).
 */

#include "Invader.h"

#include "soh/Network/Anchor/Anchor.h"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>
#include <algorithm>
#include <cmath>
#include <limits>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"
#include "objects/gameplay_keep/gameplay_keep.h"
#include "src/overlays/actors/ovl_En_Invader/z_en_invader.h"
#include "src/overlays/actors/ovl_En_Arrow/z_en_arrow.h"
extern PlayState* gPlayState;

// Target-picking placeholder. Agent 4 replaces this with a multi-
// player picker. Defined in HookHandlers.cpp as a thin wrapper
// around FindNearestPlayerActor.
Actor* Anchor_GetNearestPlayerActor(Actor* enemy, PlayState* play);
}

namespace {

// Set during EnInvader_Draw's Player_DrawImpl call; cleared after.
// Read by the VB_APPLY_TUNIC_COLOR hook to know which actor's tunic
// is being rendered, so it can apply the black-tint override. File-
// scope static rather than class member because actor draws don't
// nest — the gfx context is single-threaded and each draw completes
// before the next starts.
static Actor* sCurrentlyDrawingInvader = nullptr;

// ---------------------------------------------------------------------
// Combat tuning constants (cloned from FollowerNPC).
// ---------------------------------------------------------------------
constexpr float kAttackEngageDist     = 80.0f;
constexpr float kAttackQuadForward    = 60.0f;
constexpr float kAttackQuadHalfWidth  = 25.0f;
constexpr float kAttackQuadBaseY      = 5.0f;
constexpr float kAttackQuadTopY       = 65.0f;
constexpr float kAttackActiveStartFrame = 4.0f;
constexpr float kAttackActiveEndFrame   = 12.0f;

constexpr float kEngageAcquireDist  = 250.0f;
constexpr float kEngageBreakDist    = 400.0f;
constexpr float kEngageStrikeDist   = 70.0f;
constexpr float kEngageWalkSpeed    = 6.0f;
constexpr float kEngageRunDistance  = 150.0f;
constexpr float kEngageRunSpeed     = 12.0f;

constexpr int   kBlockDurationMs        = 2000;
constexpr float kBlockHpThresholdRatio  = 0.5f;
constexpr int   kBlockFrontalAngle      = 0x4000;  // ±90° in s16

constexpr float kRangedMinDist     = 90.0f;
constexpr float kRangedAcquireDist = 500.0f;
constexpr float kRangedBreakDist   = 800.0f;
constexpr float kRangedSpawnFrame  = 5.0f;
constexpr float kRangedSpawnHeightY = 50.0f;
constexpr float kRangedYFilter     = 250.0f;
constexpr float kRangedElevatedYDelta = 60.0f;

constexpr float kStandbyDetectDist = 600.0f;
// Idle leader-leash radius — STANDBY drops back to IDLE inside this.
constexpr float kStandbyIdleRadius = 80.0f;

constexpr int   kPostCombatCooldownMs = 1500;
static uint64_t sCombatCooldownEndFrame = 0;
// Last combat weapon. 0 = melee (sword); 1 = ranged. Set at every
// combat entry by TryEngageCombat; consumed by STANDBY anim/facing
// preference (no equipment swap here — Agent 1).
static s32 sLastCombatWeapon = 0;

// Per-swing / per-shot state. File-scope is safe because actor combat
// states are non-reentrant per actor (the dispatcher runs at most one
// state handler per actor per tick) and there is one Invader per
// dispatcher invocation. If multiple Invaders share one dispatcher
// in a future revision, this state moves onto EnInvader.
struct AttackState {
    Actor*   target = nullptr;
    bool     swingFiredAT = false;
    uint64_t entryFrame = 0;       // gameFrameCounter at last ATTACK/RANGED_ATTACK entry
};
static AttackState sAttackState;

// Minimum ticks any swing/shot state must hold before the
// curFrame>=endFrame anim-completion check is allowed to exit. This
// guards against a stale endFrame from the previous state's anim
// being non-zero when ATTACK/RANGED_ATTACK is entered — without the
// guard, the very first tick would early-exit because (curFrame >=
// endFrame) is true from leftover idle anim state. Matches roughly
// the kSwordSwing + kBowShoot anim length in ticks.
constexpr int kMinSwingHoldTicks = 6;

struct BlockState {
    uint64_t entryFrame    = 0;
    uint32_t hitAnimFrames = 0;
};
static BlockState sBlockState;

// ---------------------------------------------------------------------
// Small math helpers (private clones — same shape as
// FollowerNPC's free functions but kept inside this TU so we don't
// reach into FollowerNPC.cpp's anonymous namespace).
// ---------------------------------------------------------------------
inline float Dist2DSq(const Vec3f& a, const Vec3f& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return dx * dx + dz * dz;
}

inline s16 YawTowardTarget(const Vec3f& from, const Vec3f& to) {
    return Math_Atan2S(to.z - from.z, to.x - from.x);
}

const char* StateName(s32 s) {
    switch (s) {
        case EN_INVADER_STATE_IDLE:          return "IDLE";
        case EN_INVADER_STATE_FOLLOW:        return "FOLLOW";
        case EN_INVADER_STATE_STUCK:         return "STUCK";
        case EN_INVADER_STATE_DEAD:          return "DEAD";
        case EN_INVADER_STATE_ATTACK:        return "ATTACK";
        case EN_INVADER_STATE_ENGAGE:        return "ENGAGE";
        case EN_INVADER_STATE_BLOCK:         return "BLOCK";
        case EN_INVADER_STATE_RANGED_ATTACK: return "RANGED_ATTACK";
        case EN_INVADER_STATE_STANDBY:       return "STANDBY";
        default:                             return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------
// Target picking — placeholder hook for Agent 4. The Invader's
// hostile-target source is the local-player actor (or the nearest
// DummyPlayer) discovered by Anchor_GetNearestPlayerActor.
//
// Agent 4 integration point: replace this body with a multi-player
// picker that consults director-side state (e.g. which players are
// in scope for this invader, which are out-of-timeline, etc.).
// Caller validates result is non-null + alive before use.
// ---------------------------------------------------------------------
Actor* PickHostileTarget(Actor* self, PlayState* play, float maxRange,
                         float maxYDelta = 60.0f) {
    Actor* candidate = Anchor_GetNearestPlayerActor(self, play);
    if (candidate == nullptr || candidate->update == nullptr) return nullptr;
    const float dx = candidate->world.pos.x - self->world.pos.x;
    const float dz = candidate->world.pos.z - self->world.pos.z;
    const float dy = candidate->world.pos.y - self->world.pos.y;
    if (std::fabs(dy) > maxYDelta) return nullptr;
    const float distSq = dx * dx + dz * dz;
    if (distSq > maxRange * maxRange) return nullptr;
    return candidate;
}

// ---------------------------------------------------------------------
// AT quad positioning — flat plane in front of the Invader at chest
// height. Same vertex order as FollowerNPC's PositionAttackQuad.
// ---------------------------------------------------------------------
void PositionAttackQuad(EnInvader* this_) {
    Actor* a = &this_->actor;
    const float yawRad = (float)a->shape.rot.y * (3.14159265f / 32768.0f);
    const float fx = sinf(yawRad);
    const float fz = cosf(yawRad);
    const float rx = cosf(yawRad);
    const float rz = -sinf(yawRad);
    const Vec3f& p = a->world.pos;
    Vec3f bottomLeft  = { p.x + fx * kAttackQuadForward - rx * kAttackQuadHalfWidth,
                          p.y + kAttackQuadBaseY,
                          p.z + fz * kAttackQuadForward - rz * kAttackQuadHalfWidth };
    Vec3f bottomRight = { p.x + fx * kAttackQuadForward + rx * kAttackQuadHalfWidth,
                          p.y + kAttackQuadBaseY,
                          p.z + fz * kAttackQuadForward + rz * kAttackQuadHalfWidth };
    Vec3f topRight    = { bottomRight.x, p.y + kAttackQuadTopY, bottomRight.z };
    Vec3f topLeft     = { bottomLeft.x,  p.y + kAttackQuadTopY, bottomLeft.z };
    Collider_SetQuadVertices(&this_->atCollider, &bottomLeft, &bottomRight,
                             &topLeft, &topRight);
}

bool IsFrontalAttacker(EnInvader* this_, Actor* attacker) {
    if (attacker == nullptr) return false;
    Actor* a = &this_->actor;
    const s16 yawToAttacker = YawTowardTarget(a->world.pos, attacker->world.pos);
    const s16 delta = (s16)(yawToAttacker - a->shape.rot.y);
    const int absDelta = (delta < 0) ? -(int)delta : (int)delta;
    return absDelta < kBlockFrontalAngle;
}

bool InvaderHasRangedWeapon() {
    // v1: gate ranged engagement on the local player's inventory so
    // an Invader spawned in early-game (no bow/slingshot owned) does
    // not fire arrows. Agent 4 may revisit this — Invader's gear is
    // not necessarily tied to player inventory in the final design.
    const u8 bowSlot   = INV_CONTENT(ITEM_BOW);
    const u8 slingSlot = INV_CONTENT(ITEM_SLINGSHOT);
    return (bowSlot != ITEM_NONE) || (slingSlot != ITEM_NONE);
}

// Forward decl — ChooseCombatExitState is defined below the combat
// Tick handlers but called from each of them. C++ single-pass lookup
// for free functions requires the declaration to appear first.
s32 ChooseCombatExitState(EnInvader* this_, PlayState* play);

// ---------------------------------------------------------------------
// Tick handlers.
// ---------------------------------------------------------------------

void TickATTACK(EnInvader* this_, PlayState* play, const Vec3f& targetSeedPos) {
    (void)targetSeedPos;
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    if (this_->prevState != EN_INVADER_STATE_ATTACK) {
        sAttackState.entryFrame = Anchor::Instance->gameFrameCounter.load(
                                       std::memory_order_relaxed);
    }

    if (sAttackState.target != nullptr &&
        (sAttackState.target->update == nullptr)) {
        sAttackState.target = nullptr;
    }

    if (sAttackState.target != nullptr) {
        a->shape.rot.y = YawTowardTarget(a->world.pos, sAttackState.target->world.pos);
        a->world.rot.y = a->shape.rot.y;
    }

    const float curFrame = this_->skelAnime.curFrame;
    const bool inActiveWindow = (curFrame >= kAttackActiveStartFrame &&
                                  curFrame <= kAttackActiveEndFrame);
    if (inActiveWindow && !sAttackState.swingFiredAT) {
        PositionAttackQuad(this_);
        CollisionCheck_SetAT(play, &play->colChkCtx, &this_->atCollider.base);
    } else if (!inActiveWindow && curFrame > kAttackActiveEndFrame) {
        sAttackState.swingFiredAT = true;
    }

    // Exit only when anim has actually started AND finished, AND a
    // minimum hold has elapsed since entry. If Agent 2 hasn't wired
    // anim selection yet, endFrame may stay at the previous state's
    // value (typically non-zero); without these guards the state
    // would early-exit on the first tick because (curFrame >=
    // endFrame) is true from leftover idle-anim state.
    const uint64_t curTick = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);
    const bool holdElapsed = curTick >= sAttackState.entryFrame + kMinSwingHoldTicks;
    if (holdElapsed && this_->skelAnime.endFrame > 0.0f &&
        this_->skelAnime.curFrame >= this_->skelAnime.endFrame) {
        const s32 next = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[Invader] ATTACK→{} (swing complete)",
                    (next == EN_INVADER_STATE_STANDBY ? "STANDBY" : "IDLE/FOLLOW"));
        this_->state = next;
        sAttackState.swingFiredAT = false;
    }
}

void TickENGAGE(EnInvader* this_, PlayState* play, const Vec3f& leaderHintPos) {
    (void)leaderHintPos;
    Actor* a = &this_->actor;

    if (sAttackState.target == nullptr ||
        sAttackState.target->update == nullptr) {
        const s32 next = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[Invader] ENGAGE→{} (target lost)",
                    (next == EN_INVADER_STATE_STANDBY ? "STANDBY" : "IDLE/FOLLOW"));
        this_->state = next;
        sAttackState.target = nullptr;
        a->speedXZ = 0.0f;
        return;
    }

    const Vec3f& targetPos = sAttackState.target->world.pos;
    const float dx = targetPos.x - a->world.pos.x;
    const float dz = targetPos.z - a->world.pos.z;
    const float distXZ = std::sqrt(dx * dx + dz * dz);

    if (distXZ > kEngageBreakDist) {
        const s32 next = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[Invader] ENGAGE→{} (target fled {:.0f}u > {:.0f}u)",
                    (next == EN_INVADER_STATE_STANDBY ? "STANDBY" : "IDLE/FOLLOW"),
                    distXZ, kEngageBreakDist);
        this_->state = next;
        sAttackState.target = nullptr;
        a->speedXZ = 0.0f;
        return;
    }

    if (distXZ <= kEngageStrikeDist) {
        SPDLOG_INFO("[Invader] ENGAGE→ATTACK (strike range, dist={:.0f}u)", distXZ);
        this_->state = EN_INVADER_STATE_ATTACK;
        sAttackState.swingFiredAT = false;
        a->speedXZ = 0.0f;
        return;
    }

    // Pursuit — walk close, run far.
    a->shape.rot.y = YawTowardTarget(a->world.pos, targetPos);
    a->world.rot.y = a->shape.rot.y;
    a->speedXZ = (distXZ > kEngageRunDistance) ? kEngageRunSpeed : kEngageWalkSpeed;
}

void TickBLOCK(EnInvader* this_, PlayState* play, const Vec3f& leaderHintPos) {
    (void)leaderHintPos;
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);

    if (this_->prevState != EN_INVADER_STATE_BLOCK) {
        sBlockState.entryFrame    = curFrame;
        sBlockState.hitAnimFrames = 0;
        SPDLOG_INFO("[Invader] BLOCK entry — HP={}", (int)this_->health);
    }

    if (sAttackState.target != nullptr &&
        sAttackState.target->update == nullptr) {
        sAttackState.target = nullptr;
    }

    if (sAttackState.target != nullptr) {
        a->shape.rot.y = YawTowardTarget(a->world.pos, sAttackState.target->world.pos);
        a->world.rot.y = a->shape.rot.y;
    }

    // Frontal-deflect — TickInvaderActor runs BEFORE the C-code AC
    // drain in EnInvader_Update, so we can consume the AC_HIT flag
    // here and zero the damage before health is decremented. Only
    // deflect hits where the attacker is in the Invader's frontal
    // cone (block target known + frontally placed). Side/back hits
    // pass through to the normal drain.
    if ((this_->collider.base.acFlags & AC_HIT) &&
        sAttackState.target != nullptr &&
        IsFrontalAttacker(this_, sAttackState.target)) {
        this_->collider.base.acFlags  &= ~AC_HIT;
        this_->actor.colChkInfo.damage = 0;
        sBlockState.hitAnimFrames = 12;  // play kBlockHit reaction (Agent 2 anim)
        SPDLOG_INFO("[Invader] BLOCK deflect — frontal hit absorbed");
    }

    if (sBlockState.hitAnimFrames > 0) {
        sBlockState.hitAnimFrames--;
    }

    const uint64_t durationTicks =
        (uint64_t)Anchor::Instance->MsToGameTicks(kBlockDurationMs);
    if (curFrame >= sBlockState.entryFrame + durationTicks) {
        if (sAttackState.target != nullptr) {
            const float dx = sAttackState.target->world.pos.x - a->world.pos.x;
            const float dz = sAttackState.target->world.pos.z - a->world.pos.z;
            const float distXZ = std::sqrt(dx * dx + dz * dz);
            if (distXZ <= kAttackEngageDist) {
                SPDLOG_INFO("[Invader] BLOCK→ATTACK (timer expired, target in range "
                            "dist={:.0f})", distXZ);
                this_->state = EN_INVADER_STATE_ATTACK;
                sAttackState.swingFiredAT = false;
                return;
            }
        }
        const s32 next = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[Invader] BLOCK→{} (timer expired)",
                    (next == EN_INVADER_STATE_STANDBY ? "STANDBY" : "IDLE/FOLLOW"));
        this_->state = next;
        if (next != EN_INVADER_STATE_STANDBY) {
            sAttackState.target = nullptr;
        }
    }
}

void TickRANGED_ATTACK(EnInvader* this_, PlayState* play, const Vec3f& leaderHintPos) {
    (void)leaderHintPos;
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    if (this_->prevState != EN_INVADER_STATE_RANGED_ATTACK) {
        sAttackState.entryFrame = Anchor::Instance->gameFrameCounter.load(
                                       std::memory_order_relaxed);
    }

    if (sAttackState.target == nullptr ||
        sAttackState.target->update == nullptr) {
        const s32 next = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[Invader] RANGED_ATTACK→{} (target lost mid-shoot)",
                    (next == EN_INVADER_STATE_STANDBY ? "STANDBY" : "IDLE/FOLLOW"));
        this_->state = next;
        sAttackState.target = nullptr;
        sAttackState.swingFiredAT = false;
        return;
    }

    const Vec3f& tp = sAttackState.target->world.pos;
    const float dx = tp.x - a->world.pos.x;
    const float dz = tp.z - a->world.pos.z;
    const float distXZ = std::sqrt(dx * dx + dz * dz);
    if (distXZ > kRangedBreakDist) {
        const s32 next = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[Invader] RANGED_ATTACK→{} (target fled {:.0f}u > {:.0f}u)",
                    (next == EN_INVADER_STATE_STANDBY ? "STANDBY" : "IDLE/FOLLOW"),
                    distXZ, kRangedBreakDist);
        this_->state = next;
        sAttackState.target = nullptr;
        sAttackState.swingFiredAT = false;
        return;
    }

    a->shape.rot.y = YawTowardTarget(a->world.pos, tp);
    a->world.rot.y = a->shape.rot.y;

    if (!sAttackState.swingFiredAT &&
        this_->skelAnime.curFrame >= kRangedSpawnFrame) {
        const float dy = tp.y - a->world.pos.y;
        const s16 pitch = (s16)(-Math_Atan2S(distXZ, -dy));

        Vec3f spawnPos = {
            a->world.pos.x,
            a->world.pos.y + kRangedSpawnHeightY,
            a->world.pos.z,
        };
        Actor* arrow = Actor_Spawn(
            &play->actorCtx, play, ACTOR_EN_ARROW,
            spawnPos.x, spawnPos.y, spawnPos.z,
            pitch, a->shape.rot.y, 0,
            ARROW_NORMAL);
        sAttackState.swingFiredAT = true;
        if (arrow != nullptr) {
            SPDLOG_INFO("[Invader] RANGED_ATTACK fire — dist={:.0f} pitch=0x{:X} "
                        "yaw=0x{:X}",
                        distXZ, (uint16_t)pitch, (uint16_t)a->shape.rot.y);
        } else {
            SPDLOG_WARN("[Invader] RANGED_ATTACK Actor_Spawn(EN_ARROW) returned null");
        }
    }

    // Same anim-not-wired + min-hold guards as ATTACK.
    const uint64_t curTick = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);
    const bool holdElapsed = curTick >= sAttackState.entryFrame + kMinSwingHoldTicks;
    if (holdElapsed && this_->skelAnime.endFrame > 0.0f &&
        this_->skelAnime.curFrame >= this_->skelAnime.endFrame) {
        const s32 next = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[Invader] RANGED_ATTACK→{} (shoot complete)",
                    (next == EN_INVADER_STATE_STANDBY ? "STANDBY" : "IDLE/FOLLOW"));
        this_->state = next;
        if (next != EN_INVADER_STATE_STANDBY) {
            sAttackState.target = nullptr;
        }
        sAttackState.swingFiredAT = false;
    }
}

void TickSTANDBY(EnInvader* this_, PlayState* play, const Vec3f& targetHintPos) {
    (void)targetHintPos;
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    // Face nearest hostile target.
    Actor* faceTarget = sAttackState.target;
    if (faceTarget == nullptr || faceTarget->update == nullptr) {
        faceTarget = PickHostileTarget(a, play, kStandbyDetectDist,
                                       /*maxYDelta=*/kRangedYFilter);
        if (faceTarget != nullptr) {
            sAttackState.target = faceTarget;
        }
    }
    if (faceTarget != nullptr) {
        a->shape.rot.y = YawTowardTarget(a->world.pos, faceTarget->world.pos);
        a->world.rot.y = a->shape.rot.y;
    }

    // No target in detect range → drop to IDLE (Agent 2's locomotion
    // will pick up from there).
    if (faceTarget == nullptr) {
        sAttackState.target = nullptr;
        SPDLOG_INFO("[Invader] STANDBY→IDLE (no targets in detect range)");
        this_->state = EN_INVADER_STATE_IDLE;
        return;
    }

    // Target is far enough that the locomotion layer should resume
    // pursuit toward it. Hand off to FOLLOW (Agent 2 wires the
    // pursuit toward sAttackState.target via the target hint; for
    // now FOLLOW may default to IDLE if Agent 2's locomotion isn't
    // landed yet — that's acceptable, TryEngageCombat will re-fire
    // once a tier matches).
    const float distSq = Dist2DSq(a->world.pos, faceTarget->world.pos);
    if (distSq > kStandbyIdleRadius * kStandbyIdleRadius) {
        SPDLOG_INFO("[Invader] STANDBY→FOLLOW (target {:.0f}u away — handoff to "
                    "locomotion)",
                    std::sqrt(distSq));
        this_->state = EN_INVADER_STATE_FOLLOW;
    }
    // Otherwise stay in STANDBY; TryEngageCombat will re-engage once
    // a tier matches.
}

// Tier-ordered engagement check. Mirrors FollowerNPC's TryEngageCombat
// but targets are Players (hostile) instead of enemies (friendly).
bool TryEngageCombat(EnInvader* this_, PlayState* play) {
    // Eligible from non-combat states only. Combat states stay in
    // their own handlers; STANDBY is eligible (combat-to-combat
    // re-engage) so a STANDBY→ATTACK chain can happen the moment
    // the target is back in melee range.
    if (this_->state != EN_INVADER_STATE_IDLE &&
        this_->state != EN_INVADER_STATE_FOLLOW &&
        this_->state != EN_INVADER_STATE_STANDBY) {
        return false;
    }

    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);
    if (curFrame < sCombatCooldownEndFrame) return false;

    const char* fromName = StateName(this_->state);
    Actor* selfActor = &this_->actor;

    // Tier 1 — melee range. BLOCK if low HP, otherwise ATTACK.
    Actor* meleeTarget = PickHostileTarget(selfActor, play, kAttackEngageDist);
    if (meleeTarget != nullptr) {
        const s8 maxHp = (this_->maxHealth > 0) ? this_->maxHealth : 1;
        const bool lowHp =
            ((float)this_->health / (float)maxHp <= kBlockHpThresholdRatio);
        if (lowHp) {
            SPDLOG_INFO("[Invader] {}→BLOCK (low HP {}/{}, target Player at "
                        "({:.0f},{:.0f},{:.0f}))",
                        fromName, (int)this_->health, (int)maxHp,
                        meleeTarget->world.pos.x, meleeTarget->world.pos.y,
                        meleeTarget->world.pos.z);
            this_->state = EN_INVADER_STATE_BLOCK;
            sAttackState.target = meleeTarget;
            sAttackState.swingFiredAT = false;
            sLastCombatWeapon = 0;
            return true;
        }
        SPDLOG_INFO("[Invader] {}→ATTACK (target Player at "
                    "({:.0f},{:.0f},{:.0f}))",
                    fromName,
                    meleeTarget->world.pos.x, meleeTarget->world.pos.y,
                    meleeTarget->world.pos.z);
        this_->state = EN_INVADER_STATE_ATTACK;
        sAttackState.target = meleeTarget;
        sAttackState.swingFiredAT = false;
        sLastCombatWeapon = 0;
        return true;
    }

    // Tier 2 — ENGAGE pursuit (ground-level targets only via tight Y
    // filter so elevated targets fall through to ranged).
    Actor* pursueTarget = PickHostileTarget(selfActor, play, kEngageAcquireDist);
    if (pursueTarget != nullptr) {
        SPDLOG_INFO("[Invader] {}→ENGAGE (target Player at "
                    "({:.0f},{:.0f},{:.0f}))",
                    fromName,
                    pursueTarget->world.pos.x, pursueTarget->world.pos.y,
                    pursueTarget->world.pos.z);
        this_->state = EN_INVADER_STATE_ENGAGE;
        sAttackState.target = pursueTarget;
        sAttackState.swingFiredAT = false;
        sLastCombatWeapon = 0;
        return true;
    }

    // Tier 3 — RANGED_ATTACK. Wide Y filter for elevated targets.
    if (InvaderHasRangedWeapon()) {
        Actor* rangedTarget = PickHostileTarget(selfActor, play,
                                                 kRangedAcquireDist,
                                                 kRangedYFilter);
        if (rangedTarget != nullptr) {
            const float dx = rangedTarget->world.pos.x - selfActor->world.pos.x;
            const float dz = rangedTarget->world.pos.z - selfActor->world.pos.z;
            const float dy = rangedTarget->world.pos.y - selfActor->world.pos.y;
            const float distXZ = std::sqrt(dx * dx + dz * dz);
            const bool isElevated = std::fabs(dy) > kRangedElevatedYDelta;
            if (isElevated && distXZ >= kRangedMinDist) {
                SPDLOG_INFO("[Invader] {}→RANGED_ATTACK (elevated target at "
                            "({:.0f},{:.0f},{:.0f}) dist={:.0f} dy={:+.0f})",
                            fromName,
                            rangedTarget->world.pos.x, rangedTarget->world.pos.y,
                            rangedTarget->world.pos.z, distXZ, dy);
                this_->state = EN_INVADER_STATE_RANGED_ATTACK;
                sAttackState.target = rangedTarget;
                sAttackState.swingFiredAT = false;
                sLastCombatWeapon = 1;
                return true;
            }
        }
    }

    return false;
}

// Defined after the Tick handlers per its forward-decl above.
s32 ChooseCombatExitState(EnInvader* this_, PlayState* play) {
    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);
    sCombatCooldownEndFrame = curFrame +
        (uint64_t)Anchor::Instance->MsToGameTicks(kPostCombatCooldownMs);
    Actor* nearby = PickHostileTarget(&this_->actor, play, kStandbyDetectDist,
                                       /*maxYDelta=*/kRangedYFilter);
    return (nearby != nullptr) ? EN_INVADER_STATE_STANDBY
                               : EN_INVADER_STATE_IDLE;
}

}  // namespace

extern "C" void Anchor_TickInvaderActor(Actor* invader, PlayState* play) {
    if (invader == nullptr || play == nullptr) return;
    EnInvader* this_ = (EnInvader*)invader;

    // G18 — freeze during cutscenes (same shape as FollowerNPC's G18
    // gate). Without this, the Invader could swing or shoot during
    // a cutscene; combat AT/AC during cutscenes is a common source
    // of "what just hit me?" bugs.
    if (gPlayState->csCtx.state != CS_STATE_IDLE) {
        invader->speedXZ = 0.0f;
        return;
    }

    // Hostile-target acquisition + tier-based engagement check. Fires
    // BEFORE state dispatch so the new state's handler gets the
    // first tick (entry-frame capture, initial yaw, etc.). For the
    // intentional combat-state pre-empt, the dispatcher below picks
    // the new state's handler — locomotion is not preempted unless
    // a tier matches.
    TryEngageCombat(this_, play);

    // Hint pos for handlers that want a "where to face when nothing
    // else applies" — use the target's pos when known, else the
    // actor's own pos (no-op fallback). Agent 4 may replace with
    // a multi-target hint.
    const Vec3f hintPos = sAttackState.target != nullptr
                             ? sAttackState.target->world.pos
                             : invader->world.pos;

    switch (this_->state) {
        case EN_INVADER_STATE_ATTACK:
            TickATTACK(this_, play, hintPos);
            break;
        case EN_INVADER_STATE_ENGAGE:
            TickENGAGE(this_, play, hintPos);
            break;
        case EN_INVADER_STATE_BLOCK:
            TickBLOCK(this_, play, hintPos);
            break;
        case EN_INVADER_STATE_RANGED_ATTACK:
            TickRANGED_ATTACK(this_, play, hintPos);
            break;
        case EN_INVADER_STATE_STANDBY:
            TickSTANDBY(this_, play, hintPos);
            break;
        case EN_INVADER_STATE_IDLE:
        case EN_INVADER_STATE_FOLLOW:
        case EN_INVADER_STATE_STUCK:
        case EN_INVADER_STATE_DEAD:
        default:
            // Locomotion / non-combat states are owned by Agent 2.
            // No-op until that layer lands; the actor still ticks
            // animation + collision via EnInvader_Update.
            break;
    }

    // Update prevState tail (after dispatch so combat handlers can
    // edge-detect via prevState != state on the entry tick).
    this_->prevState = this_->state;
}

extern "C" void Anchor_InvaderDrawBegin(Actor* invader) {
    sCurrentlyDrawingInvader = invader;
}

extern "C" void Anchor_InvaderDrawEnd(void) {
    sCurrentlyDrawingInvader = nullptr;
}

extern "C" Actor* Anchor_GetCurrentlyDrawingInvader(void) {
    return sCurrentlyDrawingInvader;
}
