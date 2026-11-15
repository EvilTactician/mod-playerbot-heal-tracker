/*
 * mod-playerbot-heal-tracker
 *
 * Sends the heals cast by playerbots to players' clients in the LibHealComm-4.0 format, so addons that show incoming
 * heals (Improved Raid Frames, DragonUI, ...) can show playerbot heals too. Playerbots have no client of their own, so
 * without this their heals never show.
 *
 * Released under the GNU General Public License v2, see LICENSE.
 */

#ifndef MOD_PLAYERBOT_HEAL_TRACKER_H
#define MOD_PLAYERBOT_HEAL_TRACKER_H

#include "ObjectGuid.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

class Aura;
class Player;
class Spell;
class Unit;

// Version of this module, reported to the client addon
#define PLAYERBOT_HEAL_TRACKER_VERSION "1.0.0"

// Version of the client <-> server handshake (must match Improved Raid Frames' BotHeals.lua)
#define PLAYERBOT_HEAL_TRACKER_PROTOCOL 1

// Addon message prefix of the handshake
#define PLAYERBOT_HEAL_TRACKER_PREFIX "IRFBH"

// Addon message prefix of LibHealComm-4.0
#define LIBHEALCOMM_PREFIX "LHC40"

class PlayerbotHealTracker
{
public:
    static PlayerbotHealTracker* instance();

    // Configuration
    void LoadConfig();
    bool IsEnabled() const { return _enabled; }

    // Handshake: players who asked for playerbot heal data
    void HandleAddonMessage(Player* player, std::string const& message);
    void Unsubscribe(ObjectGuid guid);
    bool HasSubscribers() const { return _subscriberCount.load(std::memory_order_relaxed) > 0; }

    // Spell and aura events
    void OnCastStart(Spell* spell, Unit* caster);
    void OnCastEnd(Spell* spell, Unit* caster, bool interrupted);
    void OnHotApplied(Unit* target, Aura* aura);
    void OnHotRemoved(Unit* target, Aura* aura);

    // Sends queued messages whose delay has passed
    void Update(uint32 diff);

    // Whether a player is a playerbot (has playerbot AI)
    static bool IsPlayerbot(Player* player);

private:
    PlayerbotHealTracker() = default;

    struct QueuedMessage
    {
        ObjectGuid caster;
        std::string payload;
        int32 delay;
    };

    // Send a LibHealComm-4.0 message from a playerbot to the subscribed players in its group
    void SendFromBot(Player* bot, std::string const& payload);
    void QueueFromBot(Player* bot, std::string const& payload);
    void SendHandshakeReply(Player* player);

    bool IsSubscribed(ObjectGuid guid);

    // Settings
    bool _enabled = true;
    bool _directHeals = true;
    bool _hots = true;
    int32 _hotDelay = 250;
    bool _debug = false;

    // Subscribers, accessed from map threads and the world thread
    std::mutex _subscriberLock;
    std::unordered_set<ObjectGuid> _subscribers;
    std::atomic<size_t> _subscriberCount{0};

    // Heal casts in progress per bot (bot GUID -> spell ID), so we only send "stopped" for casts we announced
    std::mutex _castLock;
    std::unordered_map<ObjectGuid, uint32> _castsInProgress;

    // HoT messages wait briefly, so the client already knows the aura when the message arrives
    std::mutex _queueLock;
    std::deque<QueuedMessage> _queue;
};

#define sPlayerbotHealTracker PlayerbotHealTracker::instance()

#endif
