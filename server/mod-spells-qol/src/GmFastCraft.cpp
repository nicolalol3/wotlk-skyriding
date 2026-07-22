/*
 * GM toggle: profession crafts and gathering use 0.15s cast time.
 */

#include "Chat.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <unordered_set>

using namespace Acore::ChatCommands;

namespace
{
    static std::unordered_set<ObjectGuid> s_gmFastCraftEnabled;

    static bool SpellHasSkinningEffect(SpellInfo const* spellInfo)
    {
        if (!spellInfo)
            return false;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_SKINNING)
                return true;
        }

        return false;
    }

    static bool SpellIsGatheringSkillLine(uint32 skillLine)
    {
        return skillLine == SKILL_HERBALISM
            || skillLine == SKILL_MINING
            || skillLine == SKILL_SKINNING;
    }

    static bool SpellIsGatheringProfession(SpellInfo const* spellInfo)
    {
        if (!spellInfo)
            return false;

        if (SpellHasSkinningEffect(spellInfo))
            return true;

        bool hasOpenLock = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_OPEN_LOCK)
                hasOpenLock = true;
        }

        if (!hasOpenLock)
            return false;

        SkillLineAbilityMapBounds bounds =
            sSpellMgr->GetSkillLineAbilityMapBounds(spellInfo->Id);
        for (auto it = bounds.first; it != bounds.second; ++it)
        {
            if (SpellIsGatheringSkillLine(it->second->SkillLine))
                return true;
        }

        return false;
    }

    static bool IsProfessionCraftSpell(SpellInfo const* spellInfo)
    {
        if (!spellInfo || spellInfo->IsChanneled())
            return false;

        bool createsItem = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            uint8 const effect = spellInfo->Effects[i].Effect;
            if (effect == SPELL_EFFECT_CREATE_ITEM || effect == SPELL_EFFECT_CREATE_ITEM_2)
            {
                if (spellInfo->Effects[i].ItemType)
                    createsItem = true;
            }
        }

        if (!createsItem)
            return false;

        SkillLineAbilityMapBounds bounds =
            sSpellMgr->GetSkillLineAbilityMapBounds(spellInfo->Id);
        for (auto it = bounds.first; it != bounds.second; ++it)
        {
            if (it->second->SkillLine && IsProfessionSkill(it->second->SkillLine))
                return true;
        }

        return false;
    }

    static bool SpellIsDisenchant(SpellInfo const* spellInfo)
    {
        return spellInfo && spellInfo->HasEffect(SPELL_EFFECT_DISENCHANT);
    }

    static bool SpellIsMiningSmelt(SpellInfo const* spellInfo)
    {
        if (!spellInfo)
            return false;

        bool createsItem = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            uint8 const effect = spellInfo->Effects[i].Effect;
            if ((effect == SPELL_EFFECT_CREATE_ITEM
                    || effect == SPELL_EFFECT_CREATE_ITEM_2)
                && spellInfo->Effects[i].ItemType)
                createsItem = true;
        }

        if (!createsItem)
            return false;

        SkillLineAbilityMapBounds bounds =
            sSpellMgr->GetSkillLineAbilityMapBounds(spellInfo->Id);
        for (auto it = bounds.first; it != bounds.second; ++it)
        {
            if (it->second->SkillLine == SKILL_MINING)
                return true;
        }

        return false;
    }

    static bool SpellIsMillingOrProspecting(SpellInfo const* spellInfo)
    {
        if (!spellInfo)
            return false;

        return spellInfo->HasEffect(SPELL_EFFECT_MILLING)
            || spellInfo->HasEffect(SPELL_EFFECT_PROSPECTING);
    }

    static bool IsFastCraftSpell(SpellInfo const* spellInfo)
    {
        return IsProfessionCraftSpell(spellInfo)
            || SpellIsGatheringProfession(spellInfo)
            || SpellIsDisenchant(spellInfo)
            || SpellIsMiningSmelt(spellInfo)
            || SpellIsMillingOrProspecting(spellInfo);
    }
}

class GmFastCraft_PlayerScript : public PlayerScript
{
public:
    GmFastCraft_PlayerScript() : PlayerScript("GmFastCraft_PlayerScript",
        { PLAYERHOOK_ON_MOD_SPELL_CAST_TIME }) {}

    void OnPlayerModSpellCastTime(Player* player, SpellInfo const* spellInfo,
        int32& castTime, Spell* /*spell*/) override
    {
        if (!player || castTime <= 0)
            return;

        if (!s_gmFastCraftEnabled.count(player->GetGUID()))
            return;

        if (!IsFastCraftSpell(spellInfo))
            return;

        castTime = 150;
    }
};

class GmFastCraft_CommandScript : public CommandScript
{
public:
    GmFastCraft_CommandScript() : CommandScript("GmFastCraft_CommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "gmfastcraft", HandleGmFastCraftCommand, SEC_GAMEMASTER, Console::No },
        };
        return commandTable;
    }

    static bool HandleGmFastCraftCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return true;

        ObjectGuid const guid = player->GetGUID();
        if (s_gmFastCraftEnabled.count(guid))
        {
            s_gmFastCraftEnabled.erase(guid);
            handler->SendSysMessage("|cffffaa00GM fast craft disabled.|r");
        }
        else
        {
            s_gmFastCraftEnabled.insert(guid);
            handler->SendSysMessage(
                "|cff00ff00GM fast craft enabled.|r "
                "Profession crafts, disenchanting, smelting, milling, "
                "prospecting, and gathering use 0.15s cast time.");
        }

        return true;
    }
};

void AddSC_GmFastCraft()
{
    new GmFastCraft_PlayerScript();
    new GmFastCraft_CommandScript();
}
