#include "PetAppearanceGossip.h"
#include "PetTransmogCore.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "Pet.h"
#include "Creature.h"
#include "DatabaseEnv.h"

class mod_pet_appearance_stablemaster : public AllCreatureScript
{
public:
    mod_pet_appearance_stablemaster() : AllCreatureScript("mod_pet_appearance_stablemaster") { }

    bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        if (!PetAppearanceGossip::IsHunter(player))
            return false;

        if (!creature->HasNpcFlag(UNIT_NPC_FLAG_STABLEMASTER))
            return false;

        PetAppearanceGossip::ShowStableMasterHello(player, creature);
        return true;
    }

    bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        return PetAppearanceGossip::HandleGossipSelect(player, creature, sender, action);
    }
};

class mod_pet_appearance_script : public PetScript
{
public:
    mod_pet_appearance_script() : PetScript("mod_pet_appearance_script") { }

    void OnPetAddToWorld(Pet* pet) override
    {
        if (!pet)
            return;

        QueryResult result = CharacterDatabase.Query("SELECT target_scale FROM mod_pet_appearances WHERE owner_guid = {} AND display_id = {} LIMIT 1",
            pet->GetOwnerGUID().GetCounter(), pet->GetDisplayId());

        float targetScale = 1.0f;
        if (result)
            targetScale = result->Fetch()[0].Get<float>();
        else
            targetScale = PetTransmogCore::GetPetVisualScale(pet);

        pet->SetObjectScale(PetTransmogCore::GetPetScalingCompensation(pet, targetScale));
    }
};

void AddSC_PetAppearanceQoL()
{
    new mod_pet_appearance_stablemaster();
    new mod_pet_appearance_script();
}
