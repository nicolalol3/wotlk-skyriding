#ifndef PET_TRANSMOG_CORE_H
#define PET_TRANSMOG_CORE_H

#include "Define.h"

class Pet;

namespace PetTransmogCore
{
    float GetPetVisualScale(Pet* pet);
    float GetPetScalingCompensation(Pet* pet, float targetVisualScale = 1.0f);
}

#endif
