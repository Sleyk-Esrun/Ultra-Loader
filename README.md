<div align="center">

<img src="media/ultra_logo.png" alt="Ultra Loader" width="760">

### The native arcade execution engine. Free forever. Built for the community.

**U**nified **L**egacy **T**ranslation **R**untime **A**rchitecture

[![License: Ultra Loader Community License](https://img.shields.io/badge/license-Ultra%20Loader%20Community-2563eb.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-111420.svg)](https://github.com/Sleyk-Esrun/Ultra-Loader/releases)
[![Latest Release](https://img.shields.io/badge/release-v1.0.0-3b82f6.svg)](https://github.com/Sleyk-Esrun/Ultra-Loader/releases/latest)
[![Website](https://img.shields.io/badge/web-ultraloader.org-60a5fa.svg)](https://ultraloader.org)

**[Download](https://github.com/Sleyk-Esrun/Ultra-Loader/releases/latest) · [What is Ultra Loader?](https://ultraloader.org/what-is-ultra-loader) · [Manual](https://ultraloader.org/manual) · [Supported Games](https://ultraloader.org/vote#supported) · [EULA](https://ultraloader.org/eula)**

</div>

---

## What is Ultra Loader?

Arcade games are some of the most technically impressive and culturally significant games ever made — and most of them have never been released in any form players can own. When the cabinets age out and the operators move on, those games are simply gone.

**Ultra Loader is here to change that.** It is an original, clean-room arcade execution engine that runs your legal arcade game dumps natively on a Windows PC — no cabinet, no operator, no pay-to-play subscription gatekept behind someone else's wallet. One engine, one unified library, every supported board.

> It contains **no** copyrighted game code, **no** extracted assets, and **no** third-party firmware. You bring your own game files. We built the engine — you decide how to use it.

<div align="center">
<a href="media/ui-dual.png"><img src="media/ui-dual.png" alt="Ultra Loader settings and game library" width="100%"></a>
<br>
<i>Settings and your full game library, side by side. (Click to view full size.)</i>
</div>

---

## The ULTRA Way

Three steps. No per-game setup, no config spelunking, no launcher babysitting.

1. **Install** — Download the loader, extract the folder, run `ultra-loader.exe`.
2. **Select** — Browse your legal game dumps and pick the game's executable (`.exe`).
3. **Launch** — Press *Save and Launch*. Ultra Loader detects the board, loads the right module, and wires up omni-services — lightgun support, force feedback, wheels, flight sticks, card readers, credits — automatically. No further setup required.

---

## Built for Speed

Ultra Loader is written in **native C** and ships as a single lightweight executable with its UI, fonts, audio, splash and boot animation **embedded**. There's no managed runtime to spin up, no framework to load, and no per-game project files to parse before anything happens — the window and your entire game library are on screen the instant you double-click it.

That's a different class of responsiveness than managed, .NET-based launchers like **TeknoParrot**:

| | **Ultra Loader** | **TeknoParrot** |
|---|---|---|
| Core | Native C, single small `.exe` | Managed .NET / WPF launcher |
| Cold start | Near-instant — library shows immediately | Waits on the .NET runtime + UI framework to load |
| Launch path | Straight into a native injector/host | Through a managed launcher layer |
| Library | One unified grid that remembers **every** game you've ever launched | Per-game profiles you add and manage |
| Footprint | Lean; assets embedded in the exe | Framework + launcher dependencies |

The result: you spend time **playing**, not waiting on a launcher. Pick a game, press **Save & Launch**, and you're in.

---

## Library Art Styles

<div align="center">
<table>
<tr>
<td align="center"><img src="media/style-clear.png" width="420"><br><b>Clear Style</b></td>
<td align="center"><img src="media/style-silver.png" width="420"><br><b>Silver Ring</b></td>
</tr>
</table>
<i>Two built-in logo-art styles for your library. Art packs install from inside the loader.</i>
</div>

---

## Download & Run

1. Grab the latest **[Windows release](https://github.com/Sleyk-Esrun/Ultra-Loader/releases/latest)**.
2. Extract the **`Ultra Loader`** folder anywhere (your Desktop is fine).
3. Run **`ultra-loader.exe`**.

Everything the loader needs is embedded. On first run it writes its own `ultra.ini`, and it creates its `art\` folders automatically when you install art packs. **Game-art packs** and **per-game firmware** are distributed separately and install from inside the loader.

---

## One Screen, Everything

<div align="center">
<a href="media/ui-main.png"><img src="media/ui-main.png" alt="Ultra Loader settings" width="600"></a>
<br>
<i>Every setting in one place — display, upscaling, controllers, firmware, audio. Pick a game, hit <b>Save &amp; Launch</b>, and the loader detects the board and wires up every service automatically.<br>(Click to view full size.)</i>
</div>

---

## Game Library

<div align="center">
<a href="media/ui-library.png"><img src="media/ui-library.png" alt="Ultra Loader game library grid" width="620"></a>
<br>
<i>Your whole collection as a wall of art. (Click to view full size.)</i>
</div>

A single, art-driven grid of your games — each tile its own logo. The library is a **persistent, cross-folder history**: every game you launch is remembered and stays on the wall, no matter which folder it lives in. No re-adding, no per-game setup.

Curate it your way — tick the checkbox on any tile and hit **Delete Selected** to remove entries you don't want shown. (It only hides them from the grid; your game files are never touched.)

---

## Supported System Boards

The open-source board modules in this repository (`modules/`) cover **43 modern arcade system boards**:

| Manufacturer | Boards |
|---|---|
| **Namco** | BNA1, ES1, ES2, ES2+, ES3, ES4, N2 |
| **Sega** | ALLS, ALLS UX, Europa-R, Lindbergh, Nu, Nu2, Nu SX, RingEdge, RingEdge 2, RingWide, System SP |
| **Taito** | Type X, Type X², Type X³, Type X⁴, Type X+, Type X Zero, NESiCA, NESiCA 2 |
| **Konami** | BEMANI (PC), Arcade PC |
| **Raw Thrills** | G5, PC |
| **Adrenaline Amusements** | AA |
| **Andamiro** | PC+ |
| **Cave** | PC |
| **Examu** | eX-Board |
| **Global VR** | PC |
| **IGS** | PC |
| **Incredible Technologies** | ICE PC, IT PC |
| **Play Mechanix** | PC |
| **UNICO / UNIS / Wahlap** | UNICO PC, UNIS, Wahlap WPC |
| **Generic** | Generic PC |

Each board lives in `modules/<board>/` as original source — hook code, board I/O, profiles, and compatibility logic.

---

## This Repository

| What | Where |
|---|---|
| **Open-source board module source** | `modules/` (this repo) |
| **Windows loader (binary)** | [Releases](https://github.com/Sleyk-Esrun/Ultra-Loader/releases) |
| **Per-game firmware (`.ufw`)** | Installed from inside the loader |
| **Game-art packs** | Installed from inside the loader |

The loader itself is proprietary and free (see the [EULA](https://ultraloader.org/eula)). The **board modules are open** — made to be read, improved, and extended by the community.

---

## The Commitment

These are promises, in writing, where anyone can find them:

- **Always free.** No subscription, no paywall, no tier system. Ever.
- **No game access is ever paywalled.** Every user gets the same software with the same capabilities.
- **No telemetry.** Nothing leaves your machine unless you choose to send a bug report.
- **All R&D goes back to the community** at no cost.

Imagine 20 or 30 people pushing boards forward together instead of one gatekeeper hoarding the keys. That is how we win. Stop rewarding bad behavior. Bring your work back. **This is the way.**

---

## Contributing

The board modules are open so the community can make them better.

- Fix a bug, improve compatibility, or bring up a new game on an existing board.
- Keep it **clean-room**: original work only. No copyrighted game code, no extracted assets, no third-party firmware, and no code or references from any competing loader or tool.
- By contributing, you license your work under the [Ultra Loader Community License](LICENSE).

Open an issue or a pull request. Every improvement is returned to everyone.

---

## License

The open components (board modules and firmware modules) are released under the **[Ultra Loader Community License](LICENSE)** — free, open, non-commercial, share-alike. The Loader binary is proprietary and free under the **[EULA](https://ultraloader.org/eula)**.

<div align="center">

---

**ULTRA LOADER** · by **Sleyk** · [ultraloader.org](https://ultraloader.org)

*Rendering pay-to-play obsolete.*

</div>
