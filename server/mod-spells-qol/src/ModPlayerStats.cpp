/*
 * GM temp combat-rating, primary-stat, and magic-resistance tweaks for testing.
 * Bonuses persist until logout or map change.
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Unit.h"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    struct ModStatState
    {
        std::array<int32, MAX_COMBAT_RATING> ratings{};
        std::array<int32, MAX_STATS> primaryStats{};
        int32 magicResist = 0;
    };

    static std::array<SpellSchools, 6> const MagicResistSchools =
    {
        SPELL_SCHOOL_HOLY,
        SPELL_SCHOOL_FIRE,
        SPELL_SCHOOL_NATURE,
        SPELL_SCHOOL_FROST,
        SPELL_SCHOOL_SHADOW,
        SPELL_SCHOOL_ARCANE,
    };

    static UnitMods SchoolToResistMod(SpellSchools school)
    {
        return UnitMods(UNIT_MOD_RESISTANCE_START + school);
    }

    static UnitMods StatToUnitMod(Stats stat)
    {
        return UnitMods(UNIT_MOD_STAT_START + stat);
    }

    static char const* PrimaryStatName(Stats stat)
    {
        switch (stat)
        {
            case STAT_STRENGTH: return "strength";
            case STAT_AGILITY: return "agility";
            case STAT_STAMINA: return "stamina";
            case STAT_INTELLECT: return "intellect";
            case STAT_SPIRIT: return "spirit";
            default: return "stat";
        }
    }

    static std::unordered_map<ObjectGuid, ModStatState> s_modStats;

    static char const* RatingName(CombatRating cr)
    {
        switch (cr)
        {
            case CR_WEAPON_SKILL: return "weapon skill";
            case CR_DEFENSE_SKILL: return "defense";
            case CR_DODGE: return "dodge";
            case CR_PARRY: return "parry";
            case CR_BLOCK: return "block";
            case CR_HIT_MELEE: return "hit melee";
            case CR_HIT_RANGED: return "hit ranged";
            case CR_HIT_SPELL: return "hit spell";
            case CR_CRIT_MELEE: return "crit melee";
            case CR_CRIT_RANGED: return "crit ranged";
            case CR_CRIT_SPELL: return "crit spell";
            case CR_HIT_TAKEN_MELEE: return "hit taken melee";
            case CR_HIT_TAKEN_RANGED: return "hit taken ranged";
            case CR_HIT_TAKEN_SPELL: return "hit taken spell";
            case CR_CRIT_TAKEN_MELEE: return "crit taken melee";
            case CR_CRIT_TAKEN_RANGED: return "crit taken ranged";
            case CR_CRIT_TAKEN_SPELL: return "crit taken spell";
            case CR_HASTE_MELEE: return "haste melee";
            case CR_HASTE_RANGED: return "haste ranged";
            case CR_HASTE_SPELL: return "haste spell";
            case CR_WEAPON_SKILL_MAINHAND: return "weapon skill mainhand";
            case CR_WEAPON_SKILL_OFFHAND: return "weapon skill offhand";
            case CR_WEAPON_SKILL_RANGED: return "weapon skill ranged";
            case CR_EXPERTISE: return "expertise";
            case CR_ARMOR_PENETRATION: return "armor penetration";
            default: return "rating";
        }
    }

    static Player* GetModTarget(ChatHandler* handler)
    {
        if (!handler)
            return nullptr;

        Player* target = handler->getSelectedPlayer();
        if (!target)
            target = handler->GetPlayer();
        return target;
    }

    static void ApplyModRating(Player* player, CombatRating cr, int32 newValue)
    {
        if (!player)
            return;

        ModStatState& state = s_modStats[player->GetGUID()];
        int32& stored = state.ratings[cr];
        if (stored)
            player->ApplyRatingMod(cr, stored, false);

        stored = newValue;
        if (stored)
            player->ApplyRatingMod(cr, stored, true);
    }

    static void ApplyModRatingGroup(Player* player,
        std::vector<CombatRating> const& ratings, int32 value)
    {
        for (CombatRating cr : ratings)
            ApplyModRating(player, cr, value);
    }

    static void ApplyModPrimaryStat(Player* player, Stats stat, int32 newValue)
    {
        if (!player || stat >= MAX_STATS)
            return;

        ModStatState& state = s_modStats[player->GetGUID()];
        int32& stored = state.primaryStats[stat];
        if (stored)
            player->HandleStatFlatModifier(StatToUnitMod(stat),
                TOTAL_VALUE, float(stored), false);

        stored = newValue;
        if (stored)
            player->HandleStatFlatModifier(StatToUnitMod(stat),
                TOTAL_VALUE, float(stored), true);

        player->UpdateStats(stat);
    }

    static void ApplyModMagicResist(Player* player, int32 newValue)
    {
        if (!player)
            return;

        ModStatState& state = s_modStats[player->GetGUID()];
        int32 const previous = state.magicResist;
        if (previous)
        {
            for (SpellSchools school : MagicResistSchools)
                player->HandleStatFlatModifier(SchoolToResistMod(school),
                    TOTAL_VALUE, float(previous), false);
        }

        state.magicResist = newValue;
        if (state.magicResist)
        {
            for (SpellSchools school : MagicResistSchools)
                player->HandleStatFlatModifier(SchoolToResistMod(school),
                    TOTAL_VALUE, float(state.magicResist), true);
        }
    }

    static void ClearAllModStats(Player* player)
    {
        if (!player)
            return;

        auto itr = s_modStats.find(player->GetGUID());
        if (itr == s_modStats.end())
            return;

        for (uint8 cr = 0; cr < MAX_COMBAT_RATING; ++cr)
        {
            int32 const amount = itr->second.ratings[cr];
            if (amount)
                player->ApplyRatingMod(CombatRating(cr), amount, false);
        }

        for (uint8 stat = 0; stat < MAX_STATS; ++stat)
        {
            int32 const amount = itr->second.primaryStats[stat];
            if (!amount)
                continue;

            player->HandleStatFlatModifier(StatToUnitMod(Stats(stat)),
                TOTAL_VALUE, float(amount), false);
            player->UpdateStats(Stats(stat));
        }

        if (itr->second.magicResist)
            ApplyModMagicResist(player, 0);

        s_modStats.erase(itr);
    }

    static bool HandleModRatingGroup(ChatHandler* handler, int32 value,
        std::vector<CombatRating> const& ratings, char const* label)
    {
        Player* target = GetModTarget(handler);
        if (!target)
        {
            handler->SendSysMessage("No target player.");
            return false;
        }

        ApplyModRatingGroup(target, ratings, value);
        handler->PSendSysMessage(
            "|cff00ff00.modify {}|r on {} set to {} (clears on logout/map change).",
            label, handler->GetNameLink(target), value);
        return true;
    }

    static bool HandleModHasteCommand(ChatHandler* handler, int32 value)
    {
        return HandleModRatingGroup(handler, value,
            { CR_HASTE_SPELL, CR_HASTE_MELEE, CR_HASTE_RANGED }, "haste");
    }

    static bool HandleModCritCommand(ChatHandler* handler, int32 value)
    {
        return HandleModRatingGroup(handler, value,
            { CR_CRIT_SPELL, CR_CRIT_MELEE, CR_CRIT_RANGED }, "crit");
    }

    static bool HandleModHitCommand(ChatHandler* handler, int32 value)
    {
        return HandleModRatingGroup(handler, value,
            { CR_HIT_SPELL, CR_HIT_MELEE, CR_HIT_RANGED }, "hit");
    }

    static bool HandleModExpertiseCommand(ChatHandler* handler, int32 value)
    {
        return HandleModRatingGroup(handler, value, { CR_EXPERTISE }, "expertise");
    }

    static bool HandleModArpCommand(ChatHandler* handler, int32 value)
    {
        return HandleModRatingGroup(handler, value,
            { CR_ARMOR_PENETRATION }, "arp");
    }

    static bool HandleModDodgeCommand(ChatHandler* handler, int32 value)
    {
        return HandleModRatingGroup(handler, value, { CR_DODGE }, "dodge");
    }

    static bool HandleModParryCommand(ChatHandler* handler, int32 value)
    {
        return HandleModRatingGroup(handler, value, { CR_PARRY }, "parry");
    }

    static bool HandleModBlockCommand(ChatHandler* handler, int32 value)
    {
        return HandleModRatingGroup(handler, value, { CR_BLOCK }, "block");
    }

    static bool HandleModDefenseCommand(ChatHandler* handler, int32 value)
    {
        return HandleModRatingGroup(handler, value,
            { CR_DEFENSE_SKILL }, "defense");
    }

    static bool HandleModPrimaryStatCommand(ChatHandler* handler, int32 value,
        Stats stat)
    {
        Player* target = GetModTarget(handler);
        if (!target)
        {
            handler->SendSysMessage("No target player.");
            return false;
        }

        ApplyModPrimaryStat(target, stat, value);
        handler->PSendSysMessage(
            "|cff00ff00.modify {}|r on {} set to {} (clears on logout/map change).",
            PrimaryStatName(stat), handler->GetNameLink(target), value);
        return true;
    }

    static bool HandleModStrengthCommand(ChatHandler* handler, int32 value)
    {
        return HandleModPrimaryStatCommand(handler, value, STAT_STRENGTH);
    }

    static bool HandleModAgilityCommand(ChatHandler* handler, int32 value)
    {
        return HandleModPrimaryStatCommand(handler, value, STAT_AGILITY);
    }

    static bool HandleModStaminaCommand(ChatHandler* handler, int32 value)
    {
        return HandleModPrimaryStatCommand(handler, value, STAT_STAMINA);
    }

    static bool HandleModIntellectCommand(ChatHandler* handler, int32 value)
    {
        return HandleModPrimaryStatCommand(handler, value, STAT_INTELLECT);
    }

    static bool HandleModSpiritCommand(ChatHandler* handler, int32 value)
    {
        return HandleModPrimaryStatCommand(handler, value, STAT_SPIRIT);
    }

    static bool HandleModMresCommand(ChatHandler* handler, int32 value)
    {
        Player* target = GetModTarget(handler);
        if (!target)
        {
            handler->SendSysMessage("No target player.");
            return false;
        }

        ApplyModMagicResist(target, value);
        handler->PSendSysMessage(
            "|cff00ff00.modify mres|r on {} set to {} on holy/fire/nature/frost/shadow/arcane "
            "(clears on logout/map change).",
            handler->GetNameLink(target), value);
        return true;
    }

    static bool HandleModResetCommand(ChatHandler* handler)
    {
        Player* target = GetModTarget(handler);
        if (!target)
        {
            handler->SendSysMessage("No target player.");
            return false;
        }

        ClearAllModStats(target);
        handler->PSendSysMessage(
            "|cffffaa00.modify reset|r on {} — all temp stat/rating/resist bonuses removed.",
            handler->GetNameLink(target));
        return true;
    }

    static bool HandleModListCommand(ChatHandler* handler)
    {
        Player* target = GetModTarget(handler);
        if (!target)
        {
            handler->SendSysMessage("No target player.");
            return false;
        }

        auto itr = s_modStats.find(target->GetGUID());
        if (itr == s_modStats.end())
        {
            handler->PSendSysMessage("{} has no active .modify bonuses.",
                handler->GetNameLink(target));
            return true;
        }

        handler->PSendSysMessage(
            "|cff00ff00.modify list|r for {} (clears on logout/map change):",
            handler->GetNameLink(target));

        bool any = false;
        for (uint8 cr = 0; cr < MAX_COMBAT_RATING; ++cr)
        {
            int32 const amount = itr->second.ratings[cr];
            if (!amount)
                continue;

            any = true;
            handler->PSendSysMessage("  {}: {}", RatingName(CombatRating(cr)), amount);
        }

        for (uint8 stat = 0; stat < MAX_STATS; ++stat)
        {
            int32 const amount = itr->second.primaryStats[stat];
            if (!amount)
                continue;

            any = true;
            handler->PSendSysMessage("  {}: {}", PrimaryStatName(Stats(stat)), amount);
        }

        if (itr->second.magicResist)
        {
            any = true;
            handler->PSendSysMessage("  mres (holy/fire/nature/frost/shadow/arcane): {}",
                itr->second.magicResist);
        }

        if (!any)
            handler->SendSysMessage("  (none)");

        return true;
    }
}

class ModPlayerStats_PlayerScript : public PlayerScript
{
public:
    ModPlayerStats_PlayerScript()
        : PlayerScript("ModPlayerStats_PlayerScript",
            { PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_MAP_CHANGED }) {}

    void OnPlayerLogout(Player* player) override
    {
        ClearAllModStats(player);
    }

    void OnPlayerMapChanged(Player* player) override
    {
        ClearAllModStats(player);
    }
};

class ModPlayerStats_CommandScript : public CommandScript
{
public:
    ModPlayerStats_CommandScript()
        : CommandScript("ModPlayerStats_CommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable modifyStatCommandTable =
        {
            { "haste",      HandleModHasteCommand,      SEC_GAMEMASTER, Console::No },
            { "crit",       HandleModCritCommand,       SEC_GAMEMASTER, Console::No },
            { "hit",        HandleModHitCommand,        SEC_GAMEMASTER, Console::No },
            { "expertise",  HandleModExpertiseCommand,  SEC_GAMEMASTER, Console::No },
            { "arp",        HandleModArpCommand,        SEC_GAMEMASTER, Console::No },
            { "dodge",      HandleModDodgeCommand,      SEC_GAMEMASTER, Console::No },
            { "parry",      HandleModParryCommand,      SEC_GAMEMASTER, Console::No },
            { "block",      HandleModBlockCommand,      SEC_GAMEMASTER, Console::No },
            { "defense",    HandleModDefenseCommand,    SEC_GAMEMASTER, Console::No },
            { "strength",   HandleModStrengthCommand,   SEC_GAMEMASTER, Console::No },
            { "agility",    HandleModAgilityCommand,    SEC_GAMEMASTER, Console::No },
            { "stamina",    HandleModStaminaCommand,    SEC_GAMEMASTER, Console::No },
            { "intellect",  HandleModIntellectCommand,  SEC_GAMEMASTER, Console::No },
            { "spirit",     HandleModSpiritCommand,     SEC_GAMEMASTER, Console::No },
            { "mres",       HandleModMresCommand,       SEC_GAMEMASTER, Console::No },
            { "reset",      HandleModResetCommand,      SEC_GAMEMASTER, Console::No },
            { "list",       HandleModListCommand,       SEC_GAMEMASTER, Console::No },
        };

        static ChatCommandTable commandTable =
        {
            { "modify", modifyStatCommandTable },
        };

        return commandTable;
    }
};

void AddSC_ModPlayerStats()
{
    new ModPlayerStats_PlayerScript();
    new ModPlayerStats_CommandScript();
}
