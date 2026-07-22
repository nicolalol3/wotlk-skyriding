#include "ScriptMgr.h"
#include "Pet.h"
#include "Player.h"

class PetHappinessQoL : public AllCreatureScript
{
public:
    PetHappinessQoL() : AllCreatureScript("PetHappinessQoL") { }

    void OnAllCreatureUpdate(Creature* creature, uint32 /*diff*/) override
    {
        if (!creature->IsPet())
            return;

        Pet* pet = creature->ToPet();
        if (!pet || pet->getPetType() != HUNTER_PET)
            return;

        // Only set happiness if it dropped below a high threshold (e.g., 990,000)
        // This makes it extremely efficient as it only performs a simple integer comparison
        // and only calls SetPower when actually needed (once every few minutes of decay).
        if (pet->GetPower(POWER_HAPPINESS) < 990000)
        {
            pet->SetPower(POWER_HAPPINESS, 1000000);
        }
    }
};

void AddSC_PetHappinessQoL()
{
    new PetHappinessQoL();
}
