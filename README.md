# ComboShip — Co-op Fan Fork (junkx420)

> [!IMPORTANT]
> **This is NOT the official ComboShip. It is a personal fan project, a fork made for fun.**
>
> - **All credit for ComboShip goes to its creator, [Varuuna](https://github.com/Varuuna), and everyone who contributes to the original project: [github.com/Varuuna/ComboShip](https://github.com/Varuuna/ComboShip).** This fork only adds a few co-op changes on top of their work.
> - This fork is **not affiliated with, endorsed by, or supported by** the ComboShip developers, the HarbourMasters team (Ship of Harkinian), or the 2 Ship 2 Harkinian team.
> - **Please do not report problems from this fork to the original projects.** If something breaks here, it is most likely my fault. Open an issue in *this* repository instead.
> - The goal is to offer these changes to the original project. Once they are merged there (or turned down), please use the official ComboShip, not this fork.
> - Parts of this fork were written with AI assistance.

> [!NOTE]
> **Deutsch:** Das hier ist **nicht** das offizielle ComboShip, sondern ein privates Fan-Projekt. **Alle Credits gehören [Varuuna](https://github.com/Varuuna) und den Mitwirkenden von [ComboShip](https://github.com/Varuuna/ComboShip).** Kein Bezug zu den ComboShip-Entwicklern, HarbourMasters oder 2 Ship 2 Harkinian. Fehler aus diesem Fork bitte hier melden, nicht beim Original.

## Why this fork exists

My friend and I play ComboShip co-op over Anchor, usually split across the two games: one of us mostly in Ocarina of Time, the other mostly in Majora's Mask, and we switch when one game runs out of checks. In that setup a couple of things felt disconnected, so this fork tries to make split play feel like one shared run.

## What this fork changes

Compared to the original ComboShip `develop` branch:

- **Background world sync.** While you play MM, your teammate's OoT progress still lands in your OoT save right away: opened chests, world flags, check-tracker status, discovered entrances, dungeon keys and magic beans. When you switch to OoT, their chests are already open, so the same check can't be collected twice. (Before, this only caught up after the teammate saved.)
- **Live partner feed.** You see what your teammate finds in the *other* game, e.g. `Teammate found Hookshot (MM)` while you are in OoT. Junk such as rupees and ammo is left out. It only shows a message and never gives items.
- **Game-switch notice.** You get a message like `Teammate is now in Majora's Mask` whenever your teammate moves between the games.
- **Toggles.** Both kinds of message can be turned off under Shared > Settings > Network > *Partner Feed*.
- **Small fixes.** No echoed flag packets, a small memory leak fixed, and a teammate's single arrow no longer arrives as a "Fairy Bow".

All of it lives in the Anchor networking code and is documented in [`docs/deviations/anchor.md`](docs/deviations/anchor.md). The sections dated 2026-11-07 are the ones added by this fork.

**Both players need a build of this fork.** An official ComboShip build still connects, but you only get the parts both sides support.

## Downloads (Windows / Linux / macOS)

Builds are made by GitHub Actions:

- **Releases:** see the [Releases page](../../releases), if a release exists.
- **Latest CI build:** open the newest successful run under [Actions](../../actions), scroll to **Artifacts** and download the archive for your system (`ComboShip-windows`, `ComboShip-linux` / `ComboShip-linux-appimage`, `ComboShip-macos`).

While this repository is private, you need to be invited as a collaborator and signed in to GitHub to download anything.

Like the original, ComboShip needs your own legally dumped Ocarina of Time and Majora's Mask ROMs. No game data is included or shared here.

## Building it yourself

The build works the same as the original project. See [`docs/BUILDING_LINUX.md`](docs/BUILDING_LINUX.md) for Linux, and the [original ComboShip README](https://github.com/Varuuna/ComboShip#readme) for Windows and macOS.

## Credits

- **ComboShip** by [Varuuna](https://github.com/Varuuna) and contributors: [github.com/Varuuna/ComboShip](https://github.com/Varuuna/ComboShip). Everything here is their work, except the few changes listed above.
- **Ship of Harkinian** by the [HarbourMasters](https://github.com/HarbourMasters/Shipwright) team, including the Anchor co-op mode this fork builds on.
- **2 Ship 2 Harkinian** by the [2 Ship 2 Harkinian](https://github.com/2ship2harkinian/2ship2harkinian) team.
- **libultraship** by [Kenix3](https://github.com/Kenix3/libultraship) and contributors.
- Inspired by [OoTMM](https://ootmm.com/).

## License

Nothing is relicensed here. Each bundled project keeps its own license: see the upstream repositories linked above and [`libultraship/LICENSE`](libultraship/LICENSE). This fork's changes are offered under the same terms as the files they modify.
