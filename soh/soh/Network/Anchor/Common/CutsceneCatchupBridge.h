#ifndef SOH_ANCHOR_CUTSCENE_CATCHUP_BRIDGE_H
#define SOH_ANCHOR_CUTSCENE_CATCHUP_BRIDGE_H

// C-callable predicates for vanilla decomp + actor overlays to
// consult the cutscene late-join catchup state.
//
// Called from:
//   - soh/src/code/z_demo.c (2 sites: func_80064520 SKIPPABLE_INIT
//     entry; func_80064534 UNSKIPPABLE_INIT entry).
//   - soh/src/overlays/actors/ovl_Bg_Treemouth/z_bg_treemouth.c
//     (1 site: Deku Tree intro direct-write at line 202).
//   - soh/src/overlays/actors/ovl_En_Zl4/z_en_zl4.c
//     (2 sites: Zelda cutscene direct-writes at lines 909, 921).
//
// Design: Claude/Analysis/cutscene_entry_gate_design_2026-07-07.md
// Plan:   Claude/Plans/cutscene_late_join_plan.md §3.3
//
// Behavior: returns 1 when the local client should suppress a NEW
// cutscene entry — meaning we've detected a same-team peer mid-
// cutscene in our scene and are awaiting the CUTSCENE_CATCHUP_RESPONSE
// that will jump us into the leader's mid-cutscene state. Returns 0
// otherwise.
//
// SoC / Law of Demeter: caller "asks a question" — doesn't reach into
// Anchor state or know anything about the ledger / packet layer.

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 if any cutscene entry should be suppressed right now.
// Safe to call from any C context; no locking; reads a single
// map-emptiness check on Anchor::Instance. Nanoseconds per call.
int Anchor_ShouldSuppressLocalCutsceneEntry(void);

#ifdef __cplusplus
}
#endif

#endif  // SOH_ANCHOR_CUTSCENE_CATCHUP_BRIDGE_H
