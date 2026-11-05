# The New Tetris: Reborn

[![Platform: Linux & Windows](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows-blue.svg)](https://github.com/depressobbq/tnt64-Reborn)
[![Engine: N64Recomp + RT64](https://img.shields.io/badge/Engine-N64Recomp%20%2B%20RT64-green.svg)](https://github.com/Mr-Wiseguy/N64Recomp)
[![Audio: Hi--Fi MP3 / WAV](https://img.shields.io/badge/Audio-CD--Quality%20Streaming-orange.svg)]()
[![Status: Active Development](https://img.shields.io/badge/Status-Active-brightgreen.svg)]()

*The New Tetris: Reborn* is an enhanced native PC port of *The New Tetris* (N64).

It preserves authentic physics, rotation rules, and piece generation while adding support for high-fidelity soundtrack streaming, custom music playlists, deterministic tournament seed overrides, an expanded live HUD, and automated Discord match logging.

---

## Key Features

### 🎵 High-Fidelity Audio & Custom Soundtracks
- **CD-Quality Soundtrack:** Plays full-fidelity audio (`.mp3` and `.wav`) placed in `soundtrack/`, replacing compressed original sequences with uncompressed dynamic range.
- **Custom Music Directory:** Load custom `.mp3` or `.wav` files from `custom_tracks/`, accessible in the Audio Menu as **`CUSTOM.`**.
- **Continuous Rotation:** In Random music mode, upcoming tracks are asynchronously decoded and prepared ahead of time for uninterrupted playback between matches.
- **Accurate Mixing:** Integrated software mixing matching original game volume levels with no dropouts or clicks.

### 🏆 Tournament Mode & Deterministic RNG
- **Tournament Menu:** Configurable options menu accessible directly via **Options → TOURNAMENT**.
- **Deterministic 32-Bit Seed Override:** Locks game PRNG to a consistent seed for synchronized matches, races, and challenges.
- **Consistent Bag Generation:** Deterministic bag hashing ensures all players on identical seeds receive identical piece sequences throughout the match.
- **In-Menu Seed Selection:** Adjust seeds with D-pad or bumper steps, or generate random seeds on demand.

### 📊 Live Sidebar HUD
An integrated scorecard rendered alongside the playfield:
- **Game Mode Display:** Current mode (`MARATHON`, `SPRINT`, `ULTRA`).
- **Match Timer:** `TIME: MM:SS` counter integrated into the HUD card.
- **True Lines:** Exact physical 10-block lines cleared without score multiplier distortion.
- **Live LPM:** Pacing metric updated in real time.
- **Block Breakdown:**
  - **`TOTAL BLOCKS`**: Total 4×4 cubes formed.
  - **`GOLD BLOCKS`**: Solid 4×4 single-tetromino cubes.
  - **`SILVER BLOCKS`**: Multisquare 4×4 cubes formed from mixed tetrominoes.
- **Cube Line Clears:**
  - **`GOLD LINES`**: Cleared lines containing gold blocks.
  - **`SILVER LINES`**: Cleared lines containing silver blocks.
- **Match Seed:** Displays active seed or `RANDOM`.

### 🌐 Discord Webhook Integration
- Automatically submits post-match results to Discord via incoming webhooks.
- Match summaries include player name, match time, score lines, true lines, LPM, block counts, and seed verification.

### 🕹️ Controls & Quality-of-Life
- **In-Menu Exit Game:** Controller-navigable **`EXIT GAME`** option in the Tournament menu, as well as `ESC` key support.
- **Local Leaderboard:** Built-in match logging with local HTML rankings and CSV generation (`F4` hotkey).
- **High Resolution:** RT64 rendering support scaling up to 4K and ultrawide resolutions at 60 FPS.

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
- **Escape**: Exit game to desktop.

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
1. **Soundtrack:** Place MP3 or WAV files into the `soundtrack/` folder. The game automatically matches track names (e.g. `morocco`, `dvie`, `polyasia`, `floppy`, `pyramid`, `giali`, `thread6`, `haluci`, `mayan`, `greek`, `egypt`, `celtic`, `africa`, `japan`, `kalinka`, `title`).
2. **Custom Tracks:** Place any MP3 or WAV files into `custom_tracks/`. In the in-game Audio Menu, select **`CUSTOM.`** to play from your folder, or set music to **`RANDOM`** to rotate continuously through all soundtrack and custom songs.

---

## Building from Source

### Linux (Ubuntu/Debian/Arch/Fedora)
Requirements: `cmake`, `ninja-build`, `clang` or `gcc`, `libsdl2-dev`.

```bash
# 1. Clone repository
git clone git@github.com:depressobbq/tnt64-Reborn.git
cd tnt64-Reborn

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
antialiasing = 8x
filtering = pixel_scaling
upscale_2d = all
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
