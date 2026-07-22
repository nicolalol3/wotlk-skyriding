# wxl-skyriding

Client companion for Horizon skyriding (AzerothCore `mod-skyriding`).

## What it does

- Strips `ASCENDING` / `DESCENDING` while skyriding mode is on (`ClassicVertical=0`) so Space/X do nothing.
- Applies yaw turn inertia (`TurnRate`, default 0.75 = −25%).
- Drives AdvFly mount clips (glide / dive / surge / skyward / stall) via `CGUnit::SetBoneSequence`.
- Exposes Lua API used by `HorizontalTools/SkyridingBar.lua`.

## Requirements

- `wxl-anim-limit` enabled (anim IDs ≥ 506).
- Mount M2 contains AdvFly sequences (1530, 1532, 1678, 1680, 1702, 1704, 1722, 1726, …).
- Server `mod-skyriding` sending `HORIZON_SKY` addon messages.

## Opt-out

Create next to `Wow.exe`:

- `WarcraftXL_skyriding.disable`
- or `WarcraftXL_wxl-skyriding.disable`
