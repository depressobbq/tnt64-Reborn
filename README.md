# The New Tetris: Reborn (TNT Reborn)

[![Platform: Linux & Windows](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows-blue.svg)](https://github.com/depressobbq/tnt64-Tournament-Edition)
[![Engine: N64Recomp + RT64](https://img.shields.io/badge/Engine-N64Recomp%20%2B%20RT64-green.svg)](https://github.com/Mr-Wiseguy/N64Recomp)
[![Audio: Hi--Fi MP3 / WAV](https://img.shields.io/badge/Audio-CD--Quality%20Streaming-orange.svg)]()
[![Status: Active Development](https://img.shields.io/badge/Status-Active%20Remaster-brightgreen.svg)]()

*The New Tetris: Reborn* is an enhanced, feature-rich modern remaster and competitive fork of the *The New Tetris* (N64, USA) native PC recompilation.

Reborn preserves 100% of the authentic physics, rotation system, and piece-spawning algorithms of the original 1999 masterpiece while introducing CD-quality live soundtrack streaming, custom music rotation, deterministic tournament-grade RNG seeding, an expanded competitive live HUD, and automated Discord score broadcasting.

---

## Key Features

### 🎵 High-Fidelity Audio Engine & Custom Soundtrack
- **Live CD-Quality Soundtrack:** Automatically detects and plays live studio tracks (`.mp3` and `.wav`) placed in the `soundtrack/` folder, replacing compressed N64 sequences with full dynamic range.
- **Custom Music Directory:** Dedicated `custom_tracks/` directory seamlessly integrated into the in-game Audio Menu as **`CUSTOM.`**.
- **Continuous Seamless Rotation:** In Random music mode, a dedicated background preloader thread (`preload_worker`) decodes and resamples the next upcoming track ahead of time, ensuring hitch-free, continuous music rotation across matches.
- **Smooth Audio Mixing:** Hardware-accurate software mixing with zero audio dropouts, underruns, or clicks.

### 🏆 Tournament Mode & Deterministic RNG
- **In-Game Tournament Menu:** Dedicated, authentic beveled options menu accessible right from **Options $\rightarrow$ TOURNAMENT**.
- **Deterministic 32-Bit Seed Override:** Locks game PRNG to a consistent initial seed for synchronized races, speedrunning, and daily community challenges.
- **Golden-Ratio Bag Generation:** Employs deterministic multiplicative bag hashing (`0x9E3779B9`) ensuring that all players on the same seed receive the exact same sequence of pieces throughout the entire match.
- **In-Menu Seed Customizer:** Rapidly tune seeds with D-pad or $\pm 0\text{x}1000$ bumper steps, or roll a new seed with one-button randomization.

### 📊 Competitive Live Sidebar HUD
An authentic, beveled scorecard overlay rendered directly on the right side of the playfield:
- **Dynamic Mode Display:** Displays current game mode (`MARATHON`, `SPRINT`, `ULTRA`).
- **Integrated Match Timer:** Unobtrusive `TIME: MM:SS` counter integrated directly into the HUD card (suppressing redundant floating overlays).
- **True Lines Cleared:** Exact count of physical 10-block lines cleared without score multiplier distortion.
- **Live LPM (Lines Per Minute):** Real-time pacing metric updating continuously throughout play.
- **Cubes & Blocks Breakdown:**
  - **`TOTAL BLOCKS`**: Total $4\times 4$ cubes formed.
  - **`GOLD BLOCKS`**: Solid single-piece $4\times 4$ cubes.
  - **`SILVER BLOCKS`**: Multisquare $4\times 4$ cubes formed from mixed tetrominoes.
- **Cube Line Clears:**
  - **`GOLD LINES`**: Lines cleared containing gold blocks.
  - **`SILVER LINES`**: Lines cleared containing silver blocks.
- **Static Match Seed:** Cleanly displays active seed (`0xXXXXXXXX`) or `RANDOM`.

### 🌐 Discord Webhook Score Integration
- Automatically submits post-match results directly to your Discord community via Discord Webhooks.
- Rich embeds include Player name, Total Time, Score Lines, True Lines, LPM, Total Blocks, Gold/Silver Blocks, Gold/Silver Lines, and Seed verification.

### 🕹️ Modern Controls & Quality-of-Life
- **In-Menu Exit Game:** Controller-navigable **`EXIT GAME`** option in the Tournament menu for seamless couch play.
- **Clean Shutdown Architecture:** Robust RAII thread management ensuring clean exits (code `0`) with zero core dumps or hangs on window close, `ESC`, or in-menu quit.
- **Local Leaderboard:** Built-in automatic match logging with real-time HTML rankings and CSV generation (`F4` hotkey).
- **Native High Resolution:** Crisp RT64 rendering scaling up to 4K and Ultrawide displays at a steady 60 FPS.

---

## Controls

| Action | N64 Button | Keyboard (Classic) | Keyboard (Modern) | Gamepad (Xbox/PlayStation) |
|---|---|---|---|---|
| **Move Left / Right** | D-Pad Left / Right | Left / Right | Left / Right | D-Pad Left / Right |
| **Soft Drop** | D-Pad Down | Down | Down | D-Pad Down |
| **Hard Drop** | D-Pad Up | Up | Up or Space | D-Pad Up |
| **Rotate Clockwise** | A | X | X | A / Cross |
| **Rotate Counter-Clockwise** | B | C | Z | B / Circle or X / Square |
| **Hold Piece** | L | Q | C or Shift | LB / L1 |
| **Start / Pause** | START | Enter | Enter or P | Start / Options |
| **Spin / Special** | R | E | E | RB / R1 |
| **Multiplayer Target** | C Buttons | I J K L | I J K L | Right Stick |
| **Menu Navigation** | D-Pad / Stick | Arrows / WASD | Arrows | D-Pad / Left Stick |
| **Select / Confirm** | A | X | X | A / Cross |
| **Back / Cancel** | B | C | Z | B / Circle |

### Hotkeys
- **F3**: Toggle on-screen FPS and VI counters.
- **F4**: Open local HTML Leaderboard in default web browser.
- **F11** or **Alt + Enter**: Toggle Fullscreen.
- **Escape**: Quit game cleanly to desktop.

---

## Directory Setup

```
tnt64-recomp/
├── baserom.us.z64           # The New Tetris (USA) ROM (MD5: 7a28179b00734c9aa0f0609fafaafd5f)
├── soundtrack/              # (Optional) MP3 or WAV live soundtrack files
│   ├── 01_morocco.mp3
│   ├── 02_dvie.mp3
│   └── ...
├── custom_tracks/           # (Optional) Any MP3 or WAV tracks for custom rotation
│   ├── my_favorite_song.mp3
│   └── ...
├── play.sh                  # Linux launcher script
└── settings.ini             # Persistent user settings
```

### Adding Soundtrack & Custom Music
1. **Soundtrack:** Place your MP3 or WAV files into the `soundtrack/` folder. The game automatically matches track names (e.g. `morocco`, `dvie`, `polyasia`, `floppy`, `pyramid`, `giali`, `thread6`, `haluci`, `mayan`, `greek`, `egypt`, `celtic`, `africa`, `japan`, `kalinka`, `title`).
2. **Custom Tracks:** Place any MP3 or WAV files into `custom_tracks/`. In the in-game Audio Menu, select **`CUSTOM.`** to play from your folder, or set music to **`RANDOM`** to rotate continuously through all soundtrack and custom songs.

---

## Building from Source

### Linux (Ubuntu/Debian/Arch/Fedora)
Requirements: `cmake`, `ninja-build`, `clang` or `gcc`, `libsdl2-dev`.

```bash
# 1. Clone repository
git clone git@github.com:depressobbq/tnt64-Tournament-Edition.git
cd tnt64-Tournament-Edition

# 2. Place your USA 1.0 ROM in the repository root as baserom.us.z64

# 3. Configure and compile
cmake -B TNTRecomp/out/build/linux -S TNTRecomp -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build TNTRecomp/out/build/linux -j$(nproc)

# 4. Launch game
./play.sh
```

### Windows
Requirements: Visual Studio 2022 with **Desktop development with C++** and **C++ Clang Compiler for Windows** (`clang-cl`).

1. Place `baserom.us.z64` in the project root.
2. Run `run_build.bat` in `TNTRecomp/`.
3. Launch with `play.bat`.

---

## Settings (`settings.ini`)

A default `settings.ini` is generated on first launch:
```ini
[display]
fullscreen = false
resolution = auto
antialiasing = msaa4
aspect_ratio = original
show_fps = false

[audio]
volume = 80
audio_latency = 2

[tournament]
seeded_run = false
custom_seed = 0x12345678
tournament_hud = true
enable_discord = false
discord_webhook = ""
```

---

## Acknowledgments
- **H2O Entertainment & Blue Planet Software** for developing the timeless original game.
- **Nintendo** for the Nintendo 64 platform.
- **Mr-Wiseguy & N64Recomp Contributors** for the static recompilation engine.
- **RT64 Team** for modern graphics emulation and rendering backend.
