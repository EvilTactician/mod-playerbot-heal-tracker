/*
 * mod-playerbot-heal-tracker
 *
 * Released under the GNU General Public License v2, see LICENSE.
 */

#include "PlayerbotHealTracker.h"

#include "Chat.h"
#include "Config.h"
#include "Group.h"
#include "GroupReference.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "Unit.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#ifdef MOD_PLAYERBOTS
#include "PlayerbotMgr.h"
#endif

#include <algorithm>
#include <cstdio>
#include <vector>

namespace
{
    // LibHealComm-4.0 sends GUIDs as 8 raw bytes (most significant first). Bytes that would break the message are
    // escaped exactly as LibHealComm-4.0's compressGUID does: 0 -> \255, and , : @ | \254 \255 -> \254 + code.
    std::string CompressGUID(ObjectGuid guid)
    {
        uint64 raw = guid.GetRawValue();
        std::string out;
        out.reserve(16);

        for (int shift = 56; shift >= 0; shift -= 8)
        {
            uint8 byte = uint8((raw >> shift) & 0xFF);
            switch (byte)
            {
                case 0:   out += char(0xFF); break;
                case 44:  out += char(0xFE); out += char(0xFC); break; // ,
                case 58:  out += char(0xFE); out += char(0xFA); break; // :
                case 64:  out += char(0xFE); out += char(0xFB); break; // @
                case 124: out += char(0xFE); out += char(0xF8); break; // |
                case 254: out += char(0xFE); out += char(0xF9); break;
                case 255: out += char(0xFE); out += char(0xFD); break;
                default:  out += char(byte); break;
            }
        }

        return out;
    }

    bool IsSingleTarget(uint32 target)
    {
        switch (target)
        {
            case TARGET_UNIT_TARGET_ALLY:
            case TARGET_UNIT_TARGET_ANY:
            case TARGET_UNIT_TARGET_PARTY:
            case TARGET_UNIT_TARGET_RAID:
            case TARGET_UNIT_TARGET_CHAINHEAL_ALLY: // only the first target is known before the heal lands
                return true;
            default:
                return false;
        }
    }

    bool IsAreaTarget(uint32 target)
    {
        switch (target)
        {
            case TARGET_UNIT_CASTER_AREA_PARTY:
            case TARGET_UNIT_LASTTARGET_AREA_PARTY:
            case TARGET_UNIT_CASTER_AREA_RAID:
            case TARGET_UNIT_TARGET_AREA_RAID_CLASS:
                return true;
            default:
                return false;
        }
    }

    // Collect the group members an area heal (e.g. Prayer of Healing) will reach
    void CollectAreaTargets(Player* caster, Unit* center, uint32 target, float radius, std::vector<Unit*>& out)
    {
        Group* group = caster->GetGroup();
        Player* centerPlayer = center ? center->ToPlayer() : nullptr;
        if (!center)
            return;

        if (!group || !centerPlayer)
        {
            out.push_back(center);
            return;
        }

        bool partyOnly = (target == TARGET_UNIT_CASTER_AREA_PARTY || target == TARGET_UNIT_LASTTARGET_AREA_PARTY);
        bool sameClass = (target == TARGET_UNIT_TARGET_AREA_RAID_CLASS);

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || !member->IsInWorld() || !member->IsAlive() || member->GetMap() != center->GetMap())
                continue;
            if (partyOnly && !group->SameSubGroup(centerPlayer, member))
                continue;
            if (sameClass && member->getClass() != centerPlayer->getClass())
                continue;
            if (!center->IsWithinDistInMap(member, radius))
                continue;
            out.push_back(member);
        }
    }
}

PlayerbotHealTracker* PlayerbotHealTracker::instance()
{
    static PlayerbotHealTracker instance;
    return &instance;
}

void PlayerbotHealTracker::LoadConfig()
{
    _enabled = sConfigMgr->GetOption<bool>("PlayerbotHealTracker.Enable", true);
    _directHeals = sConfigMgr->GetOption<bool>("PlayerbotHealTracker.DirectHeals", true);
    _hots = sConfigMgr->GetOption<bool>("PlayerbotHealTracker.HoTs", true);
    _hotDelay = std::max<int32>(0, sConfigMgr->GetOption<int32>("PlayerbotHealTracker.HoTDelay", 300));
    _debug = sConfigMgr->GetOption<bool>("PlayerbotHealTracker.Debug", false);

#ifndef MOD_PLAYERBOTS
    if (_enabled)
    {
        LOG_ERROR("module", "mod-playerbot-heal-tracker: mod-playerbots was not found when the server was built. The module is disabled.");
        _enabled = false;
    }
#endif
}

bool PlayerbotHealTracker::IsPlayerbot(Player* player)
{
#ifdef MOD_PLAYERBOTS
    return player && sPlayerbotsMgr.GetPlayerbotAI(player) != nullptr;
#else
    (void)player;
    return false;
#endif
}

/*************************************************************************
 * Handshake
 *************************************************************************/

void PlayerbotHealTracker::HandleAddonMessage(Player* player, std::string const& message)
{
    // message is "IRFBH\t<command>:<protocol>"
    std::string const prefix = std::string(PLAYERBOT_HEAL_TRACKER_PREFIX) + "\t";
    if (!player || message.compare(0, prefix.size(), prefix) != 0)
        return;

    std::string body = message.substr(prefix.size());
    std::string command = body.substr(0, body.find(':'));

    if (command == "HELLO")
    {
        if (!_enabled)
            return; // no answer: the addon reports the module as not available

        {
            std::lock_guard<std::mutex> guard(_subscriberLock);
            _subscribers.insert(player->GetGUID());
            _subscriberCount.store(_subscribers.size(), std::memory_order_relaxed);
        }

        SendHandshakeReply(player);

        if (_debug)
            LOG_INFO("module", "mod-playerbot-heal-tracker: {} now receives playerbot heals.", player->GetName());
    }
    else if (command == "BYE")
    {
        Unsubscribe(player->GetGUID());

        if (_debug)
            LOG_INFO("module", "mod-playerbot-heal-tracker: {} no longer receives playerbot heals.", player->GetName());
    }
}

void PlayerbotHealTracker::Unsubscribe(ObjectGuid guid)
{
    bool empty;
    {
        std::lock_guard<std::mutex> guard(_subscriberLock);
        _subscribers.erase(guid);
        _subscriberCount.store(_subscribers.size(), std::memory_order_relaxed);
        empty = _subscribers.empty();
    }

    // Nobody listens any more: forget casts we were tracking
    if (empty)
    {
        std::lock_guard<std::mutex> guard(_castLock);
        _castsInProgress.clear();
    }
}

bool PlayerbotHealTracker::IsSubscribed(ObjectGuid guid)
{
    std::lock_guard<std::mutex> guard(_subscriberLock);
    return _subscribers.find(guid) != _subscribers.end();
}

void PlayerbotHealTracker::SendHandshakeReply(Player* player)
{
    char payload[64];
    std::snprintf(payload, sizeof(payload), "%s\tACK:%d:%s", PLAYERBOT_HEAL_TRACKER_PREFIX, PLAYERBOT_HEAL_TRACKER_PROTOCOL,
        PLAYERBOT_HEAL_TRACKER_VERSION);

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, payload);
    player->GetSession()->SendPacket(&data);
}

/*************************************************************************
 * Sending LibHealComm-4.0 messages
 *************************************************************************/

void PlayerbotHealTracker::SendFromBot(Player* bot, std::string const& payload)
{
    if (!bot || !bot->IsInWorld())
        return;

    Group* group = bot->GetGroup();
    if (!group)
        return;

    // The receiving LibHealComm-4.0 only accepts messages on the channel it uses itself
    ChatMsg chatType = CHAT_MSG_PARTY;
    if (group->isBGGroup())
        chatType = CHAT_MSG_BATTLEGROUND;
    else if (group->isRaidGroup())
        chatType = CHAT_MSG_RAID;

    std::string message = std::string(LIBHEALCOMM_PREFIX) + "\t" + payload;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, chatType, LANG_ADDON, bot, nullptr, message);

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || member == bot || !member->IsInWorld() || !member->GetSession())
            continue;
        if (!IsSubscribed(member->GetGUID()))
            continue;

        member->GetSession()->SendPacket(&data);
    }
}

void PlayerbotHealTracker::QueueFromBot(Player* bot, std::string const& payload)
{
    if (_hotDelay <= 0)
    {
        SendFromBot(bot, payload);
        return;
    }

    std::lock_guard<std::mutex> guard(_queueLock);
    _queue.push_back({ bot->GetGUID(), payload, _hotDelay });
}

void PlayerbotHealTracker::Update(uint32 diff)
{
    std::vector<QueuedMessage> ready;
    {
        std::lock_guard<std::mutex> guard(_queueLock);
        if (_queue.empty())
            return;

        for (QueuedMessage& queued : _queue)
            queued.delay -= int32(diff);

        // Messages stay in order: send everything from the front whose delay has passed
        while (!_queue.empty() && _queue.front().delay <= 0)
        {
            ready.push_back(_queue.front());
            _queue.pop_front();
        }
    }

    for (QueuedMessage const& queued : ready)
        if (Player* bot = ObjectAccessor::FindConnectedPlayer(queued.caster))
            SendFromBot(bot, queued.payload);
}

/*************************************************************************
 * Direct heals (cast-time heals such as Flash Heal, Healing Wave, Holy Light)
 *************************************************************************/

void PlayerbotHealTracker::OnCastStart(Spell* spell, Unit* caster)
{
    // Cheap checks first: this runs for every spell anything starts casting
    if (!_enabled || !_directHeals || !HasSubscribers() || !spell || !caster || !caster->IsPlayer())
        return;

    SpellInfo const* spellInfo = spell->GetSpellInfo();
    if (!spellInfo || spellInfo->IsChanneled() || !spellInfo->HasEffect(SPELL_EFFECT_HEAL) || spell->GetCastTime() <= 0)
        return;

    Player* bot = caster->ToPlayer();
    if (!bot->GetGroup() || !IsPlayerbot(bot))
        return;

    // Work out who the heal lands on, and how much it heals before the target's own healing modifiers
    // (LibHealComm-4.0 applies those on the receiving side, e.g. Mortal Strike).
    std::vector<Unit*> targets;
    int32 amount = 0;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effect = spellInfo->Effects[i];
        if (effect.Effect != SPELL_EFFECT_HEAL)
            continue;

        uint32 targetType = effect.TargetA.GetTarget();
        Unit* unitTarget = spell->m_targets.GetUnitTarget();

        if (targets.empty())
        {
            if (targetType == TARGET_UNIT_CASTER)
                targets.push_back(caster);
            else if (IsSingleTarget(targetType) && unitTarget)
                targets.push_back(unitTarget);
            else if (IsAreaTarget(targetType))
            {
                Unit* center = (targetType == TARGET_UNIT_CASTER_AREA_PARTY || targetType == TARGET_UNIT_CASTER_AREA_RAID)
                    ? caster : (unitTarget ? unitTarget : caster);
                CollectAreaTargets(bot, center, targetType, effect.CalcRadius(caster), targets);
            }
            else if (unitTarget)
                targets.push_back(unitTarget);
        }

        if (targets.empty())
            continue;

        // Average heal instead of a random roll: the expected value of the 1..DieSides roll is (DieSides + 1) / 2
        SpellEffectInfo averaged = effect;
        int32 basePoints = spell->GetSpellValue()->EffectBasePoints[i];
        if (averaged.DieSides > 1)
        {
            basePoints += (averaged.DieSides + 1) / 2 - 1;
            averaged.DieSides = 1;
        }

        int32 value = averaged.CalcValue(caster, &basePoints, targets.front());
        if (value <= 0)
            continue;

        amount += int32(caster->SpellHealingBonusDone(targets.front(), spellInfo, uint32(value), HEAL, i));
    }

    if (targets.empty() || amount <= 0)
        return;

    // D:<castTime>:<spellID>:<amount>:<target1>,<target2>,...
    std::string targetList;
    for (Unit* target : targets)
    {
        if (!targetList.empty())
            targetList += ",";
        targetList += CompressGUID(target->GetGUID());
    }

    char header[64];
    std::snprintf(header, sizeof(header), "D:%.3f:%u:%d:", float(spell->GetCastTime()) / 1000.0f, spellInfo->Id, amount);
    std::string payload = std::string(header) + targetList;

    // LibHealComm-4.0 drops messages longer than this (e.g. a 40-player area heal)
    if (payload.size() > 240)
        return;

    {
        std::lock_guard<std::mutex> guard(_castLock);
        _castsInProgress[bot->GetGUID()] = spellInfo->Id;
    }

    SendFromBot(bot, payload);

    if (_debug)
        LOG_INFO("module", "mod-playerbot-heal-tracker: {} casts {} ({}) for {} on {} target(s).", bot->GetName(), spellInfo->SpellName[0],
            spellInfo->Id, amount, targets.size());
}

void PlayerbotHealTracker::OnCastEnd(Spell* spell, Unit* caster, bool interrupted)
{
    if (!HasSubscribers() || !spell || !caster || !caster->IsPlayer())
        return;

    SpellInfo const* spellInfo = spell->GetSpellInfo();
    if (!spellInfo)
        return;

    {
        std::lock_guard<std::mutex> guard(_castLock);
        auto itr = _castsInProgress.find(caster->GetGUID());
        if (itr == _castsInProgress.end() || itr->second != spellInfo->Id)
            return;
        _castsInProgress.erase(itr);
    }

    // S::<spellID>:<interrupted 0/1>   (no targets: ends the heal on everyone it was announced for)
    char payload[48];
    std::snprintf(payload, sizeof(payload), "S::%u:%d", spellInfo->Id, interrupted ? 1 : 0);
    SendFromBot(caster->ToPlayer(), payload);
}

/*************************************************************************
 * Heals over time (Renew, Rejuvenation, Regrowth, Lifebloom, Riptide, ...)
 *************************************************************************/

namespace
{
    // Whether an aura is a class heal over time that a client's LibHealComm-4.0 would announce
    bool IsTrackedHot(Aura* aura)
    {
        if (!aura || !aura->GetCasterGUID().IsPlayer())
            return false;

        SpellInfo const* spellInfo = aura->GetSpellInfo();
        return spellInfo && spellInfo->HasAura(SPELL_AURA_PERIODIC_HEAL) && spellInfo->SpellFamilyName != SPELLFAMILY_GENERIC
            && !spellInfo->IsPassive();
    }
}

void PlayerbotHealTracker::OnHotApplied(Unit* target, Aura* aura)
{
    // Cheap checks first: this runs for every aura applied to anything
    if (!_enabled || !_hots || !HasSubscribers() || !target || !IsTrackedHot(aura))
        return;

    Unit* casterUnit = aura->GetCaster();
    Player* bot = casterUnit ? casterUnit->ToPlayer() : nullptr;
    if (!bot || !bot->GetGroup() || !IsPlayerbot(bot))
        return;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        AuraEffect* effect = aura->GetEffect(i);
        if (!effect || effect->GetAuraType() != SPELL_AURA_PERIODIC_HEAL)
            continue;

        int32 totalTicks = effect->GetTotalTicks();
        int32 amplitude = effect->GetAmplitude();
        if (totalTicks <= 0 || amplitude <= 0)
            return;

        // The aura's amount already includes the caster's healing bonuses and is multiplied by the stack count;
        // LibHealComm-4.0 multiplies by the stack count itself.
        int32 stacks = std::max<int32>(1, aura->GetStackAmount());
        int32 tickAmount = effect->GetAmount() / stacks;
        int32 tickInterval = std::max<int32>(1, (amplitude + 500) / 1000);
        if (tickAmount <= 0)
            return;

        // H:<totalTicks>:<spellID>:<tickAmount>::<tickInterval>:<target>
        char header[64];
        std::snprintf(header, sizeof(header), "H:%d:%u:%d::%d:", totalTicks, aura->GetId(), tickAmount, tickInterval);
        QueueFromBot(bot, std::string(header) + CompressGUID(target->GetGUID()));
        return;
    }
}

void PlayerbotHealTracker::OnHotRemoved(Unit* target, Aura* aura)
{
    if (!_enabled || !_hots || !HasSubscribers() || !target || !IsTrackedHot(aura))
        return;

    Unit* casterUnit = aura->GetCaster();
    Player* bot = casterUnit ? casterUnit->ToPlayer() : nullptr;
    if (!bot || !bot->GetGroup() || !IsPlayerbot(bot))
        return;

    // HS::<spellID>::<target>
    char header[48];
    std::snprintf(header, sizeof(header), "HS::%u::", aura->GetId());
    QueueFromBot(bot, std::string(header) + CompressGUID(target->GetGUID()));
}
