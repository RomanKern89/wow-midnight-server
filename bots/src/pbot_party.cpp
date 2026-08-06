/*
 * Companion Bots — Phase 5 party integration implementation.
 * See pbot_party.h for why grouping and level-matching are prerequisites, not conveniences.
 */

#include "pbot_party.h"

#include "pbot_equip.h"   // gear the bot for the level it was just raised to

#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Player.h"

bool PbotParty::JoinOwnerGroup(Player* owner, Player* bot)
{
    if (!owner || !bot)
        return false;

    if (Group* current = bot->GetGroup())
    {
        // Already grouped with the owner (typical on reload, where the bot's saved group is loaded
        // back by the login query holder before we get here) — nothing to do.
        if (current == owner->GetGroup())
            return true;

        // Grouped with someone else: leave first, otherwise AddMember below would be refused.
        PbotParty::LeaveGroup(bot);
    }

    Group* group = owner->GetGroup();
    if (!group)
    {
        // Same sequence the engine uses when a party is formed from an accepted invite
        // (GroupHandler.cpp:229-232): construct, Create() with the leader, then register with
        // GroupMgr so the group gets an id and is persisted/broadcast like any other.
        group = new Group();
        if (!group->Create(owner))
        {
            delete group;
            TC_LOG_ERROR("scripts.bots", "PbotParty: could not create a group for owner {}.",
                owner->GetGUID().ToString());
            return false;
        }
        sGroupMgr->AddGroup(group);
    }
    else if (group->IsFull())
    {
        // The owner is already partied with real players. Not an error — the bot simply fights
        // without shared experience until a slot frees up.
        TC_LOG_INFO("scripts.bots", "PbotParty: owner {} group is full; bot {} stays ungrouped.",
            owner->GetGUID().ToString(), bot->GetGUID().ToString());
        return false;
    }

    // NB: AddMember is what sets the player's group pointer (see the comment at GroupHandler.cpp:234).
    if (!group->AddMember(bot))
    {
        TC_LOG_ERROR("scripts.bots", "PbotParty: AddMember refused bot {} for owner {}.",
            bot->GetGUID().ToString(), owner->GetGUID().ToString());
        return false;
    }

    group->BroadcastGroupUpdate();
    return true;
}

void PbotParty::LeaveGroup(Player* bot)
{
    if (!bot)
        return;

    Group* group = bot->GetGroup();
    if (!group)
        return;

    // RemoveMember disbands the group by itself when too few members remain, so there is nothing
    // extra to clean up here.
    group->RemoveMember(bot->GetGUID());
}

void PbotParty::SyncLevelToOwner(Player* owner, Player* bot)
{
    if (!owner || !bot)
        return;

    uint8 const target = owner->GetLevel();

    // Only ever raise (see header): a bot that is already at or above the owner's level keeps what
    // it earned.
    if (target > bot->GetLevel())
    {
        // GiveLevel is the full path — it applies the level's stats/power, updates the character
        // fields and fires the same level-up bookkeeping a real character gets. Setting the level
        // field directly would leave the bot with level-1 health and mana at level 70.
        bot->GiveLevel(target);

        // GiveLevel awards the level but not the curriculum. Without these the bot arrives at the
        // owner's level knowing only its level-1 starter kit, so every rotation step above the
        // first would be a spell it does not have.
        bot->LearnDefaultSkills();
        bot->LearnSpecializationSpells();
        bot->InitTalentForLevel();

        // Gear for the NEW level. A companion spawned next to a level-70 owner is levelled to 70
        // but would otherwise still be wearing the level-1 starter kit.
        PbotEquip::GearUp(bot);

        // Arrive ready to fight rather than at the fraction of health the level-up left behind.
        bot->SetFullHealth();
        bot->SetFullPower(bot->GetPowerType());
    }
}
