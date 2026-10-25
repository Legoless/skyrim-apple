# Skyrim on Apple Silicon

Skyrim's Nintendo Switch version, running on macOS, iPhone, and iPad.
Metal graphics. Keyboard and mouse on Mac. Touch controls on iOS.

> **Experimental.** Recorded iPad gameplay runs at about 5–6 FPS.
> Base-cartridge decryption and gameplay are still unverified. Later scenes and shader coverage are incomplete.

## Requirements

| You need | Details |
| --- | --- |
| **Mac** | Apple Silicon. Required to build both macOS and iOS apps. |
| **Xcode** | Full Xcode selected in `xcode-select`, with its license accepted. |
| **Python** | Version **3.9 or later**. |
| **Game** | Your own **Switch Skyrim base cartridge dump**, content version `0`. One `.xci` file or a split-dump folder. |
| **Keys** | Your console's `prod.keys` file. |

Setup installs its own tools. No Homebrew or update NSP required.
Only project-authored renderer and test shaders are included: game modules and
translated Metal shaders must first be built and cached locally into `skyrim-work/`
before running.

**For iPhone or iPad, also have:**

- **iOS / iPadOS 17.0 or later.**
- **Native BC texture compression is recommended.** Devices without it use experimental software BC decoding; non-BC physical devices remain unverified.
- A signing account and development team configured in Xcode.
- A trusted, unlocked device with **Developer Mode** enabled.

## Quickstart (Build, Cache & Run)

**1. Clone the repository.**

```sh
git clone https://github.com/Legoless/skyrim-apple.git
cd skyrim-apple
```

**2. Run the interactive entry point.**

```sh
./prepare-skyrim.sh
```

The script asks for your **cartridge `.xci`** (single file or split-dump folder) and
**`prod.keys`** (or pre-fills them automatically if placed in [`inputs/`](inputs/README.md)),
then asks which target to build:

- `1) macos` — Build the macOS host
- `2) ios-prepare` — Prepare the iOS Release Xcode project
- `3) ios-deploy` — Prepare, sign, install, sync RomFS, and launch on iPhone/iPad
- `4) both` — Build the macOS host + prepare the iOS Xcode project
- `5) all` — Build the macOS host + prepare and deploy to iPhone/iPad

It installs pinned local tools, extracts the cartridge, rewrites `main` and `sdk`,
rebuilds and verifies the Metal shader cache in `skyrim-work/build/shaders/`, and
builds or deploys your selected target. Later runs reuse the verified workspace and
shader cache automatically.

Shader verification currently targets the recorded `v327680` cache. If your
cartridge produces different shader inputs, setup stops with an error; it does
not reuse an incompatible cache. Base-cartridge compatibility remains unverified.

**3. Play or redeploy.**

```sh
# Launch on Mac
./skyhost/run.sh

# Redeploy to iPhone or iPad (dump and keys are no longer needed once prepared)
./prepare-skyrim.sh --target ios-deploy
```

<details>
<summary>Commands without prompts</summary>

```sh
# macOS (add --run to launch immediately)
./prepare-skyrim.sh '/path/to/game.xci' --keys /path/to/prod.keys --target macos

# macOS + iPhone / iPad deployment
./prepare-skyrim.sh '/path/to/game.xci' --keys /path/to/prod.keys \
  --target all --team YOUR_TEAM_ID --device DEVICE_UDID_OR_NAME \
  --bundle-id your.bundle.id
```

</details>

## Controls

| Mac | Action |
| --- | --- |
| WASD | Move |
| F1 | Toggle mouse look |
| E / Space | Use / Jump |
| Escape | Open journal and release captured cursor; otherwise Back |
| Arrows or wheel / Enter | Navigate menus / Confirm |

Direct menu taps require the verified update profile. Other builds use controller-style input.

[All controls](skyhost/CONTROLS.md) · [iOS controls](skyhost/ios/README.md#controls-and-verification)

## Saves and files

| Item | Location |
| --- | --- |
| Local workspace | `skyrim-work/` |
| Mac saves | `skyrim-work/run/save/` |
| iOS saves | App's `Documents/save/` |
| iOS save backups | `skyrim-work/build/deploy-ios/save-backups/` |

<details>
<summary>Use a different workspace</summary>

```sh
./setup-skyrim.sh --work-dir /absolute/path/to/workspace
SKY_WORK_DIR=/absolute/path/to/workspace ./skyhost/run.sh
```

Pass the same `--work-dir` to iOS preparation, deployment, and checks.
The private shader cache stays at `skyrim-work/build/shaders/` in this checkout.

</details>

## Useful commands

| Command | Purpose |
| --- | --- |
| `./prepare-skyrim.sh` | Interactive entry point to cache shaders/modules and build or deploy Mac/iOS. |
| `./setup-skyrim.sh --check` | Check an already-prepared macOS workspace. |
| `./setup-skyrim.sh --check --ios` | Check an already-prepared iOS workspace. |
| `./setup-skyrim.sh --tools-only` | Install tools without game files. |
| `./setup-skyrim.sh --tools-only --ios` | Include iOS build tools. |

## More details

- [Setup guide](.cursor/skills/skyrim-setup/SKILL.md) — pinned tools, extraction, and troubleshooting.
- [Host guide](skyhost/README.md) — architecture, tests, shaders, and performance.
- [iOS guide](skyhost/ios/README.md) — signing, deployment, and device tests.
- [Audio renderer](skyhost/audio_renderer/README.md) — why .NET is used at build time.
- [Source and shader provenance](docs/DERIVED.md) — third-party code and licenses.

## License

All project-authored code and documentation are [MIT-licensed](LICENSE).
See [license scope](docs/DERIVED.md#license-scope) for third-party notices and game-derived asset exclusions.
