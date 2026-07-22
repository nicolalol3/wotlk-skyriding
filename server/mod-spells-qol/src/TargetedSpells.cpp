/*
 * --- SISTEMA SPELL TARGETED (DUMMY TRIGGER SYSTEM) ---
 * Documentazione aggiornata il 20/04/2026
 * Finalizzato con Force of Nature, Mass Dispel e Shadowfury.
 * Nomi spell aggiornati in format: "- Targeted"
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "Opcodes.h"

// Mappa: Spell Dummy -> Spell Originale (47 Mappings)
static std::map<uint32, uint32> dummyToOriginalMap = {
    // Rain of Fire
    {98000, 5740}, {98001, 6219}, {98002, 11677}, {98003, 11678}, {98004, 27212}, {98005, 47819}, {98006, 47820},
    // Inferno
    {98007, 1122},
    // Volley
    {98008, 1510}, {98009, 14294}, {98010, 14295}, {98011, 27022}, {98012, 58431}, {98013, 58434},
    // Freezing Arrow
    {98014, 60192},
    // Flamestrike
    {98015, 2120}, {98016, 2121}, {98017, 8422}, {98018, 8423}, {98019, 10215}, {98020, 10216}, {98021, 27086}, {98022, 42925}, {98023, 42926},
    // Blizzard
    {98024, 6141}, {98025, 8427}, {98026, 10185}, {98027, 10186}, {98028, 10187}, {98029, 27085}, {98030, 42939}, {98031, 42940},
    // Death and Decay
    {98032, 43265}, {98033, 49936}, {98034, 49937}, {98035, 49938},
    // Hurricane
    {98036, 16914}, {98037, 17401}, {98038, 17402}, {98039, 27012}, {98040, 48467},
    // Force of Nature
    {98041, 33831},
    // Mass Dispel
    {98042, 32375},
    // Shadowfury (Ranks 1-4)
    {98043, 30283}, {98044, 30413}, {98045, 47846}, {98046, 47847}
};

class TargetedSpellsPlayerScript : public PlayerScript
{
public:
    TargetedSpellsPlayerScript() : PlayerScript("TargetedSpellsPlayerScript") { }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!player || !spell)
            return;

        uint32 dummyId = spell->GetSpellInfo()->Id;
        auto it = dummyToOriginalMap.find(dummyId);
        if (it == dummyToOriginalMap.end())
            return;

        uint32 originalId = it->second;
        SpellInfo const* originalInfo = sSpellMgr->GetSpellInfo(originalId);
        if (!originalInfo)
            return;

        Unit* target = spell->m_targets.GetUnitTarget();
        if (!target)
            return;

        // 1. COSTO MANA
        int32 powerCost = originalInfo->CalcPowerCost(player, originalInfo->GetSchoolMask());
        if (powerCost > 0)
        {
            if (player->GetPower(POWER_MANA) < (uint32)powerCost)
                return;
            player->ModifyPower(POWER_MANA, -powerCost);
        }

        // 2. GCD
        uint32 gcd = originalInfo->StartRecoveryTime;
        if (!gcd) gcd = 1500; 

        player->GetGlobalCooldownMgr().AddGlobalCooldown(originalInfo, gcd);
        
        WorldPacket data1(SMSG_SPELL_COOLDOWN, 8 + 1 + 4 + 4);
        data1 << player->GetGUID() << uint8(0) << uint32(dummyId) << uint32(gcd);
        player->SendDirectMessage(&data1);

        WorldPacket data2(SMSG_SPELL_COOLDOWN, 8 + 1 + 4 + 4);
        data2 << player->GetGUID() << uint8(0) << uint32(originalId) << uint32(gcd);
        player->SendDirectMessage(&data2);

        // 3. CAST REALE
        player->CastSpell(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), originalId, true);
    }

    void OnPlayerLearnSpell(Player* player, uint32 spellId) override
    {
        for (auto const& [dummyId, originalId] : dummyToOriginalMap)
        {
            if (spellId == originalId && !player->HasSpell(dummyId))
            {
                player->learnSpell(dummyId, false);
                break;
            }
        }
    }

    void OnPlayerLogin(Player* player) override
    {
        // Cleanup vecchi IDs test
        for (uint32 id = 80874; id <= 80880; ++id) if (player->HasSpell(id)) player->removeSpell(id, false, false);
        for (uint32 id = 90000; id <= 90006; ++id) if (player->HasSpell(id)) player->removeSpell(id, false, false);
        for (uint32 id = 95000; id <= 95006; ++id) if (player->HasSpell(id)) player->removeSpell(id, false, false);

        // Impara tutti i rank dummy relativi agli originali conosciuti
        for (auto const& [dummyId, originalId] : dummyToOriginalMap)
        {
            if (player->HasSpell(originalId) && !player->HasSpell(dummyId))
                player->learnSpell(dummyId, false);
        }
    }
};

void AddSC_TargetedSpells()
{
    new TargetedSpellsPlayerScript();
}
