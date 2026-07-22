#ifndef PET_TRANSMOG_GOSSIP_H
#define PET_TRANSMOG_GOSSIP_H

#include "Define.h"

class Creature;
class Player;

namespace PetTransmogGossip
{
    bool HandleGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action);
}

#endif
