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
Only project-authored renderer and test shaders are included.
Game files, keys, and translated game shaders stay local.

**For iPhone or iPad, also have:**

- **iOS / iPadOS 17.0 or later.**
- **Native BC texture compression is recommended.** Devices without it use experimental software BC decoding; non-BC physical devices remain unverified.
- A signing account and development team configured in Xcode.
- A trusted, unlocked device with **Developer Mode** enabled.

## Get the source

```sh
git clone https://github.com/Legoless/skyrim-apple.git
cd skyrim-apple
```

Put your cartridge dump and console keys in the repository's `inputs/` folder:

```text
skyrim-apple/
  inputs/
    Skyrim.xci
    prod.keys
```

The XCI may have any filename; keep exactly one `.xci` file or split-dump `.xci`
directory in `inputs/`. For split dumps, keep the numbered parts (`00`, `01`, etc.)
inside that directory. Everything in `inputs/` except its placement guide is
ignored by Git. Setup discovers these files automatically, so no paths or prompts
are needed. Explicit command-line paths still work; if inputs are absent, setup
prompts in a terminal. See [input placement](inputs/README.md).

## Play on Mac

**1. Set up the game.**

```sh
./setup-skyrim.sh
```

Split dumps work directly, including `.xci` folders containing `00`, `01`, and so on.
Setup installs tools, extracts the game, rewrites its modules, rebuilds and verifies
the shader cache, then builds the host. No separate shader commands are required.
Later runs verify and reuse the matching cache.

Shader verification currently targets the recorded `v327680` cache. If your
cartridge produces different shader inputs, setup stops with an error; it does
not reuse an incompatible cache. Base-cartridge compatibility remains unverified.

**2. Launch.**

```sh
./skyhost/run.sh
```

## Install on iPhone or iPad

**1. Prepare the iOS build.**

```sh
./setup-skyrim.sh --ios
```

This also prepares the shader cache automatically; no macOS preparation pass is required.

**2. Install and launch.**

```sh
./setup-skyrim.sh --deploy-ios
```

Choose your signing team and bundle ID when prompted.
Deployment verifies the game files. It backs up saves before installation and checks them before launch.

For later installs, rerun the deploy command with the same bundle ID.
The dump and keys are only needed during preparation.

<details>
<summary>Commands without prompts</summary>

```sh
# macOS
./setup-skyrim.sh '/path/to/game.xci' --keys /path/to/prod.keys

# iPhone / iPad
./setup-skyrim.sh '/path/to/game.xci' --keys /path/to/prod.keys --ios \
  --bundle-id your.bundle.id
./setup-skyrim.sh --deploy-ios --team YOUR_TEAM_ID \
  --device DEVICE_UDID_OR_NAME --bundle-id your.bundle.id
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
