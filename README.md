# Bot LFG Accept

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module for
[mod-playerbots](https://github.com/mod-playerbots/mod-playerbots). Bots always accept the Dungeon
Finder ready check.

Out of the box, mod-playerbots declines the ready check whenever a bot is in combat or dead at the
moment the dungeon pops. One decline cancels the dungeon for the whole group and puts everyone back
in the queue. With this module:

- **Bots always accept.** Whether a bot is fighting, dead or idle, it accepts as soon as the dungeon
  pops. Real players still choose for themselves.
- **Bots arrive ready.** Before accepting, the module does what mod-playerbots' own accept does: a
  random bot that isn't in a group is revived, repaired, healed and restocked, and every bot's AI
  is reset, so it follows the group once inside.
- **Bots that can't teleport get pulled in.** A dead bot, a bot still in combat or a bot in a
  vehicle can't be teleported when the group forms. The module revives it, takes it out of combat
  and teleports it into the dungeon. It retries once a second for up to two minutes after the
  ready check.
- **Bots that never answer still accept.** That covers a bot whose AI is busy or idle, which would
  otherwise let the ready check time out.

It needs `ServerScript::OnPacketSent` and `WorldSession::IsHeadless()`. Upstream AzerothCore has
both, and the playerbots core fork has had them since its October 2026 core-align merge. Older
playerbots cores (with `PlayerbotScript` and `WorldSession::IsBot()`) need commit 78d2b99.

## How it works

mod-playerbots answers the ready check from the packet the server sends the bot. This module
reads that packet first, sets its proposal ID to 0 (so the playerbots accept action ignores it),
and accepts on the bot's behalf on the next world update. Just before accepting, it calls the same
refresh and AI reset that mod-playerbots' accept action would have. Teleport failures are caught from the
`SMSG_LFG_TELEPORT_DENIED` packet the bot is sent.

That only works because this module's hook runs before mod-playerbots' hook. AzerothCore loads
modules in folder name order, and `mod-bot-lfg-accept` sorts before `mod-playerbots`. **Keep the
folder name.**

## Requirements

- The [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) module (this module builds against its headers) on the playerbots core fork ([mod-playerbots/azerothcore-wotlk](https://github.com/mod-playerbots/azerothcore-wotlk), `Playerbot` branch) from its October 2026 core-align merge. The core needs `ServerScript::OnPacketSent` and `WorldSession::IsHeadless()`. Older playerbots cores need commit 78d2b99.
- A WoW 3.3.5a (12340) client. No SQL, no client patch.

## Installation

Clone it into your AzerothCore `modules` folder, **as `mod-bot-lfg-accept`**. The folder name
matters, both for the load order above and because AzerothCore derives the module's loader name
from it:

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/buildthehomelab/wow-mod-bot-lfg-accept.git mod-bot-lfg-accept
```

Re-run CMake, rebuild the worldserver, and copy `conf/mod_bot_lfg_accept.conf.dist` to
`mod_bot_lfg_accept.conf` in your config directory. The module needs no SQL and doesn't change
mod-playerbots.

To check that it's loaded, look for this line in the worldserver log at startup:

```
mod-bot-lfg-accept: enabled, refresh on, pull-in on
```

If it's missing, the module isn't in the build. Re-run CMake so it picks up the new folder, then
rebuild. With the Docker setup, rebuild the images rather than just restarting the containers.

## Configuration

| Option | Default | Description |
|---|---|---|
| `BotLfgAccept.Enable` | `1` | Master switch. |
| `BotLfgAccept.Refresh` | `1` | Before accepting, revive, repair, heal and restock random bots that aren't in a group, and reset every bot's AI (what mod-playerbots' own accept does). |
| `BotLfgAccept.PullIn` | `1` | Revive, take out of combat and teleport in any bot whose teleport failed when the group formed. |

## Troubleshooting

- **The `enabled, refresh on, pull-in on` line is missing from the worldserver log:** the module isn't in the build. Re-run CMake so it picks up the folder, then rebuild. With Docker, rebuild the images.
- **Bots still decline the ready check:** the folder must be named `mod-bot-lfg-accept`. The module's hook has to run before mod-playerbots', and modules load in folder name order.
- **Bots arrive with broken gear or low health:** check that `BotLfgAccept.Refresh` is `1`. Bots already in a group (for example in your party) only get the AI reset, as with mod-playerbots' own accept.
- **A bot stays outside the dungeon:** check that `BotLfgAccept.PullIn` is `1`. It retries once a second for up to two minutes after the ready check.

## Credits

Author: [buildthehomelab](https://github.com/buildthehomelab)

## License

MIT. See [LICENSE](LICENSE).
