# mod-playerbot-heal-tracker

An AzerothCore module for playerbots which provides addons with support for heal predictions from playerbots. Used by [ImprovedRaidFrames](https://github.com/EvilTactician/ImprovedRaidFrames) and my fork of DragonUI.

## Why

Addons that show incoming heals on raid and unit frames use **LibHealComm-4.0**. It only knows about another player's heals when that player's client tells the group about them. [Playerbots](https://github.com/mod-playerbots/mod-playerbots) have no client, so their heals never show as incoming heals.

This module sends those messages for the bots. The server knows exactly which heal a bot is casting, on whom, and for how much, so the prediction is as accurate as one from a real player's client. The messages use LibHealComm-4.0's own format, so **every addon that uses LibHealComm-4.0** shows them, not just Improved Raid Frames.

## What is sent

- **Heals with a cast time** (Flash Heal, Greater Heal, Healing Wave, Holy Light, Regrowth, Prayer of Healing, ...): announced when the bot starts casting, and ended when the cast finishes or is interrupted.
- **Heals over time** (Renew, Rejuvenation, Regrowth, Lifebloom, Riptide, Wild Growth, Earthliving, ...): announced when the bot applies them, and ended when they fade or are removed.

The amount includes the bot's spell power and talents. The target's own healing modifiers (such as Mortal Strike) are applied by LibHealComm-4.0 on the receiving side, as for any other heal.

Not sent (yet): channelled heals (Penance, Tranquility, Divine Hymn), Lifebloom's final bloom, and the jump targets of Chain Heal (only its first target is known before it lands). Instant heals need no prediction.

## Who receives it

Only players who ask for it. The client addon sends a short "hello" when its option is turned on, and the module answers so the addon can show that the module is installed. Players without the addon (or with the option off) receive nothing extra.

Messages only go to the members of the bot's own party, raid or battleground group.

In **Improved Raid Frames**: Options → Improved Raid Frames → Heal Prediction → **Show Playerbot Heals**. The status line under it shows whether this module was detected.

Other addons can use the same handshake: whisper yourself the addon message `HELLO:1` with prefix `IRFBH` (`SendAddonMessage("IRFBH", "HELLO:1", "WHISPER", UnitName("player"))`). The module answers with `ACK:1:<version>` and from then on sends playerbot heals to that player, until `BYE:1` or logout.

## Requirements

- AzerothCore with [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) (built and tested against [Grimfeather's AzerothCore fork](https://github.com/Grimfeather/azerothcore-wotlk)). Without mod-playerbots the module disables itself and logs an error.
- A client addon that uses LibHealComm-4.0 and asks for the data, such as Improved Raid Frames.

## Installation

1. Clone or download this repository into your AzerothCore `modules` folder, so it ends up as `modules/mod-playerbot-heal-tracker`.
2. Re-run CMake and rebuild the server.
3. Copy `mod_playerbot_heal_tracker.conf.dist` to `mod_playerbot_heal_tracker.conf` in your server's config folder (usually `etc/modules`) if you want to change any settings.

## Configuration

| Setting | Default | Description |
|---|---|---|
| `PlayerbotHealTracker.Enable` | `1` | Enable the module. |
| `PlayerbotHealTracker.DirectHeals` | `1` | Send heals with a cast time. |
| `PlayerbotHealTracker.HoTs` | `1` | Send heals over time. |
| `PlayerbotHealTracker.HoTDelay` | `300` | Milliseconds to wait before announcing a heal over time, so the client already shows the aura (LibHealComm-4.0 ignores the message otherwise). Raise it if bot HoTs don't show. |
| `PlayerbotHealTracker.Debug` | `0` | Log handshakes and bot heal casts. |

## Performance

The spell and aura hooks run for every spell and aura on the server, so they stop at the first cheap check: if nobody has asked for playerbot heals, they return immediately. Otherwise only player casts of heal spells by bots in a group are processed, and each sends one short message to the subscribed players in that group.

## Credits

- The message format is LibHealComm-4.0's (Shadowed, xbeeps, Azilroka; 3.3.5a backport by Tsoukie).
- [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) and [AzerothCore](https://www.azerothcore.org/).

## Licence

GNU General Public License v2, see [LICENSE](LICENSE).
