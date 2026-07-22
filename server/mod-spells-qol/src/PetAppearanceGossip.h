#ifndef PET_APPEARANCE_GOSSIP_H
#define PET_APPEARANCE_GOSSIP_H

#include "Define.h"

class Creature;
class Pet;
class Player;

namespace PetAppearanceGossip
{
    constexpr uint32 GOSSIP_SENDER = 1000;

    float GetPetVisualScale(Pet* pet);
    float GetPetScalingCompensation(Pet* pet, float targetVisualScale = 1.0f);

    bool IsHunter(Player* player);
    void AddHubItem(Player* player, uint32 sender);
    void ShowStableMasterHello(Player* player, Creature* creature);
    bool HandleGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action);
}

#endif
