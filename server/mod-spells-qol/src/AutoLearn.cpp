#include "ScriptMgr.h"
#include "Player.h"
#include "SpellMgr.h"
#include "ObjectMgr.h"
#include "SpellInfo.h"
#include "QuestDef.h"
#include "DBCEnums.h"
#include "Log.h"
#include "DBCStores.h"

class AutoLearnSpells : public PlayerScript
{
public:
    AutoLearnSpells() : PlayerScript("AutoLearnSpells") { }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!player)
            return;

        LOG_INFO("module", "AutoLearn: Player {} leveled up to {}. Running check...", player->GetName(), player->GetLevel());
        LOG_INFO("module", ">>> TEST: AUTO LEARN MODULE UPDATED BY ANTIGRAVITY <<<");
        HandleAutoLearn(player);
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!player)
            return;

        LOG_INFO("module", "AutoLearn: Player {} logged in. Running check...", player->GetName());
        HandleAutoLearn(player);
    }

    void OnPlayerLearnSpell(Player* player, uint32 spellId) override
    {
        if (!player)
            return;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->IsProfessionOrRiding())
            return;

        // When a player manually learns a spell (e.g. a base spell at Level 10+),
        // automatically learn all higher ranks available for their current level.
        LearnRanksForSpell(player, spellId);
    }

private:
    struct ClassQuestSpell
    {
        uint8 PlayerClass;
        uint8 RequiredLevel;
        uint32 SpellId;
        int8 Team;
    };

    struct ClassQuestItem
    {
        uint8 PlayerClass;
        uint8 RequiredLevel;
        uint32 ItemId;
    };

    static bool IsProfessionOrRidingSpell(uint32 spellId)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        return spellInfo && spellInfo->IsProfessionOrRiding();
    }

    static void SafeLearn(Player* player, uint32 spellId)
    {
        if (!player || !spellId)
            return;

        if (player->HasActiveSpell(spellId))
            return;

        PlayerSpellMap::const_iterator itr = player->GetSpellMap().find(spellId);
        if (itr != player->GetSpellMap().end() && itr->second->State != PLAYERSPELL_REMOVED)
            return;

        player->learnSpell(spellId, false);
    }

    static void SafeAddItem(Player* player, uint32 itemId)
    {
        if (!player || !itemId || player->HasItemCount(itemId, 1, true))
            return;

        player->AddItem(itemId, 1);
    }

    void HandleAutoLearn(Player* player)
    {
        // 1. Hunter special spells exception at level 5 (Run this FIRST)
        LearnHunterSpecialSpells(player);

        // 2. Early class spells at level 9
        LearnEarlyClassSpells(player);

        // 3. Explicit quest spell replacements that are not reliably covered
        // by trainer data or quest RewardSpell fields on custom databases.
        LearnExplicitClassQuestRewards(player);

        uint8 level = player->GetLevel();

        // Level 1-10: Learn everything available (new spells + ranks)
        // Level 10+: Learn ONLY ranks of already known spells
        LearnFromTrainer(player, level > 10);

        // Restored quest rewards checking
        CheckClassQuestRewards(player);
    }

    void LearnHunterSpecialSpells(Player* player)
    {
        if (player->getClass() != CLASS_HUNTER || player->GetLevel() < 5)
            return;

        // Mend Pet (136), Tame Beast (1515), Revive Pet (982), Feed Pet (6991), Dismiss Pet (2641), Call Pet (883), Call Stabled Pet (62757)
        static const std::vector<uint32> hunterSpells = { 1515, 982, 6991, 2641, 136, 883, 62757 };
        
        for (uint32 spellId : hunterSpells)
        {
            if (!player->HasSpell(spellId))
            {
                SafeLearn(player, spellId);
                if (!player->HasSpell(spellId))
                    continue;
                LOG_INFO("module", "AutoLearn: Hunter {} learned special spell {}", player->GetName(), spellId);
                
                // For spells with ranks (like Mend Pet), learn all available ranks for level 5+
                LearnRanksForSpell(player, spellId);
            }
        }
    }

    void LearnEarlyClassSpells(Player* player)
    {
        if (player->GetLevel() < 9)
            return;

        uint8 playerClass = player->getClass();
        std::vector<uint32> spells;

        if (playerClass == CLASS_DRUID)
        {
            // Thorns (782), Swipe (779)
            spells = { 782, 779 };
        }
        else if (playerClass == CLASS_PALADIN)
        {
            // Consecration (26573), Righteous Fury (25780)
            spells = { 26573, 25780 };
        }

        if (spells.empty())
            return;

        for (uint32 spellId : spells)
        {
            if (!player->HasSpell(spellId))
            {
                SafeLearn(player, spellId);
                if (!player->HasSpell(spellId))
                    continue;
                LOG_INFO("module", "AutoLearn: {} learned early spell {}", player->GetName(), spellId);
                
                // Learn available ranks for current level
                LearnRanksForSpell(player, spellId);
            }
        }
    }

    void LearnExplicitClassQuestRewards(Player* player)
    {
        uint8 playerClass = player->getClass();
        uint8 level = player->GetLevel();
        int8 team = player->GetTeamId() == TEAM_ALLIANCE ? 0 : 1;

        static const std::vector<ClassQuestSpell> questSpells =
        {
            { CLASS_DRUID,        10,  5487, -1 },
            { CLASS_DRUID,        10,  8946, -1 },
            { CLASS_DRUID,        10,  1066, -1 },
            { CLASS_DRUID,        50, 40120, -1 },

            { CLASS_WARLOCK,      10,   697, -1 },
            { CLASS_WARLOCK,      10,   712, -1 },
            { CLASS_WARLOCK,      10,   691, -1 },
            { CLASS_WARLOCK,      30,  1122, -1 },
            { CLASS_WARLOCK,      30, 18540, -1 },

            { CLASS_MAGE,         10, 53140, -1 },
            { CLASS_MAGE,         10, 33690,  0 },
            { CLASS_MAGE,         10, 35715,  1 },
            { CLASS_MAGE,         10,  3561,  0 },
            { CLASS_MAGE,         10,  3567,  1 },
            { CLASS_MAGE,         50, 28272, -1 },

            { CLASS_DEATH_KNIGHT,  4, 53428, -1 },
            { CLASS_DEATH_KNIGHT,  4, 50977, -1 },

            { CLASS_SHAMAN,       10,  3599, -1 },
            { CLASS_SHAMAN,        3,  8071, -1 },
            { CLASS_SHAMAN,       20,  5394, -1 },

            { CLASS_PALADIN,      10,  7328, -1 }
        };

        static const std::vector<ClassQuestItem> questItems =
        {
            { CLASS_SHAMAN, 10, 5176 },
            { CLASS_SHAMAN,  3, 5175 },
            { CLASS_SHAMAN, 20, 5177 },
            { CLASS_SHAMAN, 20, 5178 }
        };

        for (ClassQuestSpell const& entry : questSpells)
        {
            if (entry.PlayerClass != playerClass || level < entry.RequiredLevel)
                continue;

            if (entry.Team != -1 && entry.Team != team)
                continue;

            if (!player->HasSpell(entry.SpellId))
            {
                SafeLearn(player, entry.SpellId);
                LOG_INFO("module", "AutoLearn: Player {} learned explicit quest spell {}", player->GetName(), entry.SpellId);
            }
        }

        for (ClassQuestItem const& entry : questItems)
        {
            if (entry.PlayerClass != playerClass || level < entry.RequiredLevel)
                continue;

            SafeAddItem(player, entry.ItemId);
        }
    }


    void LearnFromTrainer(Player* player, bool ranksOnly)
    {
        uint32 playerLevel = player->GetLevel();

        // Iterate through all spells to find those relevant to the player's class and level
        for (uint32 i = 0; i < sSpellMgr->GetSpellInfoStoreSize(); ++i)
        {
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(i);
            if (!spellInfo)
                continue;

            // Only consider spells that have a level requirement met by the player
            if (spellInfo->SpellLevel == 0 || spellInfo->SpellLevel > playerLevel)
                continue;

            // Filter by class and SPECIFIC class tabs (exclude General tab)
            if (!IsClassSpell(player, spellInfo))
                continue;

            // NEW: Check blacklist
            if (IsBlacklistedSpell(spellInfo->Id))
                continue;

            // Exclude professions, riding, and other non-combat skills
            if (IsProfessionOrRidingSkill(spellInfo->Id))
                continue;

            if (ranksOnly)
            {
                // Level 10+: Only learn if we already know a rank of this spell (Rank chain)
                uint32 firstRankId = sSpellMgr->GetFirstSpellInChain(spellInfo->Id);
                if (!player->HasSpell(firstRankId))
                    continue;
            }

            // Learn it if not already known
            if (!player->HasSpell(spellInfo->Id))
            {
                // EXCLUDE RIDING SKILLS (Skill 762) from auto-learning
                bool isRiding = false;
                SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellInfo->Id);
                for (auto itr = bounds.first; itr != bounds.second; ++itr)
                {
                    if (itr->second->SkillLine == 762)
                    {
                        isRiding = true;
                        break;
                    }
                }

                if (isRiding)
                    continue;

                SafeLearn(player, spellInfo->Id);
            }
        }
    }

    bool IsSkillLineForClass(uint8 playerClass, uint32 skillLineId)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:      return (skillLineId == 26 || skillLineId == 256 || skillLineId == 257 || skillLineId == 161);
            case CLASS_PALADIN:      return (skillLineId == 184 || skillLineId == 267 || skillLineId == 597);
            case CLASS_HUNTER:       return (skillLineId == 50 || skillLineId == 163 || skillLineId == 51);
            case CLASS_ROGUE:        return (skillLineId == 38 || skillLineId == 39 || skillLineId == 253);
            case CLASS_PRIEST:       return (skillLineId == 613 || skillLineId == 56 || skillLineId == 78);
            case CLASS_DEATH_KNIGHT: return (skillLineId == 770 || skillLineId == 771 || skillLineId == 772 || skillLineId == 784);
            case CLASS_SHAMAN:       return (skillLineId == 375 || skillLineId == 373 || skillLineId == 374);
            case CLASS_MAGE:         return (skillLineId == 237 || skillLineId == 8 || skillLineId == 6);
            case CLASS_WARLOCK:      return (skillLineId == 355 || skillLineId == 354 || skillLineId == 593);
            case CLASS_DRUID:        return (skillLineId == 574 || skillLineId == 134 || skillLineId == 573);
            default: return false;
        }
    }

    bool IsClassSpell(Player* player, SpellInfo const* spellInfo)
    {
        // 1. Check SkillLineAbility (This determines the tab in the spellbook)
        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellInfo->Id);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            SkillLineAbilityEntry const* ability = itr->second;
            if (ability->ClassMask & player->getClassMask())
            {
                // Strict check: Only allow if the SkillLine is one of the specific class skill lines
                if (IsSkillLineForClass(player->getClass(), ability->SkillLine))
                    return true;
            }
        }

        return false;
    }

    uint32 GetSpellFamilyForClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_MAGE:         return SPELLFAMILY_MAGE;
            case CLASS_WARRIOR:      return SPELLFAMILY_WARRIOR;
            case CLASS_WARLOCK:      return SPELLFAMILY_WARLOCK;
            case CLASS_PRIEST:       return SPELLFAMILY_PRIEST;
            case CLASS_DRUID:        return SPELLFAMILY_DRUID;
            case CLASS_ROGUE:        return SPELLFAMILY_ROGUE;
            case CLASS_HUNTER:       return SPELLFAMILY_HUNTER;
            case CLASS_PALADIN:      return SPELLFAMILY_PALADIN;
            case CLASS_SHAMAN:       return SPELLFAMILY_SHAMAN;
            case CLASS_DEATH_KNIGHT: return SPELLFAMILY_DEATHKNIGHT;
            default:                 return SPELLFAMILY_GENERIC;
        }
    }

    void LearnRanksForSpell(Player* player, uint32 spellId)
    {
        if (IsProfessionOrRidingSpell(spellId))
            return;

        uint32 nextRankId = sSpellMgr->GetNextSpellInChain(spellId);
        while (nextRankId != 0)
        {
            if (IsProfessionOrRidingSpell(nextRankId))
                break;

            SpellInfo const* nextRankInfo = sSpellMgr->GetSpellInfo(nextRankId);
            if (!nextRankInfo || nextRankInfo->SpellLevel > player->GetLevel())
                break;

            if (!player->HasSpell(nextRankId) && !IsBlacklistedSpell(nextRankId))
            {
                // EXCLUDE RIDING SKILLS (Skill 762)
                bool isRiding = false;
                SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(nextRankId);
                for (auto itr = bounds.first; itr != bounds.second; ++itr)
                {
                    if (itr->second->SkillLine == 762)
                    {
                        isRiding = true;
                        break;
                    }
                }

                if (!isRiding)
                {
                    player->learnSpell(nextRankId, false);
                }
            }

            nextRankId = sSpellMgr->GetNextSpellInChain(nextRankId);
        }
    }

    bool IsBlacklistedSpell(uint32 spellId)
    {
        static std::set<uint32> normalizedBlacklist;
        static std::set<uint32> exactBlacklist;
        static bool initialized = false;

        if (!initialized)
        {
            // Standard blacklist: any rank here blocks the entire chain
            static const std::set<uint32> initialBlacklist = {
                // Hunter
                13481,
                // Warrior
                29842, 12723, 46857, 20647, 29801, 46947, 12355,
                // Paladin
                25997,
                // Rogue
                31125, 36554,
                // Priest
                15286, 34919,
                // Shaman
                51470, 51466,
                // Mage
                31579, 31582, 31583, 31643, 44450, 29077, 12484, 43339,
                // Warlock
                31117, 34936, 17962, 26654, 34754,
                // Druid
                1178
            };

            // Special cases: only these specific IDs are blocked (Life Tap Ranks 2+)
            static const std::set<uint32> lifeTapRanks = { 1455, 1456, 11687, 11688, 11689, 27222, 57946 };

            for (uint32 id : initialBlacklist)
            {
                uint32 firstRankId = sSpellMgr->GetFirstSpellInChain(id);
                normalizedBlacklist.insert(firstRankId != 0 ? firstRankId : id);
            }

            for (uint32 id : lifeTapRanks)
            {
                exactBlacklist.insert(id);
            }

            initialized = true;
        }

        // 1. Check exact blacklist (for Life Tap higher ranks)
        if (exactBlacklist.count(spellId))
            return true;

        // 2. Check normalized blacklist (blocks entire chains)
        uint32 currentFirstRank = sSpellMgr->GetFirstSpellInChain(spellId);
        return normalizedBlacklist.count(currentFirstRank != 0 ? currentFirstRank : spellId);
    }

    void CheckClassQuestRewards(Player* player)
    {
        uint32 classMask = player->getClassMask();
        uint32 playerLevel = player->GetLevel();

        for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
        {
            if (quest->GetRequiredClasses() & classMask)
            {
                if (playerLevel >= quest->GetMinLevel())
                {
                    int32 spellId = quest->GetRewSpellCast();
                    if (spellId > 0)
                    {
                        uint32 uSpellId = (uint32)spellId;
                        if (!player->HasSpell(uSpellId) && !IsBlacklistedSpell(uSpellId))
                        {
                            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(uSpellId);
                            // Quest spells are also filtered by class skill lines to avoid junk
                            if (spellInfo && IsClassSpell(player, spellInfo))
                            {
                                player->learnSpell(uSpellId, false);
                                LOG_INFO("module", "AutoLearn: Player {} learned quest spell {} (Quest {})", player->GetName(), uSpellId, questId);
                            }
                        }
                    }
                }
            }
        }
    }
};

void AddSC_mod_auto_learn_spells()
{
    new AutoLearnSpells();
}
