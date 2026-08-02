/**
 * ChargeStateMachine — reusable per-instance charge/cooldown state
 * for enemy-enhancement attack decisions.
 *
 * Extracted 2026-07-31 from Karebaba V6 (second consumer is Dekubaba
 * with two coexisting attacks — acid vomit at +25% steps and seed
 * spawn at +10% steps).
 *
 * Pattern: at each attack-decision point, host rolls chance = counter
 * × stepIncrement. On success → Ready → fire on next opportunity
 * (possibly with a range/preconditions gate applied by caller). On
 * fire → Cooldown for N attacks. On fail → counter++ (clamped so
 * chance never exceeds 100%).
 *
 * Semantics:
 *   - `Charging`: rolling per attack, chance = counter × stepIncrement
 *   - `Ready`:    waiting for fire preconditions (range, cooldown of
 *                 competing attack, etc.). Set by TryCharge on success.
 *                 Caller checks IsReady() at fire-decision point.
 *   - `Cooldown`: guaranteed no-fire lockout for `cooldownSteps` attacks.
 *
 * Coexistence with other charge machines on the same actor is via
 * priority-ordered rolls at the caller: the caller sequences
 * `TryCharge()` on multiple machines in priority order and reads
 * `IsReady()` in the same order. The Dekubaba descriptor pattern is:
 *   1. `seedMachine.TryCharge();`
 *   2. `acidMachine.TryCharge();`
 *   3. If `seedMachine.TryFire(preconditions)` → seed
 *   4. Else if `acidMachine.TryFire(preconditions)` → acid
 *   5. Else vanilla
 *
 * The always-advance semantic (both TryCharge calls fire before any
 * TryFire) prevents starvation — one attack winning doesn't stop
 * the other's counter from ramping.
 */

#pragma once

#include <cstdint>

extern "C" {
#include "z64.h"  // f32, u8
#include "functions.h"  // Rand_ZeroOne
}

namespace AnchorEnemyEnhancement {

class ChargeStateMachine {
public:
    enum class State : uint8_t {
        Charging = 0,
        Ready    = 1,
        Cooldown = 2,
    };

    // Configuration — set at descriptor construction; immutable per
    // instance. Field defaults match Karebaba's V6 numbers.
    struct Config {
        f32 stepIncrement    = 0.25f;  // per-attack chance step
        u8  maxCounter       = 4;      // chance clamps at counter × step
        u8  cooldownSteps    = 3;      // post-fire lockout in "attacks"
        // 2026-08-02 — starting counter value at construction, after
        // Reset(), and after Cooldown → Charging transition. Default
        // 0 keeps Karebaba's "first spin has 0% chance" behavior.
        // Dekubaba uses 1 (25% chance on first attack) per user 2026-
        // 08-02 request "when deku first activate, they already have
        // a 25% chance to use their new attacks" — short-lived enemies
        // need a shot at their new abilities before Link kills them.
        u8  initialCounter   = 0;
    };

    ChargeStateMachine() = default;
    explicit ChargeStateMachine(const Config& cfg)
        : mConfig(cfg), mCounter(cfg.initialCounter) {}

    void SetConfig(const Config& cfg) {
        mConfig = cfg;
        mCounter = cfg.initialCounter;
    }

    // --- State machine transitions ------------------------------------

    // Called at each attack-decision point. Advances the state machine
    // and returns:
    //   - true iff this call transitioned Charging → Ready (i.e. this
    //     specific TryCharge rolled a success). Caller can use the
    //     return to fire a "just-charged" SFX / VFX.
    // Semantics per state:
    //   - Charging: rolls chance = counter × step. On success → Ready.
    //     On fail → counter++ (clamped).
    //   - Ready: no-op (stays Ready until Fire()).
    //   - Cooldown: no-op (Cooldown decrement happens in OnAttackComplete).
    bool TryCharge() {
        if (mState != State::Charging) return false;
        const f32 chance = (f32)mCounter * mConfig.stepIncrement;
        if (Rand_ZeroOne() < chance) {
            mState = State::Ready;
            return true;
        }
        if (mCounter < mConfig.maxCounter) mCounter++;
        return false;
    }

    // Returns true iff the machine is in Ready state (i.e. fire is
    // pending, subject to caller-side preconditions like range).
    // Caller reads this BEFORE deciding to fire.
    bool IsReady() const { return mState == State::Ready; }

    // Called by caller when the fire actually happens (all
    // preconditions met, attack committed). Transitions Ready →
    // Cooldown with cooldownSteps ticks. Idempotent from non-Ready.
    void OnFire() {
        if (mState != State::Ready) return;
        mState        = State::Cooldown;
        mCounter      = 0;
        mCooldownLeft = mConfig.cooldownSteps;
    }

    // Called at attack-complete regardless of which attack fired.
    // Decrements Cooldown; transitions back to Charging when it hits 0.
    // No-op from Charging / Ready.
    void OnAttackComplete() {
        if (mState != State::Cooldown) return;
        if (mCooldownLeft > 0) mCooldownLeft--;
        if (mCooldownLeft == 0) {
            mState   = State::Charging;
            mCounter = mConfig.initialCounter;
        }
    }

    // Full reset — used on actor death / respawn / scene teardown.
    void Reset() {
        mState        = State::Charging;
        mCounter      = mConfig.initialCounter;
        mCooldownLeft = 0;
    }

    // --- Introspection (for wire-sync + debug) ------------------------

    State GetState()          const { return mState; }
    u8    GetCounter()        const { return mCounter; }
    u8    GetCooldownLeft()   const { return mCooldownLeft; }

    // Force a state — used by peer receive-side to mirror host's state
    // machine. Peer never runs TryCharge; it just receives Ready/not-Ready
    // and IsReady() reports the host-authoritative state.
    void ForceState(State s) { mState = s; }

private:
    Config mConfig{};
    State  mState        = State::Charging;
    u8     mCounter      = 0;  // set to mConfig.initialCounter in ctor / Reset / OnAttackComplete
    u8     mCooldownLeft = 0;
};

}  // namespace AnchorEnemyEnhancement
