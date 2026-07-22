/*
 * .summtest on|off — skips Ritual of Summoning bubble clicks after 0.5s.
 */

#include "AllGameObjectScript.h"
#include "Chat.h"
#include "CommandScript.h"
#include "GameObject.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include <chrono>
#include <unordered_set>

using namespace Acore::ChatCommands;

namespace
{
    constexpr uint32 SPELL_RITUAL_OF_SUMMONING    = 698;
    constexpr uint32 SPELL_RITUAL_SUMMON_CLOSET   = 61993;
    constexpr uint32 SPELL_SUMMONING_STONE_EFFECT = 59782;
    constexpr uint32 SPELL_RITUAL_SUMMON_EXECUTE  = 7720;

    constexpr uint32 GO_SUMMONING_PORTAL_PHASE2 = 179944;

    constexpr Milliseconds SUMMTEST_ADVANCE_DELAY = 500ms;

    enum SummTestStep : uint8
    {
        STEP_PORTAL_PHASE1 = 1,
        STEP_PORTAL_PHASE2 = 2,
    };

    static std::unordered_set<ObjectGuid> s_summTestEnabled;
    static std::unordered_set<uint64> s_scheduledCastAdvances;
    static std::unordered_set<ObjectGuid> s_scheduledGoGuids;
    static ObjectGuid s_summTestRitualInitiator;

    class SummTestAdvanceEvent;

    static bool IsSummTestEnabled(Player const* player)
    {
        return player && s_summTestEnabled.count(player->GetGUID()) != 0;
    }

    static uint64 MakeCastAdvanceKey(ObjectGuid const& playerGuid, SummTestStep step)
    {
        return (playerGuid.GetRawValue() << 8) | uint8(step);
    }

    static uint32 OwnerSpellIdForStep(SummTestStep step)
    {
        switch (step)
        {
            case STEP_PORTAL_PHASE1: return SPELL_RITUAL_OF_SUMMONING;
            case STEP_PORTAL_PHASE2: return SPELL_SUMMONING_STONE_EFFECT;
            default:                 return 0;
        }
    }

    static void DespawnOwnerGameObject(Player* caster, SummTestStep step)
    {
        if (!caster)
            return;

        if (GameObject* go = caster->GetGameObject(OwnerSpellIdForStep(step)))
            go->SetLootState(GO_JUST_DEACTIVATED);
    }

    static void FinishChannel(Player* caster)
    {
        if (!caster)
            return;

        if (Spell* spell = caster->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        {
            spell->SendChannelUpdate(0);
            spell->finish(false);
        }
    }

    static Player* ResolveSummonTarget(Player* caster)
    {
        if (!caster)
            return nullptr;

        if (Player* target = ObjectAccessor::FindPlayer(caster->GetTarget()))
        {
            if (target != caster)
                return target;
        }

        return nullptr;
    }

    static Player* ResolveSummTestCaster(GameObject* go)
    {
        if (!go)
            return nullptr;

        if (!s_summTestRitualInitiator.IsEmpty())
        {
            if (Player* initiator =
                ObjectAccessor::FindPlayer(s_summTestRitualInitiator))
            {
                if (IsSummTestEnabled(initiator)
                    && go->IsWithinDistInMap(initiator, 40.0f))
                    return initiator;
            }
        }

        if (Unit* owner = go->GetOwner())
        {
            if (Player* player = owner->ToPlayer())
            {
                if (IsSummTestEnabled(player))
                    return player;
            }
        }

        return nullptr;
    }

    static void AdvanceSummTestStep(Player* caster, SummTestStep step)
    {
        if (!caster || !IsSummTestEnabled(caster) || !step)
            return;

        switch (step)
        {
            case STEP_PORTAL_PHASE1:
                DespawnOwnerGameObject(caster, STEP_PORTAL_PHASE1);
                FinishChannel(caster);
                caster->CastSpell(caster, SPELL_RITUAL_SUMMON_CLOSET,
                    TRIGGERED_FULL_MASK);
                break;
            case STEP_PORTAL_PHASE2:
                DespawnOwnerGameObject(caster, STEP_PORTAL_PHASE2);
                FinishChannel(caster);
                if (Player* target = ResolveSummonTarget(caster))
                {
                    caster->CastSpell(target, SPELL_RITUAL_SUMMON_EXECUTE,
                        TRIGGERED_FULL_MASK);
                }
                else
                {
                    caster->CastSpell(caster, SPELL_RITUAL_SUMMON_EXECUTE,
                        TRIGGERED_FULL_MASK);
                }
                break;
            default:
                break;
        }
    }

    class SummTestAdvanceEvent : public BasicEvent
    {
    public:
        SummTestAdvanceEvent(ObjectGuid playerGuid, SummTestStep step,
            ObjectGuid goGuid = ObjectGuid::Empty)
            : _playerGuid(playerGuid)
            , _step(step)
            , _goGuid(goGuid)
        {
        }

        bool Execute(uint64 /*time*/, uint32 /*diff*/) override
        {
            if (_goGuid.IsEmpty())
                s_scheduledCastAdvances.erase(
                    MakeCastAdvanceKey(_playerGuid, _step));
            else
                s_scheduledGoGuids.erase(_goGuid);

            Player* player = ObjectAccessor::FindPlayer(_playerGuid);
            if (!player)
                return true;

            AdvanceSummTestStep(player, _step);
            return true;
        }

    private:
        ObjectGuid _playerGuid;
        SummTestStep _step;
        ObjectGuid _goGuid;
    };

    static void ScheduleCastAdvance(Player* caster, SummTestStep step)
    {
        if (!caster || !IsSummTestEnabled(caster) || !step)
            return;

        uint64 const key = MakeCastAdvanceKey(caster->GetGUID(), step);
        if (!s_scheduledCastAdvances.insert(key).second)
            return;

        caster->m_Events.AddEventAtOffset(
            new SummTestAdvanceEvent(caster->GetGUID(), step),
            SUMMTEST_ADVANCE_DELAY);
    }

    static void ScheduleGameObjectAdvance(GameObject* go, Player* caster,
        SummTestStep step)
    {
        if (!go || !caster || !IsSummTestEnabled(caster) || !step)
            return;

        if (!s_scheduledGoGuids.insert(go->GetGUID()).second)
            return;

        caster->m_Events.AddEventAtOffset(
            new SummTestAdvanceEvent(caster->GetGUID(), step, go->GetGUID()),
            SUMMTEST_ADVANCE_DELAY);
    }
}

class SummTest_AllGameObjectScript : public AllGameObjectScript
{
public:
    SummTest_AllGameObjectScript()
        : AllGameObjectScript("SummTest_AllGameObjectScript") { }

    void OnGameObjectAddWorld(GameObject* go) override
    {
        if (!go || go->GetEntry() != GO_SUMMONING_PORTAL_PHASE2)
            return;

        Player* caster = ResolveSummTestCaster(go);
        if (!caster || !IsSummTestEnabled(caster))
            return;

        ScheduleGameObjectAdvance(go, caster, STEP_PORTAL_PHASE2);
    }
};

class SummTest_PlayerScript : public PlayerScript
{
public:
    SummTest_PlayerScript() : PlayerScript("SummTest_PlayerScript",
        { PLAYERHOOK_ON_SPELL_CAST, PLAYERHOOK_ON_LOGOUT }) { }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!player || !spell || !IsSummTestEnabled(player))
            return;

        if (spell->GetSpellInfo()->Id != SPELL_RITUAL_OF_SUMMONING)
            return;

        s_summTestRitualInitiator = player->GetGUID();
        ScheduleCastAdvance(player, STEP_PORTAL_PHASE1);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;

        s_summTestEnabled.erase(player->GetGUID());

        if (s_summTestRitualInitiator == player->GetGUID())
            s_summTestRitualInitiator.Clear();
    }
};

class SummTest_CommandScript : public CommandScript
{
public:
    SummTest_CommandScript() : CommandScript("SummTest_CommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable summtestCommandTable =
        {
            { "on",  HandleSummTestOnCommand,  SEC_PLAYER, Console::No },
            { "off", HandleSummTestOffCommand, SEC_PLAYER, Console::No },
        };

        static ChatCommandTable commandTable =
        {
            { "summtest", summtestCommandTable },
        };

        return commandTable;
    }

    static bool HandleSummTestOnCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return true;

        s_summTestEnabled.insert(player->GetGUID());
        handler->SendSysMessage(
            "|cff00ff00Summ test enabled.|r Ritual of Summoning click bubbles "
            "auto-advance after 0.5s. Click the closet manually.");
        return true;
    }

    static bool HandleSummTestOffCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return true;

        s_summTestEnabled.erase(player->GetGUID());

        if (s_summTestRitualInitiator == player->GetGUID())
            s_summTestRitualInitiator.Clear();

        handler->SendSysMessage("|cffffaa00Summ test disabled.|r");
        return true;
    }
};

void AddSC_SummTest()
{
    new SummTest_AllGameObjectScript();
    new SummTest_PlayerScript();
    new SummTest_CommandScript();
}
