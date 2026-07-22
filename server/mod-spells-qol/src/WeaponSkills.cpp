#include "ScriptMgr.h"
#include "Player.h"
#include "World.h"
#include "SpellMgr.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"

class WeaponSkillsQoL : public PlayerScript
{
public:
    WeaponSkillsQoL() : PlayerScript("WeaponSkillsQoL",
        {
            PLAYERHOOK_ON_LOAD_FROM_DB,
            PLAYERHOOK_ON_LOGIN,
        }) { }

    // Runs before _LoadSpells — remove stale invalid rows so the core does not log errors.
    void OnPlayerLoadFromDB(Player* player) override
    {
        if (!player)
            return;

        for (uint32 spellId : kWeaponSkillSpells)
        {
            if (IsWeaponSkillValidForPlayer(player, spellId))
                continue;

            CharacterDatabase.Execute(
                "DELETE FROM character_spell WHERE guid = {} AND spell = {}",
                player->GetGUID().GetCounter(), spellId);
        }
    }

    void OnPlayerLogin(Player* player) override
    {
        LearnAllClassWeaponSkills(player);
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        player->UpdateSkillsToMaxSkillsForLevel();
    }

private:
    static constexpr uint32 kWeaponSkillSpells[] =
    {
        1180,  // Daggers
        196,   // One-Handed Axes
        197,   // Two-Handed Axes
        198,   // One-Handed Maces
        199,   // Two-Handed Maces
        200,   // Polearms
        201,   // One-Handed Swords
        202,   // Two-Handed Swords
        227,   // Staves
        15590, // Fist Weapons
        264,   // Bows
        266,   // Guns
        5011,  // Crossbows
        2567,  // Thrown
        5009,  // Wands
    };

    // Same rules as Player::CheckSkillLearnedBySpell, without logging (that API logs even
    // when the player does not know the spell yet).
    static bool IsWeaponSkillValidForPlayer(Player* player, uint32 spellId)
    {
        if (!player || !spellId)
            return false;

        if (!sWorld->getBoolConfig(CONFIG_VALIDATE_SKILL_LEARNED_BY_SPELLS))
            return true;

        SkillLineAbilityMapBounds skillBounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        if (skillBounds.first == skillBounds.second)
            return true;

        for (SkillLineAbilityMap::const_iterator sla = skillBounds.first; sla != skillBounds.second; ++sla)
        {
            SkillLineEntry const* skillLine = sSkillLineStore.LookupEntry(sla->second->SkillLine);
            if (!skillLine)
                continue;

            if (GetSkillRaceClassInfo(skillLine->id, player->getRace(), player->getClass()))
                return true;
        }

        return false;
    }

    static void SafeLearnWeaponSkill(Player* player, uint32 spellId)
    {
        if (!player || !spellId)
            return;

        if (!IsWeaponSkillValidForPlayer(player, spellId))
            return;

        if (player->HasActiveSpell(spellId))
            return;

        PlayerSpellMap::const_iterator itr = player->GetSpellMap().find(spellId);
        if (itr != player->GetSpellMap().end() && itr->second->State != PLAYERSPELL_REMOVED)
            return;

        player->learnSpell(spellId, false);
    }

    static void StripInvalidWeaponSkill(Player* player, uint32 spellId)
    {
        if (!player || !spellId || IsWeaponSkillValidForPlayer(player, spellId))
            return;

        player->removeSpell(spellId, SPEC_MASK_ALL, false);
    }

    void LearnAllClassWeaponSkills(Player* player)
    {
        for (uint32 spellId : kWeaponSkillSpells)
        {
            StripInvalidWeaponSkill(player, spellId);
            SafeLearnWeaponSkill(player, spellId);
        }

        player->UpdateSkillsToMaxSkillsForLevel();
    }
};

void AddSC_WeaponSkillsQoL()
{
    new WeaponSkillsQoL();
}
