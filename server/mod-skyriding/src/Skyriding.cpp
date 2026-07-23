/*
 * mod-skyriding — Horizon skyriding physics + vigor + Surge/Skyward.
 * Flight takeoff charges remain in mod-spells-qol ridingchanges (98052).
 */

#include "AllSpellScript.h"
#include "Chat.h"
#include "Config.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "MovementHandlerScript.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace
{
    constexpr char const* ADDON_PREFIX = "HORIZON_SKY";
    constexpr uint32 SPELL_FLIGHT_CHARGES = 98052;
    constexpr uint32 SPELL_SURGE_FORWARD = 98100;
    constexpr uint32 SPELL_SKYWARD_ASCENT = 98101;

    bool sEnabled = true;
    uint32 sVigorMax = 6;
    uint32 sVigorRechargeMs = 500;
    float sTurnRate = 0.75f;
    bool sClassicVertical = false;

    float sBaseFlightRate = 2.5f;   // 250% at takeoff / fall-into-flight
    float sMaxFlightRate = 6.5f;    // live skyriding peak
    float sMinFlightRate = 0.25f;   // floor while stalled (still coasts forward)
    float sStallThreshold = 1.0f;   // below this → wings can't hold → sink
    float sDivePitch = -0.12f;
    float sClimbPitch = 0.12f;
    float sDiveAccelPerSec = 0.55f; // slow climb to peak (~full dive: ~7s 2.5→6.5)
    float sHorizDecelPerSec = 0.35f;
    float sClimbDecelPerSec = 0.95f;
    float sBrakeDecelPerSec = 1.8f;
    float sStallSinkPerSec = 7.0f;
    float sSkywardSpeedZ = 42.0f;   // knockback jumpZ (ground takeoff + air climb)
    float sSurgeBoostRate = 1.5f;
    float sSkywardBoostRate = 2.0f; // air Skyward: add to flightRate after knockback
    uint32 sSpdIntervalMs = 400;
    uint32 sSkywardLandGraceMs = 700; // client takeoff grace mirror
    uint32 sGroundLockMs = 2000;      // after land: no fly re-entry (cliff = fall)


    struct SkyridingState
    {
        uint32 vigor = 6;
        uint32 rechargeLeftMs = 0;
        float flightRate = 3.0f;
        bool active = false;
        bool modeSynced = false;
        bool wasFlying = false;
        bool braking = false;
        uint32 landGraceUntilMs = 0;
        uint32 groundLockUntilMs = 0; // forced ground mode after land
        uint32 spdAccMs = 0;
        char const* band = "idle";
    };

    std::unordered_map<ObjectGuid, SkyridingState> sStates;

    void LoadConfig()
    {
        sEnabled = sConfigMgr->GetOption<bool>("Skyriding.Enable", true);
        sVigorMax = sConfigMgr->GetOption<uint32>("Skyriding.VigorMax", 6);
        sVigorRechargeMs = sConfigMgr->GetOption<uint32>("Skyriding.VigorRechargeMs", 500);
        sTurnRate = sConfigMgr->GetOption<float>("Skyriding.TurnRate", 0.75f);
        sClassicVertical = sConfigMgr->GetOption<bool>("Skyriding.ClassicVertical", false);

        sBaseFlightRate = sConfigMgr->GetOption<float>("Skyriding.BaseFlightRate", 2.5f);
        sMaxFlightRate = sConfigMgr->GetOption<float>("Skyriding.MaxFlightRate", 6.5f);
        sMinFlightRate = sConfigMgr->GetOption<float>("Skyriding.MinFlightRate", 0.25f);
        sStallThreshold = sConfigMgr->GetOption<float>("Skyriding.StallThreshold", 1.0f);
        sDivePitch = sConfigMgr->GetOption<float>("Skyriding.DivePitch", -0.12f);
        sClimbPitch = sConfigMgr->GetOption<float>("Skyriding.ClimbPitch", 0.12f);
        sDiveAccelPerSec = sConfigMgr->GetOption<float>("Skyriding.DiveAccelPerSec", 0.55f);
        sHorizDecelPerSec = sConfigMgr->GetOption<float>("Skyriding.HorizDecelPerSec", 0.35f);
        sClimbDecelPerSec = sConfigMgr->GetOption<float>("Skyriding.ClimbDecelPerSec", 0.95f);
        sBrakeDecelPerSec = sConfigMgr->GetOption<float>("Skyriding.BrakeDecelPerSec", 1.8f);
        sStallSinkPerSec = sConfigMgr->GetOption<float>("Skyriding.StallSinkPerSec", 7.0f);
        sSkywardSpeedZ = sConfigMgr->GetOption<float>("Skyriding.SkywardSpeedZ", 42.0f);
        sSurgeBoostRate = sConfigMgr->GetOption<float>("Skyriding.SurgeBoostRate", 1.5f);
        sSkywardBoostRate = sConfigMgr->GetOption<float>("Skyriding.SkywardBoostRate", 2.0f);
        sSpdIntervalMs = sConfigMgr->GetOption<uint32>("Skyriding.SpdIntervalMs", 400);
        sSkywardLandGraceMs = sConfigMgr->GetOption<uint32>("Skyriding.SkywardLandGraceMs", 700);
        sGroundLockMs = sConfigMgr->GetOption<uint32>("Skyriding.GroundLockMs", 2000);
    }

    void SendAddon(Player* player, std::string const& body)
    {
        if (!player || !player->GetSession())
            return;

        std::string const msg = std::string(ADDON_PREFIX) + "\t" + body;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, msg);
        player->SendDirectMessage(&data);
    }

    SkyridingState& GetState(Player* player)
    {
        ObjectGuid const guid = player->GetGUID();
        auto it = sStates.find(guid);
        if (it == sStates.end())
        {
            SkyridingState st;
            st.vigor = sVigorMax;
            st.flightRate = sBaseFlightRate;
            it = sStates.emplace(guid, st).first;
        }
        return it->second;
    }

    bool HasFlightCharges(Player* player)
    {
        Aura* aura = player->GetAura(SPELL_FLIGHT_CHARGES);
        return aura && aura->GetStackAmount() > 0;
    }

    bool IsSkyridingCandidate(Player* player)
    {
        if (!sEnabled || !player)
            return false;
        if (!player->IsMounted() || !player->CanFly())
            return false;
        if (!HasFlightCharges(player))
            return false;
        return true;
    }

    bool IsSkyridingActive(Player* player)
    {
        return IsSkyridingCandidate(player) && player->IsFlying();
    }

    void SyncCharges(Player* player, SkyridingState& st, bool force = false)
    {
        bool const wantActive = IsSkyridingCandidate(player);
        static std::unordered_map<ObjectGuid, uint32> sLastSentVigor;
        static std::unordered_map<ObjectGuid, uint32> sLastSentRecharge;
        static std::unordered_map<ObjectGuid, bool> sLastSentActive;

        ObjectGuid const guid = player->GetGUID();
        bool const changed = force
            || sLastSentVigor[guid] != st.vigor
            || sLastSentRecharge[guid] != st.rechargeLeftMs
            || sLastSentActive[guid] != wantActive;

        if (!changed)
            return;

        sLastSentVigor[guid] = st.vigor;
        sLastSentRecharge[guid] = st.rechargeLeftMs;
        sLastSentActive[guid] = wantActive;

        SendAddon(player,
            "CHG\t" + std::to_string(st.vigor)
            + "\t" + std::to_string(sVigorMax)
            + "\t" + std::to_string(st.rechargeLeftMs)
            + "\t" + (wantActive ? "1" : "0"));
    }

    void SyncMode(Player* player, SkyridingState& st, bool modeOn)
    {
        if (st.modeSynced && st.active == modeOn)
            return;

        st.active = modeOn;
        st.modeSynced = true;
        // MODE on while mounted+charges (ground OR air) so WXL gates Space before takeoff.
        SendAddon(player, std::string("MODE\t") + (modeOn ? "1" : "0"));
        SendAddon(player, std::string("VERT\t") + (sClassicVertical ? "1" : "0"));
        SendAddon(player, "TURN\t" + std::to_string(sTurnRate));
    }

    void ApplyTurnRate(Player* player, bool enable)
    {
        if (!player)
            return;
        if (enable)
            player->SetSpeed(MOVE_TURN_RATE, std::clamp(sTurnRate, 0.05f, 1.0f), true);
        else
            player->SetSpeed(MOVE_TURN_RATE, 1.0f, true);
    }

    void SyncRate(Player* player, SkyridingState& st)
    {
        // Normalized 0..1 for client anim bands (glide / slow / stall). Negative = stall.
        float span = std::max(0.01f, sMaxFlightRate - sMinFlightRate);
        float norm = (st.flightRate - sMinFlightRate) / span;
        if (norm < 0.f)
            norm = 0.f;
        if (norm > 1.f)
            norm = 1.f;
        if (st.flightRate < sStallThreshold)
            norm = -std::max(0.01f, norm);

        SendAddon(player, "RATE\t" + std::to_string(norm));
    }

    void SyncSpeedDebug(Player* player, SkyridingState& st, uint32 diff, float pitch)
    {
        st.spdAccMs += diff;
        if (st.spdAccMs < sSpdIntervalMs)
            return;
        st.spdAccMs = 0;

        // SPD\trate\tpitch\tband\tyardsPerSec  — UI test overlay (~2–3 Hz)
        float const yardsPerSec = player->GetSpeed(MOVE_FLIGHT);
        char buf[96];
        snprintf(buf, sizeof(buf), "SPD\t%.2f\t%.2f\t%s\t%.1f",
            st.flightRate, pitch, st.band ? st.band : "?", yardsPerSec);
        SendAddon(player, buf);
    }

    void EnsureSpells(Player* player)
    {
        if (!player)
            return;
        if (!player->HasSpell(SPELL_SURGE_FORWARD))
            player->learnSpell(SPELL_SURGE_FORWARD);
        if (!player->HasSpell(SPELL_SKYWARD_ASCENT))
            player->learnSpell(SPELL_SKYWARD_ASCENT);
    }

    void TickVigor(SkyridingState& st, uint32 diff)
    {
        if (st.vigor >= sVigorMax)
        {
            st.rechargeLeftMs = 0;
            return;
        }

        if (st.rechargeLeftMs == 0)
            st.rechargeLeftMs = sVigorRechargeMs;

        if (diff >= st.rechargeLeftMs)
        {
            uint32 left = diff - st.rechargeLeftMs;
            ++st.vigor;
            while (st.vigor < sVigorMax && left >= sVigorRechargeMs)
            {
                ++st.vigor;
                left -= sVigorRechargeMs;
            }
            st.rechargeLeftMs = (st.vigor >= sVigorMax) ? 0 : (sVigorRechargeMs - left);
        }
        else
            st.rechargeLeftMs -= diff;
    }

    void TickMomentum(Player* player, SkyridingState& st, uint32 diff)
    {
        float const dt = diff / 1000.f;
        float const pitch = player->m_movementInfo.pitch;

        // Pitch-scaled curvature: deeper dive / climb = stronger effect (gentler overall).
        if (pitch <= sDivePitch)
        {
            float strength = std::min(1.5f, (-pitch) / 0.9f);
            st.flightRate += sDiveAccelPerSec * strength * dt;
            st.band = "dive";
        }
        else if (pitch >= sClimbPitch)
        {
            float strength = std::min(1.5f, pitch / 0.9f);
            st.flightRate -= sClimbDecelPerSec * strength * dt;
            st.band = "climb";
        }
        else
        {
            st.flightRate -= sHorizDecelPerSec * dt;
            st.band = "level";
        }

        // S = brake (extra decel). Never hover-stop — mount keeps coasting forward.
        if (st.braking)
        {
            st.flightRate -= sBrakeDecelPerSec * dt;
            st.band = "brake";
        }

        if (st.flightRate > sMaxFlightRate)
            st.flightRate = sMaxFlightRate;
        if (st.flightRate < sMinFlightRate)
            st.flightRate = sMinFlightRate;

        // Below threshold → stall band (SlowFall anim via RATE) + sink Z.
        if (st.flightRate < sStallThreshold)
        {
            st.band = "stall";
            float const z = player->GetPositionZ() - sStallSinkPerSec * dt;
            player->UpdatePosition(
                player->GetPositionX(), player->GetPositionY(), z,
                player->GetOrientation());
        }

        player->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_BACKWARD);
        player->m_movementInfo.AddMovementFlag(MOVEMENTFLAG_FORWARD);

        player->SetSpeed(MOVE_FLIGHT, st.flightRate, true);
        SyncRate(player, st);
        SyncSpeedDebug(player, st, diff, pitch);
    }

    bool TryConsumeVigor(Player* player, SkyridingState& st)
    {
        if (st.vigor == 0)
            return false;
        --st.vigor;
        if (st.vigor < sVigorMax && st.rechargeLeftMs == 0)
            st.rechargeLeftMs = sVigorRechargeMs;
        SyncCharges(player, st, true);
        return true;
    }

    bool InGroundLock(SkyridingState const& st)
    {
        return st.groundLockUntilMs != 0 && getMSTime() < st.groundLockUntilMs;
    }

    void ArmGroundLock(Player* player, SkyridingState& st)
    {
        st.groundLockUntilMs = getMSTime() + sGroundLockMs;
        st.flightRate = sBaseFlightRate;
        st.braking = false;
        // Client: force ground mode for GroundLockMs (no re-fly; cliff = fall).
        SendAddon(player, "GLOCK\t" + std::to_string(sGroundLockMs));
    }

    void ApplyGroundLockFlags(Player* player)
    {
        if (!player)
            return;
        // Must clear FALLING too — otherwise server re-syncs Fall and client
        // plays "precipitate" for the whole GLOCK after soft land (client clears,
        // next move packet restores FALLING).
        player->m_movementInfo.RemoveMovementFlag(
            MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY
            | MOVEMENTFLAG_ASCENDING | MOVEMENTFLAG_DESCENDING
            | MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
        player->SetDisableGravity(false);
    }

    void ImpulseUp(Player* player, float speedZ)
    {
        float const x = player->GetPositionX();
        float const y = player->GetPositionY() - 0.01f;
        player->KnockbackFrom(x, y, 0.01f, speedZ);
    }

    // Air Skyward: knockback with current flight speed as XY so coast is not wiped.
    void ImpulseSkywardAir(Player* player, float speedZ)
    {
        float const o = player->GetOrientation();
        float speedXY = player->GetSpeed(MOVE_FLIGHT);
        if (speedXY < 7.0f)
            speedXY = 7.0f;
        float const bx = player->GetPositionX() - std::cos(o);
        float const by = player->GetPositionY() - std::sin(o);
        player->KnockbackFrom(bx, by, speedXY, speedZ);
    }

    void HandleSkyridingAbility(Player* player, uint32 spellId)
    {
        // Candidate = mounted flying mount with charges.
        // Surge requires already airborne.
        // Skyward ground = vertical knockback takeoff; air = knockback with XY=flight speed.
        if (!IsSkyridingCandidate(player))
            return;
        SkyridingState& st = GetState(player);
        if (InGroundLock(st))
            return; // forced ground mode — no Surge/Skyward re-fly
        if (spellId == SPELL_SURGE_FORWARD && !player->IsFlying())
            return;

        if (!TryConsumeVigor(player, st))
            return;

        if (spellId == SPELL_SURGE_FORWARD)
        {
            st.flightRate = std::min(sMaxFlightRate, st.flightRate + sSurgeBoostRate);
            player->SetSpeed(MOVE_FLIGHT, st.flightRate, true);
            SendAddon(player, "ANIM\t0");
        }
        else if (spellId == SPELL_SKYWARD_ASCENT)
        {
            bool const airborne = player->IsFlying();
            SendAddon(player, "ANIM\t1");

            if (airborne)
            {
                ImpulseSkywardAir(player, sSkywardSpeedZ);
                st.flightRate = std::min(sMaxFlightRate, st.flightRate + sSkywardBoostRate);
                player->SetSpeed(MOVE_FLIGHT, st.flightRate, true);
            }
            else
            {
                ImpulseUp(player, sSkywardSpeedZ);
                st.flightRate = std::max(st.flightRate, sBaseFlightRate);
                player->SetSpeed(MOVE_FLIGHT, st.flightRate, true);
            }
            // Client owns land; this only mirrors takeoff grace timing for docs/compat.
            st.landGraceUntilMs = getMSTime() + sSkywardLandGraceMs;
        }

        SyncRate(player, st);
    }
}

class SkyridingWorldScript : public WorldScript
{
public:
    SkyridingWorldScript() : WorldScript("SkyridingWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }

    void OnStartup() override
    {
        LoadConfig();
        LOG_INFO("module", "mod-skyriding: enabled={} vigorMax={} rechargeMs={}",
            sEnabled, sVigorMax, sVigorRechargeMs);
    }
};

class SkyridingPlayerScript : public PlayerScript
{
public:
    SkyridingPlayerScript() : PlayerScript("SkyridingPlayerScript") { }

    void OnPlayerLogin(Player* player) override
    {
        if (!sEnabled || !player)
            return;

        EnsureSpells(player);
        SkyridingState& st = GetState(player);
        st.vigor = sVigorMax;
        st.rechargeLeftMs = 0;
        st.flightRate = sBaseFlightRate;
        st.modeSynced = false;
        SyncCharges(player, st, true);
        SyncMode(player, st, false);
        st.landGraceUntilMs = 0;
        st.groundLockUntilMs = 0;
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;
        sStates.erase(player->GetGUID());
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!sEnabled || !player || !player->IsInWorld())
            return;

        SkyridingState& st = GetState(player);
        TickVigor(st, diff);
        SyncCharges(player, st);

        bool const candidate = IsSkyridingCandidate(player);
        bool const flying = player->IsFlying();
        bool const groundLocked = InGroundLock(st);

        if (candidate)
            EnsureSpells(player);

        // Mode ON on ground+air so client gates Space before takeoff; takeoff = Skyward.
        SyncMode(player, st, candidate);
        ApplyTurnRate(player, candidate && flying && !groundLocked);

        if (candidate && !sClassicVertical)
        {
            player->m_movementInfo.RemoveMovementFlag(
                MOVEMENTFLAG_ASCENDING | MOVEMENTFLAG_DESCENDING);

            if (groundLocked)
            {
                // Forced ground mode: strip fly even mid-fall so cliff = fall, not skyriding.
                ApplyGroundLockFlags(player);
            }
            else
            {
                // Ground Space tries ASC+FLY same frame — refuse FLYING if not already airborne.
                bool const falling = player->HasUnitMovementFlag(
                    MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
                if (!flying && !st.wasFlying && !falling)
                    player->m_movementInfo.RemoveMovementFlag(
                        MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY);
            }
        }

        if (candidate && flying && !groundLocked)
        {
            if (!st.wasFlying)
                st.flightRate = sBaseFlightRate;

            TickMomentum(player, st, diff);
        }
        else if (st.wasFlying && !flying)
        {
            ApplyTurnRate(player, false);
            st.flightRate = sBaseFlightRate;
            st.braking = false;
            // Solid land only — not mid-dive / knockback FALLING (false GLOCK zeroed pitch).
            bool const falling = player->HasUnitMovementFlag(
                MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
            if (!falling && getMSTime() >= st.landGraceUntilMs)
            {
                ArmGroundLock(player, st);
                ApplyGroundLockFlags(player);
            }
        }

        if (st.groundLockUntilMs != 0 && getMSTime() >= st.groundLockUntilMs)
            st.groundLockUntilMs = 0;

        st.wasFlying = flying && !groundLocked;
    }

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& /*type*/,
        uint32& lang, std::string& msg) override
    {
        if (!sEnabled || !player || lang != LANG_ADDON)
            return;

        size_t const tabPos = msg.find('\t');
        if (tabPos == std::string::npos)
            return;
        if (msg.substr(0, tabPos) != ADDON_PREFIX)
            return;

        std::string const body = msg.substr(tabPos + 1);
        // BRK\t0|1 — S held while skyriding (client cannot hover-stop).
        if (body.rfind("BRK\t", 0) == 0)
        {
            SkyridingState& st = GetState(player);
            st.braking = (body.size() > 4 && body[4] == '1');
        }
        // WALL\t1 — smash: instant momentum death → stall/precipitate (keep flying).
        else if (body.rfind("WALL\t", 0) == 0)
        {
            SkyridingState& st = GetState(player);
            if (!IsSkyridingCandidate(player) || InGroundLock(st))
                return;
            if (!player->IsFlying())
                return;

            st.flightRate = sMinFlightRate;
            st.braking = false;
            st.band = "stall";
            player->SetSpeed(MOVE_FLIGHT, st.flightRate, true);
            SyncRate(player, st);
        }
        // LAND\t1 — client confirmed vanilla land → GLOCK + kill rate (sync soft impacts).
        else if (body.rfind("LAND\t", 0) == 0)
        {
            SkyridingState& st = GetState(player);
            if (!IsSkyridingCandidate(player))
                return;
            st.flightRate = sBaseFlightRate;
            st.braking = false;
            ArmGroundLock(player, st);
            ApplyGroundLockFlags(player);
            st.wasFlying = false;
            // Push cleared flags so client FALLING does not bounce back from authority.
            player->SendMovementFlagUpdate(true);
        }
    }
};

class SkyridingMovementScript : public MovementHandlerScript
{
public:
    SkyridingMovementScript()
        : MovementHandlerScript("SkyridingMovementScript",
            { MOVEMENTHOOK_ON_PLAYER_MOVE })
    {
    }

    void OnPlayerMove(Player* player, MovementInfo /*movementInfo*/, uint32 /*opcode*/) override
    {
        if (!sEnabled || sClassicVertical || !player)
            return;
        if (!IsSkyridingCandidate(player))
            return;

        SkyridingState& st = GetState(player);
        player->m_movementInfo.RemoveMovementFlag(
            MOVEMENTFLAG_ASCENDING | MOVEMENTFLAG_DESCENDING);

        if (InGroundLock(st))
        {
            ApplyGroundLockFlags(player);
            return;
        }

        // Mirror client gate on every move packet (ground + air).
        bool const wasFlying = player->IsFlying();
        bool const falling = player->HasUnitMovementFlag(
            MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
        // Allow fall-into-fly: only strip bogus FLYING on solid ground.
        if (!wasFlying && !falling)
            player->m_movementInfo.RemoveMovementFlag(
                MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY);
    }
};

class SkyridingSpellScript : public AllSpellScript
{
public:
    SkyridingSpellScript()
        : AllSpellScript("SkyridingSpellScript",
            { ALLSPELLHOOK_ON_SPELL_CHECK_CAST, ALLSPELLHOOK_ON_CAST })
    {
    }

    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& res) override
    {
        if (!sEnabled || !spell || res != SPELL_CAST_OK)
            return;

        SpellInfo const* info = spell->GetSpellInfo();
        if (!info)
            return;
        if (info->Id != SPELL_SURGE_FORWARD && info->Id != SPELL_SKYWARD_ASCENT)
            return;

        Unit* caster = spell->GetCaster();
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || !IsSkyridingCandidate(player))
        {
            res = SPELL_FAILED_ONLY_MOUNTED;
            return;
        }

        if (info->Id == SPELL_SURGE_FORWARD && !player->IsFlying())
        {
            res = SPELL_FAILED_NOT_ON_GROUND;
            return;
        }

        SkyridingState& st = GetState(player);
        if (InGroundLock(st))
        {
            res = SPELL_FAILED_NOT_READY;
            return;
        }
        if (st.vigor == 0)
            res = SPELL_FAILED_DONT_REPORT;
    }

    void OnSpellCast(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (!sEnabled || !caster || !spellInfo)
            return;
        if (spellInfo->Id != SPELL_SURGE_FORWARD && spellInfo->Id != SPELL_SKYWARD_ASCENT)
            return;

        Player* player = caster->ToPlayer();
        if (!player)
            return;

        HandleSkyridingAbility(player, spellInfo->Id);
    }
};

void AddSC_mod_skyriding()
{
    new SkyridingWorldScript();
    new SkyridingPlayerScript();
    new SkyridingMovementScript();
    new SkyridingSpellScript();
}
