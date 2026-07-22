#include "ScriptMgr.h"
#include "Chat.h"
#include "Player.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"

using namespace Acore::ChatCommands;

class IdAroundMeList : public CommandScript
{
public:
    IdAroundMeList() : CommandScript("IdAroundMeList") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable idAroundMeTable = {
            { "", HandleIdAroundMeList, SEC_PLAYER, Console::No }
        };
        return {
            { "idaroundmelist", idAroundMeTable }
        };
    }

    struct AnyCreatureInObjectRangeCheck
    {
        WorldObject const* i_obj;
        float i_range;
        AnyCreatureInObjectRangeCheck(WorldObject const* obj, float range) : i_obj(obj), i_range(range) {}
        bool operator()(Creature* u)
        {
            return i_obj->IsWithinDistInMap(u, i_range);
        }
    };

    static bool HandleIdAroundMeList(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        float range = 20.0f;
        std::list<Creature*> creatureList;
        AnyCreatureInObjectRangeCheck check(player, range);
        Acore::CreatureListSearcher<AnyCreatureInObjectRangeCheck> searcher(player, creatureList, check);
        Cell::VisitObjects(player, searcher, range);

        handler->PSendSysMessage("--- NPCs within 20 yards ---");
        LOG_INFO("module", "ID Around Me List for Player: {} (GUID: {})", player->GetName(), player->GetGUID().ToString());

        uint32 count = 0;
        for (Creature* creature : creatureList)
        {
            handler->PSendSysMessage("ID: |cff00ff00%u|r | Name: |cffffffff%s|r", creature->GetEntry(), creature->GetName().c_str());
            LOG_INFO("module", "ID: {} | Name: {}", creature->GetEntry(), creature->GetName());
            count++;
        }

        if (count == 0)
        {
            handler->PSendSysMessage("No NPCs found within 20 yards.");
        }
        else
        {
            handler->PSendSysMessage("Total found: %u", count);
        }

        return true;
    }
};

void AddSC_IdAroundMeList()
{
    new IdAroundMeList();
}
