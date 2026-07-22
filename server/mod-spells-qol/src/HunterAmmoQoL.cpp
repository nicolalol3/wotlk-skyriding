#include "ScriptMgr.h"
#include "Player.h"
#include "Spell.h"
#include <unordered_map>

class HunterAmmoQoL_Spell : public AllSpellScript
{
public:
    HunterAmmoQoL_Spell() : AllSpellScript("HunterAmmoQoL_Spell") { }

    // Map to store the next time ammo should be consumed
    static std::unordered_map<ObjectGuid, time_t> _cooldowns;

    bool OnBeforeTakeAmmo(Spell* spell) override
    {
        Unit* caster = spell->GetCaster();
        if (!caster || !caster->IsPlayer())
            return false;

        Player* player = caster->ToPlayer();
        if (player->getClass() != CLASS_HUNTER)
            return false;

        ObjectGuid guid = player->GetGUID();
        time_t now = time(nullptr);

        auto it = _cooldowns.find(guid);
        if (it != _cooldowns.end() && it->second > now)
        {
            // Cooldown active, skip ammo removal
            return true;
        }

        // No cooldown or expired, consume ammo and set next cooldown to 45 seconds
        _cooldowns[guid] = now + 45;
        return false;
    }
};

std::unordered_map<ObjectGuid, time_t> HunterAmmoQoL_Spell::_cooldowns;

class HunterAmmoQoL_Player : public PlayerScript
{
public:
    HunterAmmoQoL_Player() : PlayerScript("HunterAmmoQoL_Player") { }

    void OnPlayerLogout(Player* player) override
    {
        // Cleanup memory on logout
        HunterAmmoQoL_Spell::_cooldowns.erase(player->GetGUID());
    }
};

void AddSCHunterAmmoQoL()
{
    new HunterAmmoQoL_Spell();
    new HunterAmmoQoL_Player();
}
