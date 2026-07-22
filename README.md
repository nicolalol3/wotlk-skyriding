# WotLK Skyriding

Experimental **skyriding** (dragonriding-style flight) for World of Warcraft **3.3.5a / WotLK**, built on AzerothCore + WarcraftXL (WXL) on the client.

This project is **vibe-coded**. I am not publishing it to show off — I am publishing it so it can be **open source**, and so someone who actually knows how to code can **finish it**.

---

## What is in this repo

| Path | Contents |
|------|----------|
| `client/wxl-scripts/wxl-skyriding/` | WXL module: client physics, AdvFly animations, Lua bridge |
| `client/wxl-scripts/wxl-anim-limit/` | WXL module: raises the anim ID ceiling past 505 (needed for AdvFly / drag-and-drop mount assets in a patch) |
| `server/mod-skyriding/` | AzerothCore skyriding module (server physics, vigor, Surge/Skyward, addon messages) |
| `server/mod-spells-qol/src/ridingchanges.cpp` | Single extracted file from my pre-existing riding QoL module (see below) |
| `client/dbc/Spell.dbc` | My `Spell.dbc` from **patch-b** (also contains unrelated custom content) |
| `client/addon/SkyridingBar/` | Only the UI pieces that show vigor / speed / pitch and talk to WXL — **not** the full addon |

**Not included:** mount assets (M2/skin/texture/.anim). Export those yourself with wow.export (I use **Tawny Wind Rider**).

---

## Architecture (short)

1. **WXL (WarcraftXL)** — required. The system extends client animations and behavior; **it will not run without WXL**.
2. **WXL script modules** to drop into / enable under your WXL scripts folder:
   - `wxl-anim-limit` — allows animation IDs ≥ 506 (AdvFly) without weird backporting workflows.
   - `wxl-skyriding` — client companion for skyriding.
3. **AzerothCore server modules**:
   - `mod-skyriding` — **required**.
   - `ridingchanges.cpp` — this file lived inside my personal `mod-spells-qol` on my own server. I am including **only this file** (not the rest of that module) because skyriding was integrated on top of that riding work, and **I am not sure** whether skyriding now depends on it. The clear link is the flight-charges aura **98052**, which `mod-skyriding` checks before activating on a flying mount. This file is **not** a drop-in complete module by itself — you must wire it into your own server module / build the same way I did on Horizon.
4. **Client DBC / patch**:
   - Ship the two spells with IDs **98100** (Surge Forward) and **98101** (Skyward Ascent) from the `Spell.dbc` in this repo.
   - That `Spell.dbc` also contains **other custom Horizon content that nobody else needs** — use it only for those spells (or extract them) at your own risk.
   - Put a **mount** with AdvFly sequences into your patch (I use Tawny Wind Rider from wow.export). With `wxl-anim-limit` plus an extended AnimationData in patch, you can drag-and-drop that mount without inventing a backport pipeline.
5. **Addon** — drop `client/addon/SkyridingBar/` into `Interface/AddOns`. It handles the vigor UI / speed-pitch debug and forwards `HORIZON_SKY` messages to WXL.

---

## Spell IDs

| ID | Name (role) |
|----|-------------|
| **98100** | Surge Forward |
| **98101** | Skyward Ascent |

Source: `server/mod-skyriding` + `client/dbc/Spell.dbc` (patch-b).

---

## Known issues

- The **third spell** (full swirl) is missing — still not implemented.
- Some **small animation bugs** (often from spamming the spells continuously, which should **not** be possible on a normal server because they would have cooldowns).
- **Biggest bug** (currently unfixable for me): landing from flight and switching to the mount **ground mode** flawlessly. Right now there is animation flickering and weird wait times.
- **Another big bug**: I cannot make the ascent spell (**Skyward**) keep the momentum built up at the moment of cast. Currently it decelerates the mount and lifts ~40 yards with a coherent animation, but it loses the built speed.
- The **X slowdown** on the mount should be **removed**: I implemented it by mistake thinking live had it — **it does not**.
- (Not exactly a bug) Pitch-based acceleration / deceleration cannot be scraped correctly from retail WoW because Blizzard blocked Lua from reading pitch. The right curve has to be found through **playtesting over time**.

---

## Disclaimer

Rough, personal, incomplete code. If you fork it and finish it, you will help everyone who wants skyriding on 3.3.5a. No official support; use at your own risk.