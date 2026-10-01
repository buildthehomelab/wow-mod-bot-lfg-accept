/*
 * mod-bot-lfg-accept
 *
 * Playerbots always accept the Dungeon Finder ready check. Out of the box, mod-playerbots declines
 * it whenever a bot is in combat or dead at the moment the dungeon pops, and one decline cancels
 * the dungeon for the whole group. With bots there is no reason to say no.
 *
 * How:
 *   1. When a bot is sent a proposal, the proposal ID in that packet is set to 0 before
 *      mod-playerbots reads it, so the bot's own accept/decline logic does nothing. This only
 *      works because this module loads before mod-playerbots (modules load in folder name order,
 *      and "mod-bot-lfg-accept" sorts first).
 *   2. On the next world update the module accepts the proposal for the bot.
 *   3. If the group forms but a bot can't be teleported in (dead, in combat, in a vehicle), it is
 *      revived, taken out of combat and teleported in. Retries once a second for two minutes.
 *
 * Real players are never touched. Needs the playerbots core fork (PlayerbotScript,
 * WorldSession::IsBot()).
 *
 * Released under the MIT License.
 */

#include "ByteBuffer.h"
#include "Config.h"
#include "GameTime.h"
#include "Group.h"
#include "LFGMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    struct Config
    {
        bool enabled = true;
        bool pullIn = true;
    };

    Config config;

    // SMSG_LFG_PROPOSAL_UPDATE starts with uint32 dungeon entry, uint8 state, uint32 proposal ID.
    constexpr std::size_t PROPOSAL_ID_POS = 4 + 1;

    // How long after accepting a failed teleport still gets the bot pulled in.
    constexpr time_t PULL_IN_WINDOW = 120;
    constexpr uint32 PULL_IN_INTERVAL = 1000;

    std::mutex lock;
    std::vector<std::pair<ObjectGuid, uint32>> pendingAccepts;  // bot, proposal ID
    std::unordered_map<ObjectGuid, time_t> acceptedAt;
    std::unordered_set<ObjectGuid> pullIns;

    // The playerbots fork adds WorldSession::IsBot(); stock AzerothCore doesn't have it.
    template <typename Session, typename = void>
    struct HasIsBot : std::false_type { };

    template <typename Session>
    struct HasIsBot<Session, std::void_t<decltype(std::declval<Session&>().IsBot())>> : std::true_type { };

    template <typename Session>
    bool IsBotSession(Session* session)
    {
        if constexpr (HasIsBot<Session>::value)
            return session->IsBot();
        else
            return false;
    }

    void OnProposal(Player* bot, WorldPacket& packet)
    {
        uint32 proposalId = 0;
        uint8 state = 0;
        bool answered = false;

        try
        {
            WorldPacket p(packet);
            p.rpos(0);

            uint32 dungeonEntry, encounters;
            uint8 silent, count;
            p >> dungeonEntry >> state >> proposalId >> encounters >> silent >> count;

            for (uint8 i = 0; i < count; ++i)
            {
                uint32 role;
                uint8 self, inDungeon, sameGroup, hasAnswered, accepted;
                p >> role >> self >> inDungeon >> sameGroup >> hasAnswered >> accepted;
                if (self)
                    answered = hasAnswered;
            }
        }
        catch (ByteBufferException const&)
        {
            return;
        }

        if (!proposalId)
            return;

        // Hide the proposal from mod-playerbots, which would decline it while the bot is in combat
        // or dead. Its accept action ignores proposal ID 0. The packet was built just for this bot.
        packet.put<uint32>(PROPOSAL_ID_POS, 0);

        if (state != lfg::LFG_PROPOSAL_INITIATING || answered)
            return;

        std::lock_guard<std::mutex> guard(lock);
        pendingAccepts.emplace_back(bot->GetGUID(), proposalId);
    }

    void OnTeleportDenied(Player* bot)
    {
        if (!config.pullIn)
            return;

        std::lock_guard<std::mutex> guard(lock);

        auto const itr = acceptedAt.find(bot->GetGUID());
        if (itr != acceptedAt.end() && GameTime::GetGameTime().count() - itr->second < PULL_IN_WINDOW)
            pullIns.insert(bot->GetGUID());
    }

    void AcceptPending()
    {
        std::vector<std::pair<ObjectGuid, uint32>> accepts;
        {
            std::lock_guard<std::mutex> guard(lock);
            if (pendingAccepts.empty())
                return;

            accepts.swap(pendingAccepts);

            time_t const now = GameTime::GetGameTime().count();
            for (auto const& [guid, proposalId] : accepts)
                acceptedAt[guid] = now;
        }

        // Without the lock: accepting can complete the proposal, which teleports everyone and may
        // send this bot SMSG_LFG_TELEPORT_DENIED straight back through OnTeleportDenied.
        for (auto const& [guid, proposalId] : accepts)
            sLFGMgr->UpdateProposal(proposalId, guid, true);
    }

    // Returns false once the bot no longer needs pulling in.
    bool PullIn(ObjectGuid guid)
    {
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (!bot || !bot->IsInWorld())
            return false;

        if (bot->IsBeingTeleported())
            return true;

        Group* group = bot->GetGroup();
        if (!group || !group->isLFGGroup() || sLFGMgr->GetState(group->GetGUID()) != lfg::LFG_STATE_DUNGEON)
            return false;

        uint32 const mapId = sLFGMgr->GetDungeonMapId(group->GetGUID());
        if (!mapId || bot->GetMapId() == mapId)
            return false;

        if (bot->IsFalling() || bot->HasUnitState(UNIT_STATE_JUMPING))
            return true;

        if (!bot->IsAlive())
        {
            bot->ResurrectPlayer(1.0f);
            bot->SpawnCorpseBones();
        }

        if (bot->GetVehicle())
            bot->ExitVehicle();

        if (bot->IsInCombat())
            bot->CombatStopWithPets(true);

        LOG_DEBUG("module", "mod-bot-lfg-accept: pulling {} into the dungeon", bot->GetName());
        sLFGMgr->TeleportPlayer(bot, false);
        return true;
    }

    void PullInBots()
    {
        std::vector<ObjectGuid> bots;
        time_t const now = GameTime::GetGameTime().count();
        {
            std::lock_guard<std::mutex> guard(lock);

            for (auto itr = acceptedAt.begin(); itr != acceptedAt.end();)
            {
                if (now - itr->second >= PULL_IN_WINDOW)
                {
                    pullIns.erase(itr->first);
                    itr = acceptedAt.erase(itr);
                }
                else
                    ++itr;
            }

            bots.assign(pullIns.begin(), pullIns.end());
        }

        for (ObjectGuid const& guid : bots)
        {
            if (PullIn(guid))
                continue;

            std::lock_guard<std::mutex> guard(lock);
            pullIns.erase(guid);
        }
    }
}

class BotLfgAcceptWorldScript : public WorldScript
{
public:
    BotLfgAcceptWorldScript() : WorldScript("BotLfgAcceptWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        config.enabled = sConfigMgr->GetOption<bool>("BotLfgAccept.Enable", true);
        config.pullIn = sConfigMgr->GetOption<bool>("BotLfgAccept.PullIn", true);

        LOG_INFO("server.loading", "mod-bot-lfg-accept: {}, pull-in {}", config.enabled ? "enabled" : "disabled",
            config.pullIn ? "on" : "off");
    }

    void OnUpdate(uint32 diff) override
    {
        if (!config.enabled)
            return;

        AcceptPending();

        pullInTimer += diff;
        if (pullInTimer < PULL_IN_INTERVAL)
            return;

        pullInTimer = 0;
        PullInBots();
    }

private:
    uint32 pullInTimer = 0;
};

class BotLfgAcceptPlayerbotScript : public PlayerbotScript
{
public:
    BotLfgAcceptPlayerbotScript() : PlayerbotScript("BotLfgAcceptPlayerbotScript") { }

    // Runs for every packet sent to every player, so bail out on the opcode first.
    void OnPlayerbotPacketSent(Player* player, WorldPacket const* packet) override
    {
        if (!config.enabled || !player || !packet)
            return;

        uint16 const opcode = packet->GetOpcode();
        if (opcode != SMSG_LFG_PROPOSAL_UPDATE && opcode != SMSG_LFG_TELEPORT_DENIED)
            return;

        if (!IsBotSession(player->GetSession()))
            return;

        if (opcode == SMSG_LFG_PROPOSAL_UPDATE)
            OnProposal(player, const_cast<WorldPacket&>(*packet));  // a local in WorldSession, not const
        else
            OnTeleportDenied(player);
    }
};

void AddBotLfgAcceptScripts()
{
    new BotLfgAcceptWorldScript();
    new BotLfgAcceptPlayerbotScript();
}
