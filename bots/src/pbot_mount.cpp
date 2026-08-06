/*
 * Companion Bots — riding implementation. See pbot_mount.h.
 */

#include "pbot_mount.h"

#include "DB2Stores.h"
#include "Player.h"
#include "Random.h"

#include <vector>

namespace
{
    // Ground-mount speed. Not the exact retail figure for any particular mount — the point is that
    // a mounted character does not travel at footspeed, and picking one honest number beats
    // pretending to model riding skill the bots never trained.
    constexpr float MOUNTED_SPEED_RATE = 1.9f;

    // Display ids of mounts that actually have a model, built once from client data.
    std::vector<uint32> const& MountDisplays()
    {
        static std::vector<uint32> displays;
        static bool loaded = false;

        if (loaded)
            return displays;
        loaded = true;

        for (MountEntry const* mount : sMountStore)
        {
            if (!mount)
                continue;

            DB2Manager::MountXDisplayContainer const* mountDisplays = sDB2Manager.GetMountDisplays(mount->ID);
            if (!mountDisplays)
                continue;

            for (MountXDisplayEntry const* display : *mountDisplays)
                if (display && display->CreatureDisplayInfoID)
                    displays.push_back(display->CreatureDisplayInfoID);
        }

        return displays;
    }
}

void PbotMount::MountUp(Player* bot)
{
    if (!bot || !bot->IsInWorld() || bot->IsInCombat() || bot->IsMounted() || !bot->IsAlive())
        return;

    std::vector<uint32> const& displays = MountDisplays();
    if (displays.empty())
        return;

    bot->Mount(displays[urand(0, uint32(displays.size()) - 1)]);
    bot->SetSpeedRate(MOVE_RUN, MOUNTED_SPEED_RATE);
}

void PbotMount::Dismount(Player* bot)
{
    if (!bot || !bot->IsMounted())
        return;

    bot->Dismount();
    bot->SetSpeedRate(MOVE_RUN, 1.0f);
}
