#ifndef MOD_SPELLS_QOL_RIDING_TRAINER_ACCESS_H
#define MOD_SPELLS_QOL_RIDING_TRAINER_ACCESS_H

#include <cstdint>

class Creature;

namespace SpellsQoLRidingTrainer
{
    bool IsRidingTrainer(Creature const* creature);
    bool IsRidingTrainer(uint32 creatureEntry);
}

#endif
