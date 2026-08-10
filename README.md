# Flotilla

**Flotilla** is a multiplayer / co-op mod for [Ship of Harkinian](https://www.shipofharkinian.com/)
(SoH), the PC port of *The Legend of Zelda: Ocarina of Time*.

It layers a host-authoritative enemy / world sync system, an AI companion
subsystem, coordinated cutscenes and dialogue, and cross-client cosmetic
sync (custom Link models, custom voice packs) on top of SoH's existing
Anchor networking. The design target is **2–4 friends playing the main
quest together**, not MMO-scale open worlds.

Flotilla is a fork of `HarbourMasters/Shipwright` and stays in step with
the upstream `develop-ackbar` stabilization branch.

![Ship of Harkinian](docs/shiptitle.darkmode.png#gh-dark-mode-only)
![Ship of Harkinian](docs/shiptitle.lightmode.png#gh-light-mode-only)

## Status — Alpha 1 in progress

Flotilla has not been released yet. The **Alpha 1** milestone target is
the full OoT main quest playable end-to-end with 3 concurrent players
and vanilla enemy behaviour. When Alpha 1 lands it will be distributed
through the `soh-modding` community.

The current working baseline is tagged `alpha1-playtest-baseline-2026-08-07`.
Trunk (`development-multiplayer`) continues to receive Alpha 1 blocker
work; expect rough edges if you build from head. Session-by-session
progress lives in
[`Claude/session_state.md`](Claude/session_state.md) and the running
backlog in [`Claude/task_checklist.md`](Claude/task_checklist.md).

If you want to help playtest before Alpha 1 ships, the fastest path is
to file findings against the [issue tracker](https://github.com/St3ph3nBr00ks/Shipwright/issues).

## What Flotilla adds

**Networking / sync**
- Host-authoritative per-room enemy sync (~40+ enemies + Boss Goma)
- Player pose sync via SkelAnime joint tables
- Save-flag sync (event chk info, scene switch/clear, inf table)
- Time-of-day sync with vanilla time-frozen-scene handling
- Item drop sync (grass/bush drops, Deku stick drops, etc.)
- Damage attribution + peer knockback replication (Path A / Vanilla Mirror Pattern)
- Late-join replay of dead enemies + live world state
- Horse (Epona) sync, including mounted play
- Host-enforced game-settings sync (~125 gameplay-affecting CVars)

**Multiplayer UX**
- Team ID / PvP (Off / On / On + Friendly Fire) inherited from stock Anchor
- Voting-skip dialogue and cutscene advance
- Coordination-point cutscene sync (opt-in beta)
- Colored per-peer nametags with brightness-adaptive background
- Team Marker: through-walls fairy over same-team peers

**Cosmetic sync**
- Custom Link model packs (`.otr`) sync per player
- Custom player voice packs (VRP format) sync per player

**AI companions** *(all default-off, opt-in via CVar)*
- AI Player Follower — AI drives your own Link when you set it down
- NPC Follower — friendly Link-lookalike companion actor
- NPC Invader — hostile Link-lookalike pursuer
- AI Director substrate for future spawn systems

**Design principles**
- Vanilla-altering features ship behind an opt-in CVar and default off, permanently
- Boss actors are excluded from AI/nav extensions unless explicitly opted in
- Self-hostable — the underlying Anchor relay is ~200 lines of Go, no
  proprietary server required

## Trying it now (build from source)

There are no prebuilt Flotilla binaries yet. To try the current tree
you need to compile it yourself.

### Requirements

- Windows 10/11 (primary supported platform — Linux/macOS have not been
  tested with Flotilla additions)
- Visual Studio 2022 with MSVC v14.44 and the Windows 10/11 SDK
- CMake in PATH
- A **legally acquired** dump of *The Legend of Zelda: Ocarina of Time*
  (NTSC 1.2 is the confirmed working version). Check compatibility at
  https://ship.equipment/
- Go (for optionally running your own Anchor relay locally)

### Build

```
cmake -S . -B build/x64 -G "Visual Studio 17 2022" -T v143 -A x64 \
      -DBUILD_REMOTE_CONTROL=ON -DDISABLE_SCRIPTING=ON
cmake --build build/x64 --config Release
```

`-DDISABLE_SCRIPTING=ON` is required on Windows/MSVC until TinyCC's
`config.h` generation is fixed upstream. Also run
`pip install -r libultraship/requirements.txt` so the keystore generator
can run.

Detailed base build instructions live in
[`docs/BUILDING.md`](docs/BUILDING.md); the Flotilla-specific gotchas
(reconfigure requirements when new `.cpp` files are added under
`soh/soh/Network/Anchor/**`, etc.) are documented in
[`CLAUDE.md`](CLAUDE.md).

### Connect

By default Flotilla points at the public HarbourMasters Anchor relay.
To self-host, run `go run .` in `Anchor/anchor_git/` and set the Anchor
host CVar to your machine's IP. No relay-side code changes are needed
to run Flotilla — the relay is a dumb router.

## Requirements for players (once Alpha 1 ships)

- Windows PC per Ship of Harkinian's normal requirements
- Legally acquired ROM
- A friend or two willing to play with you
- Everyone on the same Flotilla version (settings sync will refuse
  mismatched clients)

## Configuration

### Default keyboard configuration
| N64 | A | B | Z | Start | Analog stick | C buttons | D-Pad |
| - | - | - | - | - | - | - | - |
| Keyboard | X | C | Z | Space | WASD | Arrow keys | TFGH |

### Other shortcuts
| Keys | Action |
| - | - |
| ESC | Toggle menu |
| F2 | Toggle capture mouse input |
| F5 | Save state |
| F6 | Change state |
| F7 | Load state |
| F9 | Toggle Text-to-Speech (Windows and Mac only) |
| F11 | Fullscreen |
| Tab | Toggle Alternate assets |
| Ctrl+R | Reset |

## Contributing

The trunk branch is `development-multiplayer`. Session state, open
tracker items, and coding conventions live in
[`CLAUDE.md`](CLAUDE.md), [`Claude/session_state.md`](Claude/session_state.md),
and [`Claude/task_checklist.md`](Claude/task_checklist.md). File issues
or pull requests against the [GitHub tracker](https://github.com/St3ph3nBr00ks/Shipwright).

## Credits and thanks

Flotilla stands entirely on the shoulders of:

- **[Ship of Harkinian](https://github.com/HarbourMasters/Shipwright)** —
  the PC port that makes any of this possible
- **[libultraship](https://github.com/Kenix3/libultraship)** — the modern
  runtime backing SoH
- The **Ocarina of Time decompilation** project
- HarbourMasters and the SoH community for the Anchor networking primitives
  Flotilla builds on
- Everyone in `soh-modding` who has playtested and given feedback

Ocarina of Time is © Nintendo. Flotilla does not include or distribute
any copyrighted Nintendo assets; you must supply your own legally
acquired ROM.

<a href="https://github.com/Kenix3/libultraship/">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./docs/poweredbylus.darkmode.png">
    <img alt="Powered by libultraship" src="./docs/poweredbylus.lightmode.png">
  </picture>
</a>
