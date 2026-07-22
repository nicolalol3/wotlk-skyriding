#include "PetAppearanceGossip.h"

#include "mod-horizon-stuff/src/ServiceGossipLabels.h"
#include "PetTransmogCore.h"
#include "PetTransmogGossip.h"
#include "Creature.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"

float PetAppearanceGossip::GetPetVisualScale(Pet* pet)
{
    return PetTransmogCore::GetPetVisualScale(pet);
}

float PetAppearanceGossip::GetPetScalingCompensation(Pet* pet, float targetVisualScale)
{
    return PetTransmogCore::GetPetScalingCompensation(pet, targetVisualScale);
}

bool PetAppearanceGossip::IsHunter(Player* player)
{
    return player && player->getClass() == CLASS_HUNTER;
}

void PetAppearanceGossip::AddHubItem(Player* player, uint32 sender)
{
    if (!IsHunter(player))
        return;

    AddGossipItemFor(player, GOSSIP_ICON_CHAT, ServiceGossip::MenuPetTransmogrification(),
        sender, ServiceGossip::ACTION_PET_TRANSMOG_UI_OPEN);
}

void PetAppearanceGossip::ShowStableMasterHello(Player* player, Creature* creature)
{
    player->PrepareGossipMenu(creature, creature->GetCreatureTemplate()->GossipMenuId, true);
    AddHubItem(player, GOSSIP_SENDER);
    SendGossipMenuFor(player, player->GetGossipTextId(creature), creature->GetGUID());
}

bool PetAppearanceGossip::HandleGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action)
{
    return PetTransmogGossip::HandleGossipSelect(player, creature, sender, action);
}
