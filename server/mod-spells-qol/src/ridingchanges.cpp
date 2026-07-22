#include "ScriptMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellAuras.h"
#include "Spell.h"
#include "Chat.h"
#include "mod-noflyzone/src/NoFlyZone.h"
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <ctime>

using namespace Acore::ChatCommands;

/*
 * Riding Changes Module - REWRITTEN V8
 * 1. Independent Flight Charges (3 slots, 10m each).
 * 2. PERSISTENCE: Cooldowns progress while offline.
 * 3. Charge consumption on DISMOUNT if the player flew.
 * 4. MOUNTED SPEED: Updated rates and acceleration per user request.
 *    - 150: 2.15 -> 2.35 (20s)
 *    - 225: 3.50 -> 4.10 (40s)
 *    - 300: 4.10 -> 5.00 (40s)
 */

#define SPELL_FLIGHT_CHARGES 98052
#define SPELL_FLIGHT_EMPTY   98053
#define SPELL_DRUID_FLIGHT_FORM 33943
#define SPELL_DRUID_SWIFT_FLIGHT_FORM 40120
#define FLIGHT_CHARGE_MAX_STACKS 3
#define FLIGHT_CHARGE_RECHARGE_MS 1200000

struct ChargeData
{
    uint32 slots[3] = { 0, 0, 0 };
    bool wasMounted = false;
    bool pendingCharge = false;
    time_t lastLogoutTimestamp = 0;
};

static std::unordered_map<ObjectGuid, ChargeData> playerCharges;
static std::unordered_map<ObjectGuid, uint32> mountScalingTimers;
static std::unordered_map<ObjectGuid, float> lastRunSpeed;
static std::unordered_map<ObjectGuid, float> lastFlightSpeed;

class mod_riding_world_changes : public WorldScript
{
public:
    mod_riding_world_changes() : WorldScript("mod_riding_world_changes") { }
    void OnStartup() override
    {
        if (SpellInfo* spellInfo = const_cast<SpellInfo*>(sSpellMgr->GetSpellInfo(54197)))
            spellInfo->SpellLevel = 40;
    }
};

class mod_riding_changes : public PlayerScript
{
public:
    mod_riding_changes() : PlayerScript("mod_riding_changes") { }

    void OnPlayerLogin(Player* player) override
    {
        if (player->GetLevel() == 1 && !player->HasSpell(33388))
            player->learnSpell(33388, false);

        ObjectGuid guid = player->GetGUID();
        if (!playerCharges.count(guid))
            playerCharges[guid] = ChargeData();
        else
        {
            ChargeData& data = playerCharges[guid];
            if (data.lastLogoutTimestamp > 0)
            {
                time_t now = time(nullptr);
                uint32 diffMs = uint32(difftime(now, data.lastLogoutTimestamp)) * 1000;
                for (int i = 0; i < 3; ++i)
                {
                    if (data.slots[i] > 0)
                    {
                        if (data.slots[i] <= diffMs) data.slots[i] = 0;
                        else data.slots[i] -= diffMs;
                    }
                }
            }
        }
        SyncAuraVisual(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        ObjectGuid guid = player->GetGUID();
        if (playerCharges.count(guid))
        {
            playerCharges[guid].lastLogoutTimestamp = time(nullptr);
            playerCharges[guid].wasMounted = false;
            playerCharges[guid].pendingCharge = false;
        }
        mountScalingTimers.erase(guid);
        lastRunSpeed.erase(guid);
        lastFlightSpeed.erase(guid);
    }

    bool OnPlayerBeforeAchievementComplete(Player* player, AchievementEntry const*) override
    {
        return player->GetLevel() != 1;
    }

    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        ObjectGuid guid = player->GetGUID();
        uint16 ridingSkill = player->GetSkillValue(762);

        if (!playerCharges.count(guid)) playerCharges[guid] = ChargeData();
        ChargeData& data = playerCharges[guid];

        for (int i = 0; i < 3; ++i)
        {
            if (data.slots[i] > 0)
            {
                if (data.slots[i] <= p_time) data.slots[i] = 0;
                else data.slots[i] -= p_time;
            }
        }

        bool isMounted = player->IsMounted();
        bool isManagedDruidFlightForm = IsManagedDruidFlightForm(player);
        bool isRideState = isMounted || isManagedDruidFlightForm;
        if (isRideState)
        {
            if (!data.pendingCharge && player->IsFlying())
                data.pendingCharge = true;
        }
        else if (data.wasMounted)
        {
            if (data.pendingCharge)
            {
                for (int i = 0; i < 3; ++i)
                {
                    if (data.slots[i] == 0)
                    {
                        data.slots[i] = FLIGHT_CHARGE_RECHARGE_MS;
                        break;
                    }
                }
                data.pendingCharge = false;
            }
        }
        data.wasMounted = isRideState;

        SyncAuraVisual(player);

        if (!isRideState)
        {
            if (mountScalingTimers.count(guid))
            {
                mountScalingTimers.erase(guid);
                lastRunSpeed.erase(guid);
                lastFlightSpeed.erase(guid);
                player->SetSpeed(MOVE_RUN, 1.0f, true);
                player->SetSpeed(MOVE_FLIGHT, 1.0f, true);
            }
            return;
        }

        uint32 available = 0;
        for (int i = 0; i < 3; ++i) if (data.slots[i] == 0) available++;

        bool isFlyingMount = player->HasAuraType(SPELL_AURA_MOD_INCREASE_FLIGHT_SPEED) || 
                             player->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) ||
                             player->HasAuraType(SPELL_AURA_MOD_FLIGHT_SPEED_ALWAYS) ||
                             isManagedDruidFlightForm;

        bool isGroundMount = player->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED);

        if (!isFlyingMount && !isGroundMount)
            return;

        uint32& currentTimer = mountScalingTimers[guid];
        currentTimer += p_time;
        uint32 seconds = currentTimer / 1000;

        // --- Manual Bonus Detection ---
        float manualBonus = 0.0f;
        if (player->HasAura(26022)) manualBonus = std::max(manualBonus, 0.08f);
        if (player->HasAura(26023)) manualBonus = std::max(manualBonus, 0.15f);
        if (player->HasAura(49146)) manualBonus = std::max(manualBonus, 0.10f);
        if (player->HasAura(51267)) manualBonus = std::max(manualBonus, 0.10f);
        if (player->HasAura(19559)) manualBonus = std::max(manualBonus, 0.05f);
        if (player->HasAura(19560)) manualBonus = std::max(manualBonus, 0.10f);
        if (player->HasAura(13947)) manualBonus = std::max(manualBonus, 0.02f);
        if (player->HasAura(9783))  manualBonus = std::max(manualBonus, 0.05f);
        if (player->HasAura(13587)) manualBonus = std::max(manualBonus, 0.03f);
        if (player->HasAura(32223))
        {
            float crusader = 0.20f;
            if (player->HasAura(31821)) crusader = 0.40f;
            manualBonus = std::max(manualBonus, crusader);
        }

        // --- UPDATED BASE VALUES ---
        float targetRunSpeedRate = (ridingSkill <= 75) ? 1.80f : 2.15f;
        float maxRun = (ridingSkill <= 75) ? 2.00f : 2.35f;
        float runAccel = 0.01f; // 20s to reach max for both 75 and 150
        
        targetRunSpeedRate += manualBonus;
        maxRun += manualBonus;

        targetRunSpeedRate += (seconds * runAccel);
        if (targetRunSpeedRate > maxRun) targetRunSpeedRate = maxRun;

        float targetFlightSpeedRate = targetRunSpeedRate;
        bool canPlayerFly = (ridingSkill >= 225 && available > 0);

        bool inNoFlyZone = IsPlayerInsideNoFlyZone(player);

        uint32 zoneId = player->GetZoneId();
        uint32 areaId = player->GetAreaId();
        bool isInsideDalaran = (zoneId == 4395 || zoneId == 4613 || areaId == 4395 || areaId == 4613);
        bool isOnBalcony = (areaId == 4564 || areaId == 4598);
        if (player->GetMap()->Instanceable() || player->GetMap()->IsBattleground() || 
            (isInsideDalaran && !isOnBalcony) || areaId == 4378 || areaId == 4500)
            canPlayerFly = false;

        if (inNoFlyZone && !player->IsFlying())
            canPlayerFly = false;

        if (isFlyingMount && canPlayerFly)
        {
            player->SetCanFly(true);
            float baseFlight = (ridingSkill == 225) ? 3.50f : 4.10f;
            float maxFlight = (ridingSkill == 225) ? 4.10f : 5.00f;
            float flightAccel = (ridingSkill == 225) ? 0.015f : 0.0225f; // 40s to reach max
            
            baseFlight += manualBonus;
            maxFlight += manualBonus;
            
            targetFlightSpeedRate = baseFlight + (seconds * flightAccel);
            if (targetFlightSpeedRate > maxFlight) targetFlightSpeedRate = maxFlight;
        }
        else
        {
            player->SetCanFly(false);
            if (player->IsFlying() && !inNoFlyZone)
            {
                if (isManagedDruidFlightForm)
                    RemoveManagedDruidFlightForm(player);
                else
                    player->RemoveAurasByType(SPELL_AURA_MOUNTED);
            }
        }

        // While airborne, mod-skyriding owns MOVE_FLIGHT (pitch momentum / stall).
        // Overwriting it here made skyriding speed changes invisible.
        bool const skyridingOwnsFlight = player->IsFlying() && isFlyingMount && canPlayerFly;
        if (lastRunSpeed[guid] != targetRunSpeedRate)
        {
            player->SetSpeed(MOVE_RUN, targetRunSpeedRate, true);
            lastRunSpeed[guid] = targetRunSpeedRate;
        }
        if (!skyridingOwnsFlight && lastFlightSpeed[guid] != targetFlightSpeedRate)
        {
            player->SetSpeed(MOVE_FLIGHT, targetFlightSpeedRate, true);
            lastFlightSpeed[guid] = targetFlightSpeedRate;
        }
        else if (skyridingOwnsFlight)
            lastFlightSpeed[guid] = targetFlightSpeedRate; // keep cache warm; do not apply
    }

private:
    bool IsManagedDruidFlightForm(Player* player) const
    {
        return player && (player->HasAura(SPELL_DRUID_FLIGHT_FORM) ||
            player->HasAura(SPELL_DRUID_SWIFT_FLIGHT_FORM));
    }

    void RemoveManagedDruidFlightForm(Player* player) const
    {
        if (!player)
            return;

        player->RemoveAurasDueToSpell(SPELL_DRUID_FLIGHT_FORM);
        player->RemoveAurasDueToSpell(SPELL_DRUID_SWIFT_FLIGHT_FORM);
    }

    void SyncAuraVisual(Player* player)
    {
        uint16 ridingSkill = player->GetSkillValue(762);
        if (ridingSkill < 225)
        {
            if (player->HasAura(SPELL_FLIGHT_CHARGES)) player->RemoveAura(SPELL_FLIGHT_CHARGES);
            if (player->HasAura(SPELL_FLIGHT_EMPTY)) player->RemoveAura(SPELL_FLIGHT_EMPTY);
            return;
        }

        ObjectGuid guid = player->GetGUID();
        if (!playerCharges.count(guid)) return;
        ChargeData& data = playerCharges[guid];
        uint32 available = 0;
        uint32 minRemaining = 0xFFFFFFFF;
        for (int i = 0; i < 3; ++i)
        {
            if (data.slots[i] == 0) available++;
            else if (data.slots[i] < minRemaining) minRemaining = data.slots[i];
        }

        bool hideVisual = player->GetMap()->Instanceable() || player->GetMap()->IsBattleground();

        if (available > 0)
        {
            if (player->HasAura(SPELL_FLIGHT_EMPTY)) player->RemoveAura(SPELL_FLIGHT_EMPTY);
            if (hideVisual)
            {
                if (player->HasAura(SPELL_FLIGHT_CHARGES)) player->RemoveAura(SPELL_FLIGHT_CHARGES);
            }
            else
            {
                if (!player->HasAura(SPELL_FLIGHT_CHARGES)) player->AddAura(SPELL_FLIGHT_CHARGES, player);
                if (Aura* aura = player->GetAura(SPELL_FLIGHT_CHARGES))
                {
                    if (aura->GetStackAmount() != available) aura->SetStackAmount(available);
                    aura->SetDuration(-1);
                    aura->SetMaxDuration(-1);
                }
            }
        }
        else
        {
            if (player->HasAura(SPELL_FLIGHT_CHARGES)) player->RemoveAura(SPELL_FLIGHT_CHARGES);
            if (hideVisual)
            {
                if (player->HasAura(SPELL_FLIGHT_EMPTY)) player->RemoveAura(SPELL_FLIGHT_EMPTY);
            }
            else
            {
                if (!player->HasAura(SPELL_FLIGHT_EMPTY))
                {
                    if (Aura* aura = player->AddAura(SPELL_FLIGHT_EMPTY, player))
                    {
                        aura->SetDuration(minRemaining);
                        aura->SetMaxDuration(minRemaining);
                    }
                }
            }
        }
    }
};

class mod_riding_command : public CommandScript
{
public:
    mod_riding_command() : CommandScript("mod_riding_command") { }
    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable ridingCommandTable = { { "check", HandleRidingStacksCheck, SEC_PLAYER, Console::No } };
        return { { "rsc", ridingCommandTable }, { "ridingstackscheck", ridingCommandTable } };
    }
    static bool HandleRidingStacksCheck(ChatHandler* handler, char const*)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player) return false;
        ObjectGuid guid = player->GetGUID();
        if (player->GetSkillValue(762) < 225) { handler->SendSysMessage("System inactive."); return true; }
        ChargeData& data = playerCharges[guid];
        handler->PSendSysMessage("--- Flight Charges Status ---");
        for (int i = 0; i < 3; ++i)
        {
            if (data.slots[i] == 0) handler->PSendSysMessage("Slot %d: READY", i + 1);
            else handler->PSendSysMessage("Slot %d: %02u:%02u", i + 1, (data.slots[i]/60000), (data.slots[i]%60000)/1000);
        }
        return true;
    }
};

class mod_riding_level_up : public PlayerScript
{
public:
    mod_riding_level_up() : PlayerScript("mod_riding_level_up") { }
    void OnPlayerLevelChanged(Player* player, uint8) override
    {
        if (player->GetLevel() >= 4 && !player->HasSpell(54197)) player->learnSpell(54197, false);
    }
};

void AddSC_mod_riding_changes()
{
    new mod_riding_world_changes();
    new mod_riding_changes();
    new mod_riding_level_up();
    new mod_riding_command();
}
