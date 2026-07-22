#include "PetTransmogManager.h"
#include "PetTransmogCore.h"

#include "AllSpellScript.h"
#include "Chat.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Tokenize.h"
#include "WorldPacket.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace
{
    constexpr uint32 PTMOG_LIST_PAGE_SIZE = 9;
    constexpr uint32 PTMOG_PREVIEW_ENTRY_BASE = 0x01000000;

    uint32 GetPreviewCreatureEntry(uint32 displayId)
    {
        return PTMOG_PREVIEW_ENTRY_BASE + displayId;
    }

    void SendPreviewCreatureQuery(Player* player,
        PetTransmogUnlockEntry const& entry)
    {
        if (!player || !entry.displayId || !entry.creatureEntry)
            return;

        CreatureTemplate const* creatureInfo =
            sObjectMgr->GetCreatureTemplate(entry.creatureEntry);
        if (!creatureInfo)
            return;

        std::string name = PetTransmogManager::GetCreatureNameForEntry(
            player, entry.creatureEntry);
        uint32 previewEntry = GetPreviewCreatureEntry(entry.displayId);

        WorldPacket data(SMSG_CREATURE_QUERY_RESPONSE, 100);
        data << previewEntry;
        data << name;
        data << uint8(0) << uint8(0) << uint8(0);
        data << creatureInfo->SubName;
        data << creatureInfo->IconName;
        data << uint32(creatureInfo->type_flags);
        data << uint32(creatureInfo->type);
        data << uint32(creatureInfo->family);
        data << uint32(creatureInfo->rank);
        data << uint32(creatureInfo->KillCredit[0]);
        data << uint32(creatureInfo->KillCredit[1]);
        data << entry.displayId;
        data << uint32(0) << uint32(0) << uint32(0);
        data << float(creatureInfo->ModHealth);
        data << float(creatureInfo->ModMana);
        data << uint8(creatureInfo->RacialLeader);
        for (uint8 i = 0; i < MAX_CREATURE_QUEST_ITEMS; ++i)
            data << uint32(0);
        data << uint32(creatureInfo->movementId);
        player->SendDirectMessage(&data);
    }

    void SendPtRes(Player* player, std::string const& body)
    {
        if (!player)
            return;

        std::string response = "MIS_RES\t" + body;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, response);
        player->SendDirectMessage(&data);
    }

    bool ItemMatchesSearch(std::string const& name, std::string const& search)
    {
        if (search.empty())
            return true;

        std::string lowered = name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
        return lowered.find(search) != std::string::npos;
    }

    void SendState(Player* player)
    {
        if (!player || !player->IsClass(CLASS_HUNTER, CLASS_CONTEXT_PET))
        {
            SendPtRes(player, "PTMOG:STATE|0");
            return;
        }

        std::ostringstream ss;
        ss << "PTMOG:STATE|1";
        ss << "|COST:" << PetTransmogManager::APPLY_COST_COPPER;

        auto unlocked = PetTransmogManager::GetUnlockedAppearances(player);
        ss << "|COUNT:" << unlocked.size();

        Pet* pet = player->GetPet();
        if (pet)
        {
            uint32 displayId = pet->GetDisplayId();
            uint32 nativeDisplay = PetTransmogManager::GetNativeDisplayId(pet);
            uint32 creatureEntry = pet->GetEntry();
            float targetScale = PetTransmogCore::GetPetVisualScale(pet);

            for (PetTransmogUnlockEntry const& entry : unlocked)
            {
                if (entry.displayId == displayId)
                {
                    creatureEntry = entry.creatureEntry;
                    targetScale = entry.targetScale;
                    break;
                }
            }

            PetTransmogUnlockEntry preview;
            preview.displayId = displayId;
            preview.creatureEntry = creatureEntry;
            preview.targetScale = targetScale;
            SendPreviewCreatureQuery(player, preview);

            uint32 previewEntry = GetPreviewCreatureEntry(displayId);
            uint32 scaleMilli = uint32(std::lround(targetScale * 1000.f));
            ss << "|PET:" << pet->GetEntry() << ":" << displayId
               << ":" << nativeDisplay << ":" << previewEntry << ":"
               << scaleMilli << ":" << pet->GetName();
        }

        SendPtRes(player, ss.str());
    }

    void SendList(Player* player, uint32 page, uint32 pageSize,
        std::string search, uint32 requestToken)
    {
        std::transform(search.begin(), search.end(), search.begin(), ::tolower);

        std::vector<PetTransmogUnlockEntry> entries =
            PetTransmogManager::GetUnlockedAppearances(player);

        std::vector<PetTransmogUnlockEntry> filtered;
        filtered.reserve(entries.size());
        for (PetTransmogUnlockEntry const& entry : entries)
        {
            std::string name = PetTransmogManager::GetCreatureNameForEntry(
                player, entry.creatureEntry);
            if (ItemMatchesSearch(name, search))
                filtered.push_back(entry);
        }

        if (pageSize < 1)
            pageSize = PTMOG_LIST_PAGE_SIZE;
        if (pageSize > 50)
            pageSize = 50;

        uint32 offset = page > 0 ? (page - 1) * pageSize : 0;
        bool hasMore = offset + pageSize < filtered.size();

        std::ostringstream ss;
        ss << "PTMOG:LIST|" << page << ":" << (hasMore ? 1 : 0)
           << ":" << requestToken << "|";

        bool first = true;
        for (uint32 i = offset; i < filtered.size() && i < offset + pageSize; ++i)
        {
            if (!first)
                ss << ",";
            first = false;

            PetTransmogUnlockEntry const& entry = filtered[i];
            uint32 scaleMilli = uint32(std::lround(entry.targetScale * 1000.f));
            std::string name = PetTransmogManager::GetCreatureNameForEntry(
                player, entry.creatureEntry);
            for (char& ch : name)
            {
                if (ch == ':' || ch == ',' || ch == '|')
                    ch = ' ';
            }

            SendPreviewCreatureQuery(player, entry);
            ss << entry.displayId << ":"
               << GetPreviewCreatureEntry(entry.displayId) << ":"
               << scaleMilli << "::" << name;
        }

        SendPtRes(player, ss.str());
    }

    void SendApplyResult(Player* player, PetTransmogManager::ApplyResult result,
        uint32 displayId = 0)
    {
        switch (result)
        {
            case PetTransmogManager::ApplyResult::Ok:
            {
                Pet* pet = player->GetPet();
                std::ostringstream ss;
                ss << "PTMOG:APPLY|OK|" << displayId;
                if (pet)
                    ss << ":" << pet->GetDisplayId();
                SendPtRes(player, ss.str());
                player->GetSession()->SendAreaTriggerMessage(
                    "Pet appearance applied.");
                SendState(player);
                break;
            }
            case PetTransmogManager::ApplyResult::NoPet:
                SendPtRes(player, "PTMOG:APPLY|ERR|NO_PET");
                break;
            case PetTransmogManager::ApplyResult::NotUnlocked:
                SendPtRes(player, "PTMOG:APPLY|ERR|LOCKED");
                break;
            case PetTransmogManager::ApplyResult::NotEnoughMoney:
                SendPtRes(player, "PTMOG:APPLY|ERR|MONEY");
                break;
            default:
                SendPtRes(player, "PTMOG:APPLY|ERR|INVALID");
                break;
        }
    }

    void HandlePetTransmogRequest(Player* player, std::string_view payload,
        std::string const& searchExtra)
    {
        if (!player || payload.empty())
            return;

        if (!player->IsClass(CLASS_HUNTER, CLASS_CONTEXT_PET))
            return;

        std::vector<std::string_view> parts = Acore::Tokenize(payload, ':', false);
        if (parts.size() < 2 || parts[0] != "PTMOG")
            return;

        std::string_view cmd = parts[1];

        if (cmd == "STATE")
        {
            SendState(player);
            return;
        }

        if (cmd == "LIST" && parts.size() >= 5)
        {
            uint32 page = 1;
            uint32 pageSize = PTMOG_LIST_PAGE_SIZE;
            uint32 requestToken = 0;
            try
            {
                page = uint32(std::stoul(std::string(parts[2])));
                pageSize = uint32(std::stoul(std::string(parts[3])));
                requestToken = uint32(std::stoul(std::string(parts[4])));
            }
            catch (...) { return; }

            SendList(player, page, pageSize, searchExtra, requestToken);
            return;
        }

        if (cmd == "APPLY" && parts.size() >= 3)
        {
            uint32 displayId = 0;
            try { displayId = uint32(std::stoul(std::string(parts[2]))); }
            catch (...) { return; }

            SendApplyResult(player,
                PetTransmogManager::ApplyAppearance(player, displayId), displayId);
            return;
        }

        if (cmd == "RESTORE")
        {
            if (PetTransmogManager::RestoreOriginalAppearance(player))
            {
                SendPtRes(player, "PTMOG:RESTORE|OK");
                player->GetSession()->SendAreaTriggerMessage(
                    "Pet appearance restored.");
                SendState(player);
            }
            else
                SendPtRes(player, "PTMOG:RESTORE|ERR|NO_PET");
        }
    }
}

class PetTransmogAddonPlayerScript : public PlayerScript
{
public:
    PetTransmogAddonPlayerScript() : PlayerScript("PetTransmogAddonPlayerScript",
        { PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE }) { }

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& /*type*/,
        uint32& lang, std::string& msg) override
    {
        if (lang != LANG_ADDON || msg.find("MIS_REQ") == std::string::npos)
            return;

        std::vector<std::string_view> tokens = Acore::Tokenize(msg, '\t', false);
        if (tokens.empty())
            return;

        std::string_view payload;
        std::string searchExtra;

        if (tokens.size() >= 2 && tokens[0] == "MIS_REQ")
        {
            payload = tokens[1];
            if (tokens.size() >= 3)
                searchExtra = std::string(tokens[2]);
        }
        else
            payload = tokens[0];

        if (payload.rfind("PTMOG:", 0) != 0)
            return;

        HandlePetTransmogRequest(player, payload, searchExtra);
    }
};

class mod_pet_transmog_pet_script : public PetScript
{
public:
    mod_pet_transmog_pet_script() : PetScript("mod_pet_transmog_pet_script",
        { PETHOOK_ON_PET_ADD_TO_WORLD }) { }

    void OnPetAddToWorld(Pet* pet) override
    {
        if (!pet)
            return;

        Player* player = PetTransmogManager::ResolveHunterOwner(pet);
        if (!player || !player->IsClass(CLASS_HUNTER, CLASS_CONTEXT_PET))
            return;

        PetTransmogManager::UnlockNativeAppearance(player, pet);
        PetTransmogManager::ApplyScaleOnSpawn(pet);
    }
};

class PetTransmogTameSpellScript : public AllSpellScript
{
public:
    PetTransmogTameSpellScript() : AllSpellScript("PetTransmogTameSpellScript",
        { ALLSPELLHOOK_ON_CAST }) { }

    void OnSpellCast(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo,
        bool /*skipCheck*/) override
    {
        if (!caster || !caster->IsPlayer() || !spellInfo)
            return;

        bool hasTame = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_TAMECREATURE)
            {
                hasTame = true;
                break;
            }
        }

        if (!hasTame)
            return;

        Player* player = caster->ToPlayer();
        if (!player->IsClass(CLASS_HUNTER, CLASS_CONTEXT_PET))
            return;

        if (Pet* pet = player->GetPet())
            PetTransmogManager::UnlockNativeAppearance(player, pet);
    }
};

void AddSC_PetTransmog()
{
    new PetTransmogAddonPlayerScript();
    new mod_pet_transmog_pet_script();
    new PetTransmogTameSpellScript();
}
