#ifndef PET_TRANSMOG_MANAGER_H
#define PET_TRANSMOG_MANAGER_H

#include "Define.h"

#include <string>
#include <vector>

class Pet;
class Player;

struct PetTransmogUnlockEntry
{
    uint32 displayId = 0;
    uint32 creatureEntry = 0;
    float targetScale = 1.0f;
};

namespace PetTransmogManager
{
    constexpr uint32 APPLY_COST_COPPER = 100000; // 10 gold

    void UnlockNativeAppearance(Player* player, Pet* pet);
    void ApplyScaleOnSpawn(Pet* pet);

    bool IsAppearanceUnlocked(Player* player, uint32 displayId);
    std::vector<PetTransmogUnlockEntry> GetUnlockedAppearances(Player* player);

    uint32 GetNativeDisplayId(Pet* pet);
    uint32 GetNativeDisplayId(uint32 creatureEntry);

    enum class ApplyResult : uint8
    {
        Ok,
        NoPet,
        NotUnlocked,
        NotEnoughMoney,
        InvalidDisplay
    };

    ApplyResult ApplyAppearance(Player* player, uint32 displayId);
    bool RestoreOriginalAppearance(Player* player);

    Player* ResolveHunterOwner(Pet* pet);

    std::string GetCreatureNameForEntry(Player* player, uint32 creatureEntry);
}

class Creature;

void SendPetTransmogUISignal(Player* player, Creature* creature);

#endif
