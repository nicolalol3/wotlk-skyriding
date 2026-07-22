/*
 * .massdeletemailsonline — mail wipe for online characters only.
 * .massdeletemailsall    — mail wipe for every character (DB + online memory).
 * .mailsarrivenow        — deliver all pending mail immediately (server-wide).
 * Attached items are destroyed, not returned (delete commands only).
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Mail.h"
#include "Item.h"
#include "Bag.h"
#include "CommandScript.h"
#include "GameTime.h"
#include "ObjectMgr.h"
#include "DBCStores.h"

#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    std::string GetActorName(ChatHandler* handler)
    {
        return handler->GetPlayer() ? handler->GetPlayer()->GetName() : "console";
    }

    std::string BuildInList(std::unordered_set<uint32> const& ids)
    {
        std::ostringstream ss;
        bool first = true;
        for (uint32 id : ids)
        {
            if (!first)
                ss << ',';
            ss << id;
            first = false;
        }
        return ss.str();
    }

    void ClearOnlinePlayerMail(Player* player)
    {
        if (!player)
            return;

        std::vector<Mail*> mailsCopy(player->GetMails().begin(), player->GetMails().end());
        for (Mail* mail : mailsCopy)
        {
            if (!mail)
                continue;

            if (mail->HasItems())
            {
                for (MailItemInfo const& mi : mail->items)
                {
                    if (Item* item = player->GetMItem(mi.item_guid))
                    {
                        player->RemoveMItem(mi.item_guid);
                        delete item;
                    }
                }
            }

            player->RemoveMail(mail->messageID);
            delete mail;
        }

        for (auto& pair : player->mMitems)
            delete pair.second;
        player->mMitems.clear();

        player->unReadMails = 0;
        player->m_mailsUpdated = false;
        sCharacterCache->UpdateCharacterMailCount(player->GetGUID(), 0, true);
    }

    void ResetMailCountInCache(std::unordered_set<uint32> const& receivers)
    {
        for (uint32 lowGuid : receivers)
            sCharacterCache->UpdateCharacterMailCount(ObjectGuid(HighGuid::Player, lowGuid), 0, true);
    }

    uint32 CountMailsForReceivers(std::string const& inList)
    {
        if (inList.empty())
            return 0;

        if (QueryResult result = CharacterDatabase.Query("SELECT COUNT(*) FROM mail WHERE receiver IN ({})", inList))
            return (*result)[0].Get<uint32>();

        return 0;
    }

    uint32 CountMailItemsForReceivers(std::string const& inList)
    {
        if (inList.empty())
            return 0;

        if (QueryResult result = CharacterDatabase.Query("SELECT COUNT(*) FROM mail_items WHERE receiver IN ({})", inList))
            return (*result)[0].Get<uint32>();

        return 0;
    }

    void DeleteMailFromDatabaseForReceivers(std::string const& inList)
    {
        if (inList.empty())
            return;

        CharacterDatabase.Execute(
            "DELETE ii FROM item_instance ii "
            "INNER JOIN mail_items mi ON ii.guid = mi.item_guid "
            "WHERE mi.receiver IN ({})", inList);
        CharacterDatabase.Execute("DELETE FROM mail_items WHERE receiver IN ({})", inList);
        CharacterDatabase.Execute("DELETE FROM mail WHERE receiver IN ({})", inList);
    }

    void DeleteAllMailFromDatabase()
    {
        CharacterDatabase.Execute(
            "DELETE ii FROM item_instance ii INNER JOIN mail_items mi ON ii.guid = mi.item_guid");
        CharacterDatabase.Execute("DELETE FROM mail_items");
        CharacterDatabase.Execute("DELETE FROM mail");
    }

    std::unordered_set<uint32> CollectAllMailReceivers()
    {
        std::unordered_set<uint32> receivers;
        if (QueryResult receiverResult = CharacterDatabase.Query("SELECT DISTINCT receiver FROM mail"))
        {
            do
            {
                receivers.insert((*receiverResult)[0].Get<uint32>());
            } while (receiverResult->NextRow());
        }
        return receivers;
    }

    std::unordered_set<uint32> CollectOnlinePlayerLowGuids()
    {
        std::unordered_set<uint32> online;
        for (auto const& playerRef : ObjectAccessor::GetPlayers())
        {
            Player* player = playerRef.second;
            if (!player || !player->IsInWorld())
                continue;

            online.insert(player->GetGUID().GetCounter());
        }
        return online;
    }

    // #region agent log
    void AgentDebugLog(char const* location, char const* message, char const* hypothesisId, std::string const& dataJson)
    {
        std::ofstream log("C:/Azerothcore/debug-d4f826.log", std::ios::app);
        if (!log)
            return;

        log << "{\"sessionId\":\"d4f826\",\"location\":\"" << location << "\",\"message\":\"" << message
            << "\",\"hypothesisId\":\"" << hypothesisId << "\",\"data\":" << dataJson
            << ",\"timestamp\":" << (uint64(GameTime::GetGameTime().count()) * 1000) << ",\"runId\":\"pre-fix\"}\n";
    }
    // #endregion

    Item* LoadMailedItemForPlayer(Player* player, ObjectGuid playerGuid, Mail* mail, Field* fields)
    {
        ObjectGuid::LowType itemGuid = fields[11].Get<uint32>();
        uint32 itemEntry = fields[12].Get<uint32>();

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
        if (!proto)
            return nullptr;

        Item* item = NewItemOrBag(proto);
        ObjectGuid ownerGuid = fields[13].Get<uint32>()
            ? ObjectGuid::Create<HighGuid::Player>(fields[13].Get<uint32>())
            : ObjectGuid::Empty;

        if (!item->LoadFromDB(itemGuid, ownerGuid, fields, itemEntry))
        {
            delete item;
            return nullptr;
        }

        if (mail)
            mail->AddItem(itemGuid, itemEntry);

        player->AddMItem(item);
        return item;
    }

    uint32 SyncDeliveredMailForOnlinePlayer(Player* player, time_t now)
    {
        if (!player)
            return 0;

        std::unordered_set<uint32> knownMailIds;
        for (Mail const* mail : player->GetMails())
            knownMailIds.insert(mail->messageID);

        uint32 lowGuid = player->GetGUID().GetCounter();
        uint32 loadedCount = 0;

        for (Mail const* mail : player->GetMails())
        {
            Mail* mutableMail = player->GetMail(mail->messageID);
            if (mutableMail && mutableMail->deliver_time > now)
                mutableMail->deliver_time = now;
        }

        QueryResult mailsResult = CharacterDatabase.Query(
            "SELECT id, messageType, sender, receiver, subject, body, expire_time, deliver_time, money, cod, checked, stationery, mailTemplateId "
            "FROM mail WHERE receiver = {}", lowGuid);

        std::unordered_map<uint32, Mail*> mailById;

        if (mailsResult)
        {
            do
            {
                Field* fields = mailsResult->Fetch();
                uint32 mailId = fields[0].Get<uint32>();
                if (knownMailIds.count(mailId))
                    continue;

                if (now > time_t(fields[6].Get<uint32>()))
                    continue;

                Mail* m = new Mail;
                m->messageID      = mailId;
                m->messageType    = fields[1].Get<uint8>();
                m->sender         = fields[2].Get<uint32>();
                m->receiver       = fields[3].Get<uint32>();
                m->subject        = fields[4].Get<std::string>();
                m->body           = fields[5].Get<std::string>();
                m->expire_time    = time_t(fields[6].Get<uint32>());
                m->deliver_time   = now;
                m->money          = fields[8].Get<uint32>();
                m->COD            = fields[9].Get<uint32>();
                m->checked        = fields[10].Get<uint8>();
                m->stationery     = fields[11].Get<uint8>();
                m->mailTemplateId = fields[12].Get<int16>();

                if (m->mailTemplateId && !sMailTemplateStore.LookupEntry(m->mailTemplateId))
                    m->mailTemplateId = 0;

                m->state = MAIL_STATE_UNCHANGED;
                player->AddMail(m);
                mailById[mailId] = m;
                ++loadedCount;
            } while (mailsResult->NextRow());
        }

        if (!mailById.empty())
        {
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_MAILITEMS);
            stmt->SetData(0, lowGuid);
            PreparedQueryResult mailItemsResult = CharacterDatabase.Query(stmt);

            if (mailItemsResult)
            {
                do
                {
                    Field* fields = mailItemsResult->Fetch();
                    uint32 mailId = fields[14].Get<uint32>();
                    auto itr = mailById.find(mailId);
                    if (itr != mailById.end())
                        LoadMailedItemForPlayer(player, player->GetGUID(), itr->second, fields);
                } while (mailItemsResult->NextRow());
            }
        }

        return loadedCount;
    }
}

class MassDeleteEmails_Command : public CommandScript
{
public:
    MassDeleteEmails_Command() : CommandScript("MassDeleteEmails_Command") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "massdeletemailsonline", HandleMassDeleteEmailsOnline, SEC_ADMINISTRATOR, Console::Yes },
            { "massdeletemailsall",    HandleMassDeleteEmailsAll,    SEC_ADMINISTRATOR, Console::Yes },
            { "mailsarrivenow",        HandleMailArriveNow,          SEC_ADMINISTRATOR, Console::Yes },
        };
        return commandTable;
    }

    static bool HandleMassDeleteEmailsOnline(ChatHandler* handler, char const* /*args*/)
    {
        std::unordered_set<uint32> const onlineReceivers = CollectOnlinePlayerLowGuids();
        if (onlineReceivers.empty())
        {
            handler->SendSysMessage("|cffff8800[MassDeleteEmails]|r No players online.");
            return true;
        }

        std::string const inList = BuildInList(onlineReceivers);
        uint32 const mailCount = CountMailsForReceivers(inList);
        uint32 const mailItemCount = CountMailItemsForReceivers(inList);

        uint32 playersCleared = 0;
        for (auto const& playerRef : ObjectAccessor::GetPlayers())
        {
            Player* player = playerRef.second;
            if (!player || !player->IsInWorld())
                continue;

            if (player->GetMailSize())
                ++playersCleared;

            ClearOnlinePlayerMail(player);
        }

        DeleteMailFromDatabaseForReceivers(inList);

        handler->PSendSysMessage(
            "|cffff0000[MassDeleteEmails]|r Online wipe: {} mail(s), {} mail attachment row(s) "
            "for {} online character(s). Items were destroyed.",
            mailCount, mailItemCount, onlineReceivers.size());

        LOG_WARN("module", "MassDeleteEmailsOnline: {} wiped mail for {} online character(s) ({} mails, {} mail_items).",
            GetActorName(handler), onlineReceivers.size(), mailCount, mailItemCount);

        return true;
    }

    static bool HandleMassDeleteEmailsAll(ChatHandler* handler, char const* /*args*/)
    {
        uint32 mailCount = 0;
        if (QueryResult mailCountResult = CharacterDatabase.Query("SELECT COUNT(*) FROM mail"))
            mailCount = (*mailCountResult)[0].Get<uint32>();

        uint32 mailItemCount = 0;
        if (QueryResult mailItemCountResult = CharacterDatabase.Query("SELECT COUNT(*) FROM mail_items"))
            mailItemCount = (*mailItemCountResult)[0].Get<uint32>();

        std::unordered_set<uint32> const receivers = CollectAllMailReceivers();

        uint32 onlineCleared = 0;
        for (auto const& playerRef : ObjectAccessor::GetPlayers())
        {
            Player* player = playerRef.second;
            if (!player || !player->IsInWorld())
                continue;

            if (player->GetMailSize())
                ++onlineCleared;

            ClearOnlinePlayerMail(player);
        }

        DeleteAllMailFromDatabase();
        ResetMailCountInCache(receivers);

        handler->PSendSysMessage(
            "|cffff0000[MassDeleteEmails]|r Server-wide wipe: {} mail(s), {} mail attachment row(s). "
            "Cleared in-memory mail for {} online player(s). Offline mail removed from DB. Items destroyed.",
            mailCount, mailItemCount, onlineCleared);

        LOG_WARN("module", "MassDeleteEmailsAll: {} deleted all server mail ({} mails, {} mail_items, {} receivers).",
            GetActorName(handler), mailCount, mailItemCount, receivers.size());

        return true;
    }

    static bool HandleMailArriveNow(ChatHandler* handler, char const* /*args*/)
    {
        time_t const now = GameTime::GetGameTime().count();

        uint32 pendingCount = 0;
        if (QueryResult pendingResult = CharacterDatabase.Query("SELECT COUNT(*) FROM mail WHERE deliver_time > {}", uint32(now)))
            pendingCount = (*pendingResult)[0].Get<uint32>();

        // #region agent log
        {
            std::ostringstream data;
            data << "{\"pendingCount\":" << pendingCount << ",\"now\":" << uint32(now) << "}";
            AgentDebugLog("MassDeleteEmails.cpp:HandleMailArriveNow", "pending mail before update", "H1", data.str());
        }
        // #endregion

        if (pendingCount == 0)
        {
            handler->SendSysMessage("|cffff8800[MailArriveNow]|r No pending mail on the server.");
            return true;
        }

        CharacterDatabase.Execute(
            "UPDATE mail SET deliver_time = {} WHERE deliver_time > {}",
            uint32(now), uint32(now));

        uint32 onlinePlayersNotified = 0;
        uint32 onlineMailLoaded = 0;

        for (auto const& playerRef : ObjectAccessor::GetPlayers())
        {
            Player* player = playerRef.second;
            if (!player || !player->IsInWorld())
                continue;

            uint32 const loadedForPlayer = SyncDeliveredMailForOnlinePlayer(player, now);
            onlineMailLoaded += loadedForPlayer;

            player->UpdateNextMailTimeAndUnreads();
            player->SendNewMail();
            ++onlinePlayersNotified;
        }

        // #region agent log
        {
            std::ostringstream data;
            data << "{\"pendingCount\":" << pendingCount
                 << ",\"onlinePlayersNotified\":" << onlinePlayersNotified
                 << ",\"onlineMailLoaded\":" << onlineMailLoaded << "}";
            AgentDebugLog("MassDeleteEmails.cpp:HandleMailArriveNow", "mail delivery forced", "H2", data.str());
        }
        // #endregion

        handler->PSendSysMessage(
            "|cff00ff00[MailArriveNow]|r Delivered {} pending mail(s) server-wide. "
            "Notified {} online player(s), loaded {} new mail(s) into online memory.",
            pendingCount, onlinePlayersNotified, onlineMailLoaded);

        LOG_WARN("module", "MailArriveNow: {} forced delivery of {} pending mail(s) ({} online notified, {} loaded).",
            GetActorName(handler), pendingCount, onlinePlayersNotified, onlineMailLoaded);

        return true;
    }
};

void AddSC_MassDeleteEmails()
{
    new MassDeleteEmails_Command();
}
