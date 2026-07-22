# WotLK Skyriding

Port sperimentale dello **skyriding** (dragonriding-style) su World of Warcraft **3.3.5a / WotLK**, basato su AzerothCore + WarcraftXL (WXL) lato client.

Questo progetto è **vibe-coddato**. Non lo pubblico per vantarmi, ma perché sia **open source** e perché qualcuno di davvero capace di coddare riesca a **finirlo**.

---

## Cosa c'è in questa repo

| Path | Contenuto |
|------|-----------|
| `client/wxl-scripts/wxl-skyriding/` | Modulo WXL: fisiche client, animazioni AdvFly, bridge Lua |
| `client/wxl-scripts/wxl-anim-limit/` | Modulo WXL: alza il tetto anim ID oltre 505 (necessario per AdvFly / mount drag-and-drop in patch) |
| `server/mod-skyriding/` | Modulo AzerothCore skyriding (fisiche server, vigor, Surge/Skyward, messaggi addon) |
| `server/mod-spells-qol/` | Modulo QoL **già presente** sul mio server personale, incluso per completezza (vedi sotto) |
| `client/dbc/Spell.dbc` | Il mio `Spell.dbc` da **patch-b** (contiene anche roba custom non legata allo skyriding) |
| `client/addon/SkyridingBar/` | Solo i pezzi UI che mostrano vigor / velocità / pitch e parlano con WXL — **non** l'addon intera |

**Non incluso:** asset della mount (M2/skin/texture/.anim). Va estratta a parte con wow.export (io uso **Tawny Wind Rider**).

---

## Architettura (in breve)

1. **WXL (WarcraftXL)** — obbligatorio. Il sistema estende le animazioni e il comportamento del client; **senza WXL non gira**.
2. **Moduli WXL** da droppare/abilitare nei scripts WXL:
   - `wxl-anim-limit` — permette anim ID ≥ 506 (AdvFly) senza processi strani di backporting.
   - `wxl-skyriding` — companion client dello skyriding.
3. **Moduli server AzerothCore**:
   - `mod-skyriding` — **necessario**.
   - `mod-spells-qol` — lo infilo anche se «non c'entra niente» con lo skyriding in sé: sul mio server personale c'era già, e **non so** se lavorandoci sopra ho reso lo skyriding dipendente da pezzi di quel modulo. Per completezza lo includo intero. La parte chiaramente collegata è `ridingchanges.cpp` (aura flight charges **98052**), che `mod-skyriding` usa come prerequisito per attivarsi sulla mount volante. Il resto del modulo è QoL personale non documentata qui.
4. **Client DBC / patch**:
   - Montare le due spell con ID **98100** (Surge Forward) e **98101** (Skyward Ascent) dal `Spell.dbc` in questa repo.
   - Quel `Spell.dbc` contiene **anche altra roba custom del mio progetto Horizon** che alla gente **non serve** — usatelo solo per quelle spell (o estraetele) a vostro rischio.
   - Mettere in patch una **mount** con le sequenze AdvFly (io: Tawny Wind Rider da wow.export). Con `wxl-anim-limit` + AnimationData esteso in patch potete drag-and-drop la mount senza backporting inventato.
5. **Addon** — droppare `client/addon/SkyridingBar/` in `Interface/AddOns`. Serve a UI vigor / debug velocità-pitch e a inoltrare i messaggi `HORIZON_SKY` verso WXL.

---

## Spell ID

| ID | Nome (uso) |
|----|------------|
| **98100** | Surge Forward |
| **98101** | Skyward Ascent |

Fonte: `server/mod-skyriding` + `client/dbc/Spell.dbc` (patch-b).

---

## Known issues

- Manca la **terza spell** che fa lo swirl completo — ancora da implementare.
- Alcuni **piccoli bug di animazioni** (spesso causati dallo spam continuo delle spell, che comunque **non** dovrebbe essere possibile su un server normale perché avrebbero cooldown).
- **Bug più grande** (per me impossibile da fixare al momento): quando si atterra da un volo, passare alla **ground mode** della mount in modo flawless. Ora ci sono flickering d'animazioni e strani tempi d'attesa.
- **Altro bug grande**: non riesco a far sì che la spell di ascesa (**Skyward**) mantenga il momento buildato al punto del cast. Attualmente decelerà la mount e la fa salire di ~40 yard con animazione coerente, ma perde lo «slancio».
- Va **rimosso il rallentamento della mount con X**: l'ho implementato per sbaglio credendo esistesse su live — **non esiste**.
- (Non esattamente un bug) L'accelerazione / decelerazione rispetto al **pitch** è impossibile da estrarre correttamente da retail perché Blizzard ha negato a Lua di controllare il pitch. La curva giusta va raggiunta col **testing nel tempo**.

---

## Disclaimer

Codice grezzo, personale, incompleto. Se lo forkate e lo finite, fate un favore a chi vuole skyriding su 3.3.5a. Non c'è supporto ufficiale; usate a vostro rischio.