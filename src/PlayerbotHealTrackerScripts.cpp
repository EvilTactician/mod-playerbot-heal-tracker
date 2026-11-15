/*
 * mod-playerbot-heal-tracker
 *
 * Released under the GNU General Public License v2, see LICENSE.
 */

#include "PlayerbotHealTracker.h"

#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuras.h"

// Configuration, and sending the queued heal over time messages
class PlayerbotHealTrackerWorldScript : public WorldScript
{
public:
    PlayerbotHealTrackerWorldScript() : WorldScript("PlayerbotHealTrackerWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_UPDATE
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sPlayerbotHealTracker->LoadConfig();
    }

    void OnUpdate(uint32 diff) override
    {
        sPlayerbotHealTracker->Update(diff);
    }
};

// The handshake with the client addon, and forgetting players who log out
class PlayerbotHealTrackerPlayerScript : public PlayerScript
{
public:
    PlayerbotHealTrackerPlayerScript() : PlayerScript("PlayerbotHealTrackerPlayerScript", {
        PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE,
        PLAYERHOOK_ON_LOGOUT
    }) { }

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override
    {
        // The addon whispers itself: "IRFBH\tHELLO:1" or "IRFBH\tBYE:1"
        if (lang == uint32(LANG_ADDON) && type == CHAT_MSG_WHISPER && msg.compare(0, 6, PLAYERBOT_HEAL_TRACKER_PREFIX "\t") == 0)
            sPlayerbotHealTracker->HandleAddonMessage(player, msg);
    }

    void OnPlayerLogout(Player* player) override
    {
        sPlayerbotHealTracker->Unsubscribe(player->GetGUID());
    }
};

// Heal casts by playerbots
class PlayerbotHealTrackerSpellScript : public AllSpellScript
{
public:
    PlayerbotHealTrackerSpellScript() : AllSpellScript("PlayerbotHealTrackerSpellScript", {
        ALLSPELLHOOK_ON_PREPARE,
        ALLSPELLHOOK_ON_CAST,
        ALLSPELLHOOK_ON_CAST_CANCEL
    }) { }

    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/) override
    {
        sPlayerbotHealTracker->OnCastStart(spell, caster);
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/, bool /*skipCheck*/) override
    {
        sPlayerbotHealTracker->OnCastEnd(spell, caster, false);
    }

    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/, bool /*bySelf*/) override
    {
        sPlayerbotHealTracker->OnCastEnd(spell, caster, true);
    }
};

// Heals over time applied and removed by playerbots
class PlayerbotHealTrackerUnitScript : public UnitScript
{
public:
    PlayerbotHealTrackerUnitScript() : UnitScript("PlayerbotHealTrackerUnitScript", true, {
        UNITHOOK_ON_AURA_APPLY,
        UNITHOOK_ON_AURA_REMOVE
    }) { }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        sPlayerbotHealTracker->OnHotApplied(unit, aura);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode /*mode*/) override
    {
        if (aurApp)
            sPlayerbotHealTracker->OnHotRemoved(unit, aurApp->GetBase());
    }
};

void AddPlayerbotHealTrackerScripts()
{
    new PlayerbotHealTrackerWorldScript();
    new PlayerbotHealTrackerPlayerScript();
    new PlayerbotHealTrackerSpellScript();
    new PlayerbotHealTrackerUnitScript();
}
