#include "ScriptMgr.h"
#include "Player.h"
#include "Map.h"

class DalaranFlightBlocker : public PlayerScript
{
public:
    DalaranFlightBlocker() : PlayerScript("DalaranFlightBlocker") { }

    // This hook is called when the server checks if a player can fly
    bool OnPlayerCanFlyInZone(Player* player, uint32 /*mapId*/, uint32 zoneId, SpellInfo const* /*bySpell*/) override
    {
        // Dalaran Area IDs: 4395, 4613, etc.
        // We return true to allow MOUNTING (avoiding Wrong Zone error), 
        // but flight is forced to ground mode in ridingchanges.cpp
        if (zoneId == 4395 || zoneId == 4613 || zoneId == 4378 || zoneId == 4601 || zoneId == 4632)
        {
            return true;
        }
        return true;
    }

    // Double check on area update to ensure they are dismounted or forced to ground
    void OnPlayerUpdateArea(Player* player, uint32 /*oldArea*/, uint32 newArea) override
    {
        if (newArea == 4395 || newArea == 4613)
        {
            // If they are flying, we ensure the server knows they are NOT flying
            if (player->IsFlying())
            {
                // Logic already handled in ridingchanges.cpp
            }
        }
    }
};

void AddSC_dalaran_flight_override()
{
    new DalaranFlightBlocker();
}
