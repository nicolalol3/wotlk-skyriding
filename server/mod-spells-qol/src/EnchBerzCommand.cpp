/*
 * GM: .enchberz — apply Berserking weapon enchant to the equipped main-hand
 * (or off-hand) weapon, bypassing item/player level checks.
 */

#include "Chat.h"
#include "Item.h"
#include "Player.h"
#include "ScriptMgr.h"

using namespace Acore::ChatCommands;

namespace
{
    // SpellItemEnchantment: Enchant Weapon - Berserking (WotLK 3.3.5)
    constexpr uint32 BERSERKING_ENCHANT_ID = 3789;
}

class EnchBerzCommand : public CommandScript
{
public:
    EnchBerzCommand() : CommandScript("EnchBerzCommand") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "enchberz", HandleEnchBerz, SEC_GAMEMASTER, Console::No }
        };
        return commandTable;
    }

    static Item* GetEquippedWeapon(Player* player)
    {
        if (Item* main = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
            if (main->GetTemplate() && main->GetTemplate()->Class == ITEM_CLASS_WEAPON)
                return main;

        if (Item* off = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
            if (off->GetTemplate() && off->GetTemplate()->Class == ITEM_CLASS_WEAPON)
                return off;

        return nullptr;
    }

    static bool HandleEnchBerz(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        Item* weapon = GetEquippedWeapon(player);
        if (!weapon)
        {
            handler->PSendSysMessage("enchberz: no weapon equipped in main-hand/off-hand.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        // Direct enchant write — does not cast the scroll spell, so level/skill gates are skipped.
        player->ApplyEnchantment(weapon, PERM_ENCHANTMENT_SLOT, false);
        weapon->SetEnchantment(PERM_ENCHANTMENT_SLOT, BERSERKING_ENCHANT_ID, 0, 0, player->GetGUID());
        player->ApplyEnchantment(weapon, PERM_ENCHANTMENT_SLOT, true);

        handler->PSendSysMessage("enchberz: applied Berserking (%u) to [%s].",
            BERSERKING_ENCHANT_ID, weapon->GetTemplate()->Name1.c_str());
        return true;
    }
};

void AddSC_EnchBerzCommand()
{
    new EnchBerzCommand();
}
