
#include "ScriptMgr.h"
#include "Player.h"
#include "Unit.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "Pet.h"
#include <unordered_map>

/**
 * @brief Exploit Demon (Warlock Misdirection) Logic
 * 
 * This module handles:
 * 1. Targeting restrictions (Pet/Guardian only).
 * 2. Visual Aura Swapping (35079 -> 98048) for Warlocks.
 * 3. 100% Threat Redirection for the Warlock aura 98048.
 * 4. Persistence across mounts for 35079 and 98048.
 */

static std::unordered_map<ObjectGuid, std::vector<uint32>> pendingAuras;
static std::unordered_map<ObjectGuid, bool> playerWasMounted;

class MisdirectionPlusUnitScript : public UnitScript
{
public:
    MisdirectionPlusUnitScript() : UnitScript("MisdirectionPlusUnitScript") { }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        uint32 spellId = aura->GetSpellInfo()->Id;

        // Visual Swap: 35079 (Hunter Proc) -> 98048 (Warlock Purple Proc)
        if (spellId == 35079 && unit->ToPlayer() && unit->ToPlayer()->getClass() == CLASS_WARLOCK)
        {
            unit->AddAura(98048, unit);
            aura->Remove();
            return;
        }

        // Handle permanency and threat redirection for the custom Warlock Proc
        if (spellId == 98048)
        {
            aura->SetDuration(-1);
            aura->SetMaxDuration(-1);

            if (Player* player = unit->ToPlayer())
            {
                if (Unit* pet = player->GetPet())
                {
                    player->SetRedirectThreat(pet->GetGUID(), 100);
                }
            }

            if (Unit* caster = aura->GetCaster())
            {
                if (caster->HasAura(98047))
                    caster->RemoveAurasDueToSpell(98047);
            }
        }
        else if (spellId == 35079) // Hunter misdirection proc
        {
            aura->SetDuration(-1);
            aura->SetMaxDuration(-1);
        }
    }

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode /*mode*/) override
    {
        if (aurApp->GetBase()->GetSpellInfo()->Id == 98048)
        {
            if (Player* player = unit->ToPlayer())
            {
                player->ResetRedirectThreat();
            }
        }
    }

    void OnUnitUpdate(Unit* unit, uint32 /*diff*/) override
    {
        if (unit->HasAura(98048) || unit->HasAura(35079))
        {
            if (Player* player = unit->ToPlayer())
            {
                Unit* pet = player->GetPet();
                ObjectGuid petGuid = pet ? pet->GetGUID() : ObjectGuid::Empty;
                Unit* currentTarget = player->GetRedirectThreatTarget();
                ObjectGuid currentTargetGuid = currentTarget ? currentTarget->GetGUID() : ObjectGuid::Empty;

                if (currentTargetGuid != petGuid)
                {
                    player->SetRedirectThreat(petGuid, 100);
                }
            }
        }
    }
};

class MisdirectionPlusPlayerScript : public PlayerScript
{
public:
    MisdirectionPlusPlayerScript() : PlayerScript("MisdirectionPlusPlayerScript") { }

    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        ObjectGuid guid = player->GetGUID();
        bool isMounted = player->IsMounted();

        if (!isMounted)
        {
            // If we just dismounted, reapply pending auras
            if (playerWasMounted[guid])
            {
                if (pendingAuras.count(guid))
                {
                    for (uint32 spellId : pendingAuras[guid])
                    {
                        if (!player->HasAura(spellId))
                        {
                            player->AddAura(spellId, player);
                        }
                    }
                    pendingAuras.erase(guid);
                }
            }

            // Continuously save state when NOT mounted
            std::vector<uint32> current;
            if (player->HasAura(35079)) current.push_back(35079);
            if (player->HasAura(98048)) current.push_back(98048);
            
            if (!current.empty())
                pendingAuras[guid] = current;
            else
                pendingAuras.erase(guid);
        }

        playerWasMounted[guid] = isMounted;
    }

    void OnPlayerLogout(Player* player) override
    {
        ObjectGuid guid = player->GetGUID();
        pendingAuras.erase(guid);
        playerWasMounted.erase(guid);
    }
};

class MisdirectionPlusSpellSC : public SpellSC
{
public:
    MisdirectionPlusSpellSC() : SpellSC("MisdirectionPlusSpellSC") { }

    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& res) override
    {
        uint32 spellId = spell->GetSpellInfo()->Id;
        if (spellId == 34477 || spellId == 98047)
        {
            Unit* target = spell->m_targets.GetUnitTarget();
            if (target && !target->IsPet() && !target->IsGuardian())
                res = SPELL_FAILED_BAD_TARGETS;
        }
    }
};

void AddSC_MisdirectionPlus()
{
    new MisdirectionPlusUnitScript();
    new MisdirectionPlusSpellSC();
    new MisdirectionPlusPlayerScript();
}
