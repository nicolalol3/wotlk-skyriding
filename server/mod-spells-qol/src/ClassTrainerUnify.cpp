#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "NPCPackets.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "RidingTrainerAccess.h"
#include "ScriptMgr.h"
#include "ServerScript.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <unordered_map>

namespace
{
    bool sUnifyEnabled = true;

    struct TrainerGroupKey
    {
        Trainer::Type type = Trainer::Type::Class;
        uint32 requirement = 0;

        bool operator==(TrainerGroupKey const& other) const
        {
            return type == other.type && requirement == other.requirement;
        }
    };

    struct TrainerGroupKeyHash
    {
        size_t operator()(TrainerGroupKey const& key) const
        {
            return std::hash<uint32_t>()(AsUnderlyingType(key.type))
                ^ (std::hash<uint32_t>()(key.requirement) << 1);
        }
    };

    std::unordered_map<uint32, std::vector<Trainer::Spell>> sUnifiedSpellsByTrainerId;
    std::unordered_map<uint32, uint32> sCreatureToTrainerId;

    uint8 ResolveReqLevel(Trainer::Spell const& trainerSpell)
    {
        uint8 reqLevel = trainerSpell.ReqLevel;
        if (reqLevel >= 61 && reqLevel <= 80
            && !sSpellMgr->GetPrevSpellInChain(trainerSpell.SpellId))
            reqLevel = 60;

        if (trainerSpell.SpellId == 33391)
            reqLevel = 20;
        else if (trainerSpell.SpellId == 34090)
            reqLevel = 40;
        else if (trainerSpell.SpellId == 34091)
            reqLevel = 50;
        else if (trainerSpell.SpellId == 54197)
            reqLevel = 40;

        return reqLevel;
    }

    int32 ResolveMoneyCost(Player* player, Creature* npc,
        Trainer::Spell const& trainerSpell)
    {
        float reputationDiscount = player->GetReputationPriceDiscount(npc);
        int32 moneyCost = int32(trainerSpell.MoneyCost * reputationDiscount);

        if (trainerSpell.SpellId == 33391)
            moneyCost = 50000;
        else if (trainerSpell.SpellId == 34090)
            moneyCost = 2000000;
        else if (trainerSpell.SpellId == 34091)
            moneyCost = 20000000;
        else if (trainerSpell.SpellId == 54197)
            moneyCost = 10000;

        return moneyCost;
    }

    Trainer::SpellState GetVanillaTrainerSpellState(
        Player const* player, Trainer::Spell const& trainerSpell)
    {
        if (player->HasSpell(trainerSpell.SpellId))
            return Trainer::SpellState::Known;

        if (!player->IsSpellFitByClassAndRace(trainerSpell.SpellId))
            return Trainer::SpellState::Unavailable;

        if (trainerSpell.ReqSkillLine
            && player->GetBaseSkillValue(trainerSpell.ReqSkillLine)
                < trainerSpell.ReqSkillRank)
            return Trainer::SpellState::Unavailable;

        for (int32 reqAbility : trainerSpell.ReqAbility)
            if (reqAbility && !player->HasSpell(reqAbility))
                return Trainer::SpellState::Unavailable;

        if (player->GetLevel() < ResolveReqLevel(trainerSpell))
            return Trainer::SpellState::Unavailable;

        bool hasLearnSpellEffect = false;
        bool knowsAllLearnedSpells = true;
        SpellInfo const* trainerSpellInfo =
            sSpellMgr->AssertSpellInfo(trainerSpell.SpellId);

        for (SpellEffectInfo const& spellEffectInfo :
            trainerSpellInfo->GetEffects())
        {
            if (!spellEffectInfo.IsEffect(SPELL_EFFECT_LEARN_SPELL))
                continue;

            hasLearnSpellEffect = true;
            if (!player->HasSpell(spellEffectInfo.TriggerSpell))
                knowsAllLearnedSpells = false;

            if (uint32 previousRankSpellId =
                    sSpellMgr->GetPrevSpellInChain(spellEffectInfo.TriggerSpell))
                if (!player->HasSpell(previousRankSpellId))
                    return Trainer::SpellState::Unavailable;
        }

        if (!hasLearnSpellEffect)
        {
            if (uint32 previousRankSpellId =
                    sSpellMgr->GetPrevSpellInChain(trainerSpell.SpellId))
                if (!player->HasSpell(previousRankSpellId))
                    return Trainer::SpellState::Unavailable;
        }
        else if (knowsAllLearnedSpells)
            return Trainer::SpellState::Known;

        for (auto const& requirePair :
            sSpellMgr->GetSpellsRequiredForSpellBounds(trainerSpell.SpellId))
            if (!player->HasSpell(requirePair.second))
                return Trainer::SpellState::Unavailable;

        return Trainer::SpellState::Available;
    }

    bool CanBuyVanillaTrainerSpell(Player const* player,
        Trainer::Trainer const* trainer, Trainer::Spell const& trainerSpell)
    {
        if (!player || !trainer)
            return false;

        if (!trainer->IsTrainerValidForPlayer(player))
            return false;

        if (GetVanillaTrainerSpellState(player, trainerSpell)
            != Trainer::SpellState::Available)
            return false;

        SpellInfo const* trainerSpellInfo =
            sSpellMgr->AssertSpellInfo(trainerSpell.SpellId);

        for (SpellEffectInfo const& spellEffectInfo :
            trainerSpellInfo->GetEffects())
        {
            if (!spellEffectInfo.IsEffect(SPELL_EFFECT_LEARN_SPELL))
                continue;

            SpellInfo const* learnedSpellInfo =
                sSpellMgr->GetSpellInfo(spellEffectInfo.TriggerSpell);
            if (learnedSpellInfo && learnedSpellInfo->IsPrimaryProfessionFirstRank()
                && !player->GetFreePrimaryProfessionPoints())
                return false;
        }

        return true;
    }

    void SendTrainerBuyFailed(Creature const* npc, Player const* player,
        uint32 spellId, Trainer::FailReason reason)
    {
        WorldPackets::NPC::TrainerBuyFailed packet;
        packet.TrainerGUID = npc->GetGUID();
        packet.SpellID = spellId;
        packet.TrainerFailedReason = AsUnderlyingType(reason);
        player->SendDirectMessage(packet.Write());
    }

    void SendTrainerBuySucceeded(Creature const* npc, Player const* player,
        uint32 spellId)
    {
        WorldPackets::NPC::TrainerBuySucceeded packet;
        packet.TrainerGUID = npc->GetGUID();
        packet.SpellID = spellId;
        player->SendDirectMessage(packet.Write());
    }

    void TeachVanillaTrainerSpell(Creature* npc, Player* player,
        Trainer::Trainer const* trainer, Trainer::Spell const& trainerSpell,
        uint32 spellId)
    {
        if (!CanBuyVanillaTrainerSpell(player, trainer, trainerSpell))
        {
            SendTrainerBuyFailed(npc, player, spellId,
                Trainer::FailReason::NotEnoughSkill);
            return;
        }

        int32 moneyCost = ResolveMoneyCost(player, npc, trainerSpell);
        if (!player->HasEnoughMoney(moneyCost))
        {
            SendTrainerBuyFailed(npc, player, spellId,
                Trainer::FailReason::NotEnoughMoney);
            return;
        }

        player->ModifyMoney(-moneyCost);

        npc->SendPlaySpellVisual(179);
        npc->SendPlaySpellImpact(player->GetGUID(), 362);

        if (trainerSpell.IsCastable())
            player->CastSpell(player, trainerSpell.SpellId, true);
        else
            player->learnSpell(trainerSpell.SpellId, false);

        SendTrainerBuySucceeded(npc, player, spellId);
    }

    uint32 GetTrainerIdForCreature(uint32 creatureEntry)
    {
        auto itr = sCreatureToTrainerId.find(creatureEntry);
        if (itr != sCreatureToTrainerId.end())
            return itr->second;

        return 0;
    }

    Trainer::Spell const* GetUnifiedSpell(uint32 trainerId, uint32 spellId)
    {
        auto itr = sUnifiedSpellsByTrainerId.find(trainerId);
        if (itr == sUnifiedSpellsByTrainerId.end())
            return nullptr;

        for (Trainer::Spell const& trainerSpell : itr->second)
            if (trainerSpell.SpellId == spellId)
                return &trainerSpell;

        return nullptr;
    }

    bool IsClassTrainerType(Trainer::Type type)
    {
        return type == Trainer::Type::Class;
    }

    WorldPackets::NPC::TrainerListSpell BuildTrainerListEntry(
        Player* player, Creature* npc, Trainer::Spell const& trainerSpell)
    {
        SpellInfo const* trainerSpellInfo =
            sSpellMgr->AssertSpellInfo(trainerSpell.SpellId);

        bool primaryProfessionFirstRank = false;
        for (SpellEffectInfo const& spellEffectInfo :
            trainerSpellInfo->GetEffects())
        {
            if (!spellEffectInfo.IsEffect(SPELL_EFFECT_LEARN_SPELL))
                continue;

            SpellInfo const* learnedSpellInfo =
                sSpellMgr->GetSpellInfo(spellEffectInfo.TriggerSpell);
            if (learnedSpellInfo && learnedSpellInfo->IsPrimaryProfessionFirstRank())
                primaryProfessionFirstRank = true;
        }

        WorldPackets::NPC::TrainerListSpell entry;
        entry.SpellID = trainerSpell.SpellId;
        entry.Usable = AsUnderlyingType(
            GetVanillaTrainerSpellState(player, trainerSpell));
        entry.MoneyCost = ResolveMoneyCost(player, npc, trainerSpell);
        entry.ReqLevel = ResolveReqLevel(trainerSpell);
        entry.ReqSkillLine = trainerSpell.ReqSkillLine;
        entry.ReqSkillRank = trainerSpell.ReqSkillRank;
        std::copy(trainerSpell.ReqAbility.begin(), trainerSpell.ReqAbility.end(),
            entry.ReqAbility.begin());
        entry.PointCost[0] = 0;
        entry.PointCost[1] = primaryProfessionFirstRank ? 1 : 0;

        return entry;
    }

    void LoadUnifiedClassTrainers()
    {
        sUnifyEnabled = sConfigMgr->GetOption<bool>(
            "SpellsQoL.UnifyClassTrainers.Enable", true);

        sUnifiedSpellsByTrainerId.clear();
        sCreatureToTrainerId.clear();

        if (!sUnifyEnabled)
            return;

        std::unordered_map<uint32, Trainer::Type> trainerTypes;
        std::unordered_map<uint32, uint32> trainerRequirements;
        std::unordered_map<uint32, std::vector<Trainer::Spell>> spellsByTrainerId;

        if (QueryResult creatureTrainers = WorldDatabase.Query(
            "SELECT CreatureId, TrainerId FROM creature_default_trainer"))
        {
            do
            {
                Field* fields = creatureTrainers->Fetch();
                sCreatureToTrainerId[fields[0].Get<uint32>()] =
                    fields[1].Get<uint32>();
            } while (creatureTrainers->NextRow());
        }

        if (QueryResult trainerSpells = WorldDatabase.Query(
            "SELECT TrainerId, SpellId, MoneyCost, ReqSkillLine, ReqSkillRank, "
            "ReqAbility1, ReqAbility2, ReqAbility3, ReqLevel FROM trainer_spell"))
        {
            do
            {
                Field* fields = trainerSpells->Fetch();
                uint32 trainerId = fields[0].Get<uint32>();

                Trainer::Spell spell;
                spell.SpellId = fields[1].Get<uint32>();
                spell.MoneyCost = fields[2].Get<uint32>();
                spell.ReqSkillLine = fields[3].Get<uint32>();
                spell.ReqSkillRank = fields[4].Get<uint32>();
                spell.ReqAbility[0] = fields[5].Get<uint32>();
                spell.ReqAbility[1] = fields[6].Get<uint32>();
                spell.ReqAbility[2] = fields[7].Get<uint32>();
                spell.ReqLevel = fields[8].Get<uint8>();

                if (!sSpellMgr->GetSpellInfo(spell.SpellId))
                    continue;

                spellsByTrainerId[trainerId].push_back(spell);
            } while (trainerSpells->NextRow());
        }

        if (QueryResult trainers = WorldDatabase.Query(
            "SELECT Id, Type, Requirement FROM trainer"))
        {
            do
            {
                Field* fields = trainers->Fetch();
                uint32 trainerId = fields[0].Get<uint32>();
                Trainer::Type type = Trainer::Type(fields[1].Get<uint8>());

                if (!IsClassTrainerType(type))
                    continue;

                trainerTypes[trainerId] = type;
                trainerRequirements[trainerId] = fields[2].Get<uint32>();
            } while (trainers->NextRow());
        }

        std::unordered_map<TrainerGroupKey,
            std::vector<uint32>, TrainerGroupKeyHash> trainersPerGroup;

        for (auto const& [trainerId, type] : trainerTypes)
        {
            TrainerGroupKey key{ type, trainerRequirements[trainerId] };
            trainersPerGroup[key].push_back(trainerId);
        }

        for (auto const& [groupKey, trainerIds] : trainersPerGroup)
        {
            uint32 bestTrainerId = 0;
            size_t bestSpellCount = 0;

            for (uint32 trainerId : trainerIds)
            {
                size_t spellCount = spellsByTrainerId[trainerId].size();
                if (spellCount > bestSpellCount)
                {
                    bestSpellCount = spellCount;
                    bestTrainerId = trainerId;
                }
            }

            if (!bestTrainerId)
                continue;

            std::unordered_map<uint32, Trainer::Spell> unifiedSpells;
            for (Trainer::Spell const& spell :
                spellsByTrainerId[bestTrainerId])
                unifiedSpells[spell.SpellId] = spell;

            for (uint32 trainerId : trainerIds)
            {
                if (trainerId == bestTrainerId)
                    continue;

                for (Trainer::Spell const& spell : spellsByTrainerId[trainerId])
                {
                    if (unifiedSpells.find(spell.SpellId) == unifiedSpells.end())
                        unifiedSpells[spell.SpellId] = spell;
                }
            }

            std::vector<Trainer::Spell> unifiedList;
            unifiedList.reserve(unifiedSpells.size());
            for (auto const& pair : unifiedSpells)
                unifiedList.push_back(pair.second);

            for (uint32 trainerId : trainerIds)
                sUnifiedSpellsByTrainerId[trainerId] = unifiedList;
        }
    }

    void RewriteClassTrainerListPacket(Player* player, WorldPacket& packet)
    {
        if (!player || !sUnifyEnabled)
            return;

        WorldPacket data(packet);
        data.rpos(0);

        ObjectGuid trainerGuid;
        int32 trainerTypeInt = 0;
        data >> trainerGuid >> trainerTypeInt;

        if (trainerTypeInt == AsUnderlyingType(Trainer::Type::Tradeskill))
            return;

        Creature* npc = ObjectAccessor::GetCreature(*player, trainerGuid);
        if (!npc)
            return;

        if (SpellsQoLRidingTrainer::IsRidingTrainer(npc->GetEntry()))
            return;

        Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
        if (!trainer || !IsClassTrainerType(trainer->GetTrainerType()))
            return;

        uint32 trainerId = GetTrainerIdForCreature(npc->GetEntry());
        auto spellsItr = sUnifiedSpellsByTrainerId.find(trainerId);
        if (spellsItr == sUnifiedSpellsByTrainerId.end())
            return;

        int32 spellCount = 0;
        data >> spellCount;
        for (int32 i = 0; i < spellCount; ++i)
        {
            int32 spellId = 0;
            uint8 usable = 0;
            int32 moneyCost = 0;
            int32 pointCost0 = 0;
            int32 pointCost1 = 0;
            uint8 reqLevel = 0;
            int32 reqSkillLine = 0;
            int32 reqSkillRank = 0;
            int32 reqAbility0 = 0;
            int32 reqAbility1 = 0;
            int32 reqAbility2 = 0;

            data >> spellId >> usable >> moneyCost >> pointCost0 >> pointCost1
                >> reqLevel >> reqSkillLine >> reqSkillRank >> reqAbility0
                >> reqAbility1 >> reqAbility2;
        }

        std::string greeting;
        data >> greeting;

        std::vector<WorldPackets::NPC::TrainerListSpell> spells;
        spells.reserve(spellsItr->second.size());

        for (Trainer::Spell const& trainerSpell : spellsItr->second)
        {
            if (!player->IsSpellFitByClassAndRace(trainerSpell.SpellId))
                continue;

            spells.push_back(BuildTrainerListEntry(player, npc, trainerSpell));
        }

        packet.Initialize(SMSG_TRAINER_LIST);
        packet << trainerGuid;
        packet << trainerTypeInt;
        packet << int32(spells.size());

        for (WorldPackets::NPC::TrainerListSpell const& spell : spells)
        {
            packet << spell.SpellID;
            packet << uint8(spell.Usable);
            packet << spell.MoneyCost;
            for (int32 pointCost : spell.PointCost)
                packet << pointCost;
            packet << uint8(spell.ReqLevel);
            packet << spell.ReqSkillLine;
            packet << spell.ReqSkillRank;
            for (int32 reqAbility : spell.ReqAbility)
                packet << reqAbility;
        }

        packet << greeting;
    }

    bool TryHandleClassTrainerBuy(WorldSession* session, WorldPacket& packet)
    {
        if (!sUnifyEnabled || !session)
            return false;

        Player* player = session->GetPlayer();
        if (!player)
            return false;

        WorldPacket data(packet);
        data.rpos(0);

        ObjectGuid trainerGuid;
        int32 spellId = 0;
        data >> trainerGuid >> spellId;

        Creature* npc = player->GetNPCIfCanInteractWith(
            trainerGuid, UNIT_NPC_FLAG_TRAINER);
        if (!npc)
            return false;

        if (player->HasUnitState(UNIT_STATE_DIED))
            player->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
        if (!trainer || !IsClassTrainerType(trainer->GetTrainerType()))
            return false;

        if (SpellsQoLRidingTrainer::IsRidingTrainer(npc->GetEntry()))
            return false;

        uint32 trainerId = GetTrainerIdForCreature(npc->GetEntry());
        Trainer::Spell const* trainerSpell = GetUnifiedSpell(trainerId, spellId);
        if (!trainerSpell)
            return false;

        TeachVanillaTrainerSpell(npc, player, trainer, *trainerSpell, spellId);
        return true;
    }
}

class SpellsQoLClassTrainerUnifyServer : public ServerScript
{
public:
    SpellsQoLClassTrainerUnifyServer()
        : ServerScript("SpellsQoLClassTrainerUnifyServer",
            { SERVERHOOK_CAN_PACKET_RECEIVE, SERVERHOOK_CAN_PACKET_SEND })
    {
    }

    bool CanPacketReceive(WorldSession* session, WorldPacket& packet) override
    {
        if (!session || packet.GetOpcode() != CMSG_TRAINER_BUY_SPELL)
            return true;

        if (TryHandleClassTrainerBuy(session, packet))
            return false;

        return true;
    }

    bool CanPacketSend(WorldSession* session, WorldPacket& packet) override
    {
        if (!session || packet.GetOpcode() != SMSG_TRAINER_LIST)
            return true;

        Player* player = session->GetPlayer();
        if (!player)
            return true;

        RewriteClassTrainerListPacket(player, packet);
        return true;
    }
};

class SpellsQoLClassTrainerUnifyWorld : public WorldScript
{
public:
    SpellsQoLClassTrainerUnifyWorld()
        : WorldScript("SpellsQoLClassTrainerUnifyWorld",
            { WORLDHOOK_ON_STARTUP })
    {
    }

    void OnStartup() override
    {
        LoadUnifiedClassTrainers();
    }
};

void AddSC_ClassTrainerUnify()
{
    new SpellsQoLClassTrainerUnifyWorld();
    new SpellsQoLClassTrainerUnifyServer();
}
