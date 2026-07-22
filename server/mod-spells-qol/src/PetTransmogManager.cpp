#include "PetTransmogManager.h"

#include "PetTransmogCore.h"

#include "mod-horizon-stuff/src/ServiceUIProximity.h"

#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "SharedDefines.h"
#include "WorldPacket.h"

namespace
{
    uint32 GetAccountId(Player* player)
    {
        return player ? player->GetSession()->GetAccountId() : 0;
    }
}

void SendPetTransmogUISignal(Player* player, Creature* creature)
{
    if (!player)
        return;

    RegisterServiceUIAnchor(player, creature, ServiceUIKind::PetTransmog);
    ChatHandler(player->GetSession()).PSendSysMessage("##PET_TRANSMOG_UI##");
}

uint32 PetTransmogManager::GetNativeDisplayId(uint32 creatureEntry)
{
    CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(creatureEntry);
    if (!cInfo || cInfo->Models.empty())
        return 0;

    return cInfo->Models[0].CreatureDisplayID;
}

uint32 PetTransmogManager::GetNativeDisplayId(Pet* pet)
{
    if (!pet)
        return 0;

    return GetNativeDisplayId(pet->GetEntry());
}

std::string PetTransmogManager::GetCreatureNameForEntry(Player* player, uint32 creatureEntry)
{
    CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(creatureEntry);
    if (!cInfo || !player)
        return "Unknown";

    LocaleConstant locale = player->GetSession()->GetSessionDbLocaleIndex();
    std::string name = cInfo->Name;
    if (CreatureLocale const* cl = sObjectMgr->GetCreatureLocale(creatureEntry))
        ObjectMgr::GetLocaleString(cl->Name, locale, name);

    return name;
}

Player* PetTransmogManager::ResolveHunterOwner(Pet* pet)
{
    if (!pet)
        return nullptr;

    if (Unit* owner = pet->GetOwner())
        if (Player* player = owner->ToPlayer())
            return player;

    ObjectGuid ownerGuid = pet->GetOwnerGUID();
    if (ownerGuid.IsPlayer())
        if (Player* player = ObjectAccessor::FindPlayer(ownerGuid))
            return player;

    ObjectGuid creatorGuid = pet->GetCreatorGUID();
    if (creatorGuid.IsPlayer())
        if (Player* player = ObjectAccessor::FindPlayer(creatorGuid))
            return player;

    return nullptr;
}

void PetTransmogManager::UnlockNativeAppearance(Player* player, Pet* pet)
{
    if (!player || !pet || !player->IsClass(CLASS_HUNTER, CLASS_CONTEXT_PET))
        return;

    uint32 displayId = pet->GetDisplayId();
    if (!displayId)
        displayId = GetNativeDisplayId(pet);
    if (!displayId)
        return;

    float targetScale = PetTransmogCore::GetPetVisualScale(pet);
    uint32 accountId = GetAccountId(player);
    uint32 creatureEntry = pet->GetEntry();

    if (IsAppearanceUnlocked(player, displayId))
        return;

    std::string appearanceName = GetCreatureNameForEntry(player, creatureEntry);

    CharacterDatabase.Execute(
        "INSERT INTO mod_pet_transmog_unlocked "
        "(account_id, display_id, target_scale, source_creature_entry) "
        "VALUES ({}, {}, {}, {})",
        accountId, displayId, targetScale, creatureEntry);

    ChatHandler(player->GetSession()).PSendSysMessage(
        "|cffffd200[Pet Transmog]|r New pet appearance unlocked: {}",
        appearanceName);
}

void PetTransmogManager::ApplyScaleOnSpawn(Pet* pet)
{
    if (!pet)
        return;

    Player* owner = ResolveHunterOwner(pet);
    if (!owner)
        return;

    uint32 displayId = pet->GetDisplayId();
    uint32 nativeDisplayId = GetNativeDisplayId(pet);

    float targetScale = PetTransmogCore::GetPetVisualScale(pet);

    if (displayId != nativeDisplayId)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT target_scale FROM mod_pet_transmog_unlocked "
            "WHERE account_id = {} AND display_id = {} LIMIT 1",
            GetAccountId(owner), displayId);

        if (result)
            targetScale = result->Fetch()[0].Get<float>();
    }

    pet->SetObjectScale(PetTransmogCore::GetPetScalingCompensation(pet, targetScale));
}

bool PetTransmogManager::IsAppearanceUnlocked(Player* player, uint32 displayId)
{
    if (!player || !displayId)
        return false;

    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM mod_pet_transmog_unlocked "
        "WHERE account_id = {} AND display_id = {} LIMIT 1",
        GetAccountId(player), displayId);

    return bool(result);
}

std::vector<PetTransmogUnlockEntry> PetTransmogManager::GetUnlockedAppearances(Player* player)
{
    std::vector<PetTransmogUnlockEntry> entries;
    if (!player)
        return entries;

    QueryResult result = CharacterDatabase.Query(
        "SELECT display_id, target_scale, source_creature_entry "
        "FROM mod_pet_transmog_unlocked WHERE account_id = {} "
        "ORDER BY display_id ASC",
        GetAccountId(player));

    if (!result)
        return entries;

    do
    {
        Field* fields = result->Fetch();
        PetTransmogUnlockEntry entry;
        entry.displayId = fields[0].Get<uint32>();
        entry.targetScale = fields[1].Get<float>();
        entry.creatureEntry = fields[2].Get<uint32>();
        entries.push_back(entry);
    } while (result->NextRow());

    return entries;
}

PetTransmogManager::ApplyResult PetTransmogManager::ApplyAppearance(Player* player, uint32 displayId)
{
    if (!player || !displayId)
        return ApplyResult::InvalidDisplay;

    Pet* pet = player->GetPet();
    if (!pet)
        return ApplyResult::NoPet;

    QueryResult result = CharacterDatabase.Query(
        "SELECT target_scale FROM mod_pet_transmog_unlocked "
        "WHERE account_id = {} AND display_id = {} LIMIT 1",
        GetAccountId(player), displayId);

    if (!result)
        return ApplyResult::NotUnlocked;

    if (!player->HasEnoughMoney(APPLY_COST_COPPER))
        return ApplyResult::NotEnoughMoney;

    float targetScale = result->Fetch()[0].Get<float>();
    player->ModifyMoney(-int32(APPLY_COST_COPPER));

    pet->SetDisplayId(displayId);
    pet->SetNativeDisplayId(displayId);
    pet->SetObjectScale(PetTransmogCore::GetPetScalingCompensation(pet, targetScale));
    pet->SavePetToDB(PET_SAVE_AS_CURRENT);

    return ApplyResult::Ok;
}

bool PetTransmogManager::RestoreOriginalAppearance(Player* player)
{
    if (!player)
        return false;

    Pet* pet = player->GetPet();
    if (!pet)
        return false;

    uint32 originalDisplayId = GetNativeDisplayId(pet);
    if (!originalDisplayId)
        return false;

    pet->SetDisplayId(originalDisplayId);
    pet->SetNativeDisplayId(originalDisplayId);

    float nativeVisualScale = PetTransmogCore::GetPetVisualScale(pet);
    pet->SetObjectScale(PetTransmogCore::GetPetScalingCompensation(pet, nativeVisualScale));
    pet->SavePetToDB(PET_SAVE_AS_CURRENT);

    return true;
}
