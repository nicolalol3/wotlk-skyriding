# wxl-anim-limit

Breaks the stock 3.3.5a client animation ID ceiling (505 / `FlyCarried2H`) so models
can play AnimationData IDs **506+**. Port of Duskhaven
`ExtendedAnimationIdFixes` from
[`CharacterFixes.cpp`](https://github.com/Duskhaven-Reforged/dusk-tswow/blob/master/misc/client-extensions/src/Character/CharacterFixes.cpp).

## What it does

Hooks three client functions:

| Address | Role |
|---------|------|
| `0x826050` | `CM2Model::HasPlayableSequenceFallback` — for ID ≥ 506, ask the M2 directly |
| `0x825F40` | `CM2Model::FindPlayableSequenceFallbackId` — same, returns ID or -1 |
| `0x7176F0` | `CGUnit::ResolveModelAnimationId` — resolve via M2 sequence or `AnimationData.dbc` fallback chain |

## Requirements

1. Extended rows in **client** `AnimationData.dbc` (IDs ≥ 506).
2. Character / mount M2 (and optional `.anim`) that actually contain those sequences.
3. Spell / movement data that *requests* those IDs (DBC visuals, mount state, etc.).

This module only removes the **client resolution gate**. It does not add models or DBC rows.

## Opt-out

Create either file next to `Wow.exe`:

- `WarcraftXL_anim-limit.disable`
- `WarcraftXL_wxl-anim-limit.disable`

## Notes

- Does **not** include Duskhaven monk sheath / channel-layering hooks.
- `wxl-modern-assets` still remaps a few modern sequence IDs (>505) onto vanilla ones
  when downporting; that path is separate from this limit break.
