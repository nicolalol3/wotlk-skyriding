/*
 * Starter flying mount (lvl 4+).
 * Preview: GossipDef appends item 25474/25470 to quest reward packets.
 * Grant: learn spell 32243/32235 on quest complete — never create the item.
 * Gate: character/account already has the faction mount spell.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "mod-account-bound/src/AccountBoundMount.h"

namespace
{
    uint32 GetStarterMountSpell(TeamId team)
    {
        return team == TEAM_HORDE ? 32243u : 32235u;
    }

    bool AccountNeedsStarterFlyingMount(Player const* player)
    {
        if (!player || !player->GetSession() || player->GetLevel() < 4)
            return false;

        uint32 const spellId = GetStarterMountSpell(player->GetTeamId());
        if (player->HasSpell(spellId))
            return false;

        uint32 const accountId = player->GetSession()->GetAccountId();
        if (AccountBoundMount::AccountHasMount(accountId, spellId))
            return false;

        return true;
    }
}

class StarterFlyingMount_PlayerScript : public PlayerScript
{
public:
    StarterFlyingMount_PlayerScript()
        : PlayerScript("StarterFlyingMount_PlayerScript",
            { PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST })
    {
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* /*quest*/) override
    {
        if (!AccountNeedsStarterFlyingMount(player))
            return;

        player->learnSpell(GetStarterMountSpell(player->GetTeamId()), false);
        // AccountBoundMount::OnPlayerLearnSpell → mod_account_mount + alts
    }
};

void AddSC_StarterFlyingMount()
{
    new StarterFlyingMount_PlayerScript();
}
