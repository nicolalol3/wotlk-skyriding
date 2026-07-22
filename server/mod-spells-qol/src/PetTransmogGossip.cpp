#include "PetTransmogManager.h"

#include "mod-horizon-stuff/src/ServiceGossipLabels.h"
#include "Player.h"
#include "ScriptedGossip.h"

namespace PetTransmogGossip
{
    bool HandleGossipSelect(Player* player, Creature* creature,
        uint32 /*sender*/, uint32 action)
    {
        if (action != ServiceGossip::ACTION_PET_TRANSMOG_UI_OPEN)
            return false;

        SendPetTransmogUISignal(player, creature);
        CloseGossipMenuFor(player);
        return true;
    }
}
