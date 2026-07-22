#include "AllCreatureScript.h"

#include "Chat.h"

#include "Config.h"

#include "Creature.h"

#include "DatabaseEnv.h"

#include "GossipDef.h"

#include "NPCPackets.h"

#include "ObjectAccessor.h"

#include "ObjectMgr.h"

#include "Opcodes.h"

#include "Player.h"

#include "ScriptMgr.h"

#include "ScriptedGossip.h"

#include "ServerScript.h"

#include "Trainer.h"

#include "Unit.h"

#include "UnitScript.h"

#include "World.h"

#include "WorldScript.h"

#include "RidingTrainerAccess.h"

#include "RidingTrainerUnify.h"

#include "WorldSession.h"



#include <unordered_set>



namespace SpellsQoLRidingTrainer

{

    bool sEnabled = true;

    std::unordered_set<uint32> sRidingTrainerEntries;



    bool IsRidingTrainer(Creature const* creature)

    {

        return creature && sRidingTrainerEntries.contains(creature->GetEntry());

    }



    bool IsRidingTrainer(uint32 creatureEntry)

    {

        return sRidingTrainerEntries.contains(creatureEntry);

    }



    void LoadRidingTrainerEntries()

    {

        sEnabled = sConfigMgr->GetOption<bool>(

            "SpellsQoL.RidingTrainer.IgnoreReputation", true);



        sRidingTrainerEntries.clear();

        if (!sEnabled)

            return;



        if (QueryResult result = WorldDatabase.Query(

            "SELECT cdt.CreatureId FROM creature_default_trainer cdt "

            "INNER JOIN trainer t ON t.Id = cdt.TrainerId "

            "WHERE t.Type = 1"))

        {

            do

            {

                Field* fields = result->Fetch();

                sRidingTrainerEntries.insert(fields[0].Get<uint32>());

            } while (result->NextRow());

        }

    }



    Creature* GetRidingTrainerNpc(Player* player, ObjectGuid const& guid)

    {

        if (!player)

            return nullptr;



        if (Creature* npc = player->GetNPCIfCanInteractWith(

            guid, UNIT_NPC_FLAG_TRAINER))

        {

            if (IsRidingTrainer(npc))

                return npc;

        }



        if (Creature* npc = player->GetNPCIfCanInteractWith(

            guid, UNIT_NPC_FLAG_NONE))

        {

            if (IsRidingTrainer(npc))

                return npc;

        }



        return nullptr;

    }



    void OpenRidingTrainer(Player* player, Creature* creature)

    {

        if (!player || !creature)

            return;



        Trainer::Trainer const* trainer =

            sObjectMgr->GetTrainer(creature->GetEntry());

        if (!trainer)

            return;



        trainer->SendSpells(creature, player,

            player->GetSession()->GetSessionDbLocaleIndex());

    }

}

namespace SpellsQoLRidingPanelNotify
{
    constexpr char const* ADDON_PREFIX = "HT_RID";

    void SendOpen(Player* player)
    {
        if (!player)
            return;

        std::string const msg = std::string(ADDON_PREFIX) + "\tOPEN";
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON,
            player, player, msg);
        player->SendDirectMessage(&data);
    }

    void OnTrainerListSent(Player* player, WorldPacket& packet)
    {
        if (!player)
            return;

        WorldPacket data(packet);
        data.rpos(0);

        ObjectGuid trainerGuid;
        int32 trainerTypeInt = 0;
        data >> trainerGuid >> trainerTypeInt;

        if (trainerTypeInt != AsUnderlyingType(Trainer::Type::Mount))
            return;

        Creature* npc = ObjectAccessor::GetCreature(*player, trainerGuid);
        if (!npc || !SpellsQoLRidingTrainer::IsRidingTrainer(npc->GetEntry()))
            return;

        SendOpen(player);
    }
}

namespace

{

    using namespace SpellsQoLRidingTrainer;



    class SpellsQoLRidingTrainerAccessServer : public ServerScript

    {

    public:

        SpellsQoLRidingTrainerAccessServer()

            : ServerScript("SpellsQoLRidingTrainerAccessServer",

                { SERVERHOOK_CAN_PACKET_RECEIVE, SERVERHOOK_CAN_PACKET_SEND })

        {

        }



        bool CanPacketSend(WorldSession* session, WorldPacket& packet) override

        {

            if (!sEnabled || !session

                || packet.GetOpcode() != SMSG_TRAINER_LIST)

                return true;



            Player* player = session->GetPlayer();

            if (!player)

                return true;



            SpellsQoLRidingTrainerUnify::RewriteTrainerListPacket(

                player, packet);

            SpellsQoLRidingPanelNotify::OnTrainerListSent(player, packet);

            return true;

        }



        bool CanPacketReceive(WorldSession* session,

            WorldPacket& packet) override

        {

            if (!sEnabled || !session)

                return true;



            Player* player = session->GetPlayer();

            if (!player)

                return true;



            uint16 opcode = packet.GetOpcode();



            if (opcode == CMSG_TRAINER_BUY_SPELL)

            {

                if (TryHandleRidingTrainerBuy(packet, player))

                    return false;

                return true;

            }



            if (opcode != CMSG_TRAINER_LIST)

                return true;



            WorldPacket data(packet);

            data.rpos(0);



            ObjectGuid guid;

            data >> guid;



            Creature* creature = GetRidingTrainerNpc(player, guid);

            if (!creature)

                return true;



            OpenRidingTrainer(player, creature);

            return false;

        }



        bool TryHandleRidingTrainerBuy(WorldPacket& packet, Player* player)

        {

            WorldPacket data(packet);

            data.rpos(0);



            ObjectGuid trainerGuid;

            int32 spellId = 0;

            data >> trainerGuid >> spellId;



            Creature* creature = GetRidingTrainerNpc(player, trainerGuid);

            if (!creature)

                return false;



            if (player->HasUnitState(UNIT_STATE_DIED))

                player->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);



            if (SpellsQoLRidingTrainerUnify::TryHandleTrainerBuy(

                player, creature, spellId))

                return true;



            Trainer::Trainer* trainer =

                sObjectMgr->GetTrainer(creature->GetEntry());

            if (!trainer)

                return false;



            trainer->TeachSpell(creature, player, spellId);

            return true;

        }

    };



    class SpellsQoLRidingTrainerAccessGossip : public AllCreatureScript

    {

    public:

        SpellsQoLRidingTrainerAccessGossip()

            : AllCreatureScript("SpellsQoLRidingTrainerAccessGossip")

        {

        }



        bool CanCreatureGossipHello(Player* player, Creature* creature) override

        {

            if (!sEnabled || !player || !creature

                || !IsRidingTrainer(creature))

                return false;



            ClearGossipMenuFor(player);

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, GOSSIP_TEXT_TRAIN,

                GOSSIP_SENDER_MAIN, GOSSIP_OPTION_TRAINER);

            SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature);



            OpenRidingTrainer(player, creature);

            return true;

        }

    };



    class SpellsQoLRidingTrainerAccessUnit : public UnitScript

    {

    public:

        SpellsQoLRidingTrainerAccessUnit()

            : UnitScript("SpellsQoLRidingTrainerAccessUnit", true,

                { UNITHOOK_IF_NORMAL_REACTION, UNITHOOK_ON_PATCH_VALUES_UPDATE })

        {

        }



        bool IfNormalReaction(Unit const* unit, Unit const* target,

            ReputationRank& repRank) override

        {

            if (!sEnabled)

                return true;



            Creature const* creature = unit->ToCreature();

            Player const* player = target->ToPlayer();

            if (!creature || !player || !IsRidingTrainer(creature))

                return true;



            repRank = REP_NEUTRAL;

            return false;

        }



        void OnPatchValuesUpdate(Unit const* unit, ByteBuffer& valuesUpdateBuf,

            BuildValuesCachePosPointers& posPointers, Player* target) override

        {

            if (!sEnabled || !target || posPointers.UnitNPCFlagsPos < 0)

                return;



            Creature const* creature = unit->ToCreature();

            if (!creature || !IsRidingTrainer(creature))

                return;



            if (!(creature->GetNpcFlags() & UNIT_NPC_FLAG_TRAINER))

                return;



            uint32 appendValue = creature->GetNpcFlags();



            if (sWorld->getIntConfig(CONFIG_INSTANT_TAXI) == 2

                && (appendValue & UNIT_NPC_FLAG_FLIGHTMASTER))

                appendValue |= UNIT_NPC_FLAG_GOSSIP;



            if (!target->CanSeeSpellClickOn(creature))

                appendValue &= ~UNIT_NPC_FLAG_SPELLCLICK;



            if (!target->CanSeeVendor(creature))

            {

                appendValue &= ~UNIT_NPC_FLAG_REPAIR;

                appendValue &= ~UNIT_NPC_FLAG_VENDOR_MASK;

            }



            valuesUpdateBuf.put(posPointers.UnitNPCFlagsPos, appendValue);

        }

    };



    class SpellsQoLRidingTrainerAccessWorld : public WorldScript

    {

    public:

        SpellsQoLRidingTrainerAccessWorld()

            : WorldScript("SpellsQoLRidingTrainerAccessWorld",

                { WORLDHOOK_ON_STARTUP })

        {

        }



        void OnStartup() override

        {

            LoadRidingTrainerEntries();

            SpellsQoLRidingTrainerUnify::Load();

        }

    };

}



void AddSC_RidingTrainerAccess()

{

    new SpellsQoLRidingTrainerAccessWorld();

    new SpellsQoLRidingTrainerAccessGossip();

    new SpellsQoLRidingTrainerAccessUnit();

    new SpellsQoLRidingTrainerAccessServer();

}


