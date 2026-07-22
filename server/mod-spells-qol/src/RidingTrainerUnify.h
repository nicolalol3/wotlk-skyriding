/*
 * mod-spells-qol - unified riding trainer spell lists
 */

#ifndef SPELLS_QOL_RIDING_TRAINER_UNIFY_H
#define SPELLS_QOL_RIDING_TRAINER_UNIFY_H

#include "Common.h"

class Creature;
class Player;
class WorldPacket;

namespace SpellsQoLRidingTrainerUnify
{
    void Load();
    bool IsEnabled();
    void RewriteTrainerListPacket(Player* player, WorldPacket& packet);
    bool TryHandleTrainerBuy(Player* player, Creature* creature, int32 spellId);
}

#endif
