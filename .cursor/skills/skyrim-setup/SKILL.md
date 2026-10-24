---
name: skyrim-setup
description: >-
  Sets up Skyrim on a new machine from a clone plus a dump of a copy the user
  owns. Use when cloning this repo, preparing skyrim-work, extracting the base
  cartridge XCI, rewriting main and sdk, translating captured shaders, or asking
  where patches, Metal shaders, and touch controls live.
---

# Skyrim setup on a new machine

A clone contains host source, setup tooling, and project-authored helper/test shaders. Game shader captures and translations stay private in the ignored `skyrim-work/` directory. Never commit `prod.keys`, packages, RomFS, `*.patched.bin`, game-derived shader pairs, raw captures, or runtime logs. Use `setup-skyrim.sh`.

Touch controls are already source and need nothing generated: `skyhost/touch.c`, `skyhost/touch.h`, and `skyhost/ios/TouchControlsView.m`. Device checks are in `AGENTS.md`.

## Original game input

The default input folder is `inputs/` at the repository root, independent of the
current directory and `--work-dir`. It contains `prod.keys` and exactly one `.xci`
file or split-dump `.xci` directory (any basename). All contents except
`inputs/README.md` are ignored by Git. Normal setup discovers these when explicit
paths are omitted; multiple dumps fail with a request for an explicit path.
Explicit paths take precedence. Read-only, tools-only, and deployment modes do
not inspect this folder. Setup never moves or modifies the original inputs.

Supply your own cartridge dump as a single `.xci` file or a split-dump directory at any path. A split dump may contain numbered parts `00`, `01`, and so on directly, or one nested `.xci` directory containing those parts. Pass that original directory directly. Setup finds the parts, joins them into a verified local cache, and ignores sidecar certificate/initial-data files. It does not change the dump. No manual concatenation or repacking is needed. `--base` remains an alternative to the positional input.

Setup targets the original **base cartridge, content version `0`**. Supply that dump and the user's console `prod.keys` to decrypt its encrypted NCAs. No update NSP is required or downloaded. Keys remain user-provided and outside the repository.

Never download game packages or keys as part of bootstrap. Decrypt only the user's own dumps with their console's keys.

## Prepare and build

Requirements: an Apple Silicon Mac, full Xcode with its license accepted, Python 3.9 or later, and internet access for the first tool downloads and NuGet restore. The setup supports this macOS build environment; it does not support Linux or Windows. The wrapper selects `SKY_PYTHON` when explicitly set, otherwise `python3` on PATH, with `/usr/bin/python3` as a fallback. For a specific interpreter, use `SKY_PYTHON=/path/to/python3 ./setup-skyrim.sh`. The bootstrap installs its other tools locally; no Homebrew or manual tool installation is needed for normal setup. This does not replace Xcode, Apple SDKs, or signing credentials.

Run from the repository root in an interactive terminal:

```sh
./setup-skyrim.sh
```

Setup uses `inputs/` when paths are omitted, then prompts only for still-missing dump and key paths when attached to a terminal. A populated input folder requires no prompts. Explicit paths also work for automation; a noninteractive run fails if either input remains missing:

```sh
./setup-skyrim.sh '/path/to/game.xci' --keys /path/to/prod.keys
```

The default command installs pinned tools, extracts the cartridge's Program and Control content, records the actual `main` and `sdk` hashes/Build IDs, generates the ExeFS inventory, rewrites both modules, and builds the macOS host. It writes `skyrim-work/data/switch/skyrim/base/Update/{ExeFS,RomFS,Control}` and `skyrim-work/build/rewrite-native/`. The `Update` directory name is retained for the workspace layout; its contents come from the base cartridge. `Control/control.nacp` provides the cartridge's save settings, RomFS contains `Skyrim.ini` and `Data`, and rewrite output includes the `.patched.bin`, `.fixups.tsv`, and `.gates.tsv` files consumed by both hosts. Inventory, module identities, transfer manifests, and rewrite outputs stay local.

Both macOS and iOS setup automatically call `rebuild_shaders.py --ensure` after extraction and rewriting and before preparing the host/project. It extracts shader inputs from the archive and main module, verifies they match the recorded `v327680` reference, then rebuilds missing/empty caches or verifies and reuses existing matching files. Changed or incomplete caches are preserved and rejected. No separate shader command or macOS preparation pass is required for iOS. `--check` is read-only and requires a complete cache. Base-cartridge cache compatibility and runtime capture from zero remain unverified; mismatching game inputs stop setup instead of reusing an incompatible cache.

The base-cartridge pipeline has build and synthetic-test coverage. Real decryption and base gameplay have not been verified in this environment because the user explicitly waived testing with their keys. An unrecognized cartridge Build ID uses the generic controller path, including on-screen Back (B), Pause, Use (A), and sticks. Private menu-touch hooks from the verified update build are enabled only for that matching profile; direct tapping of base-game menus is not established.

For iPhone or iPad, add `--ios`:

```sh
./setup-skyrim.sh '/path/to/game.xci' --ios \
  --keys /path/to/prod.keys
```

This prepares the device Release DSP and `skyhost/build/ios/SkyrimIOS.xcodeproj` with default bundle ID `dev.skyhost.skyrim`; `--bundle-id your.bundle.id` overrides it. `./setup-skyrim.sh --ios` also prompts for missing paths in an interactive terminal. Preparation embeds the rewritten modules and shaders; deployment is a separate mode below. Normal setup and play do not require an existing save.

Deploy the prepared workspace without supplying the dump or keys again:

```sh
./setup-skyrim.sh --deploy-ios --team YOUR_TEAM_ID --device DEVICE_UDID_OR_NAME \
  --bundle-id your.bundle.id
```

Prerequisites are a signing account configured in Xcode and a trusted, unlocked iPhone or iPad with Developer Mode enabled. Deployment uses the pinned local tools, prepares the current project for the chosen bundle ID, signs a Release build, installs it with `devicectl`, syncs only `Documents/Skyrim/RomFS`, verifies every transferred file by SHA-256 readback, then launches. Installing over the same bundle ID preserves the app's container and saves. A failed transfer or verification stops the command; it must not be reported as a verified deployment.

Before installation, deployment must complete an independent local `Documents/save` backup in `<work-dir>/build/deploy-ios/save-backups/<unique-run>`. After RomFS verification, it checks the save inventory and hashes again before launch; missing or changed saves stop deployment with the backup path. RomFS sync is nondeleting: unexpected files stop before transfer and suggest a new bundle ID; only missing or mismatched known files are repaired. Never use `devicectl --remove-existing-content` in this flow.

Omit `--team` or `--device` to choose them interactively in a terminal; one connected device is selected automatically. `--device` accepts a UDID or name. Preparation's bundle ID remains `dev.skyhost.skyrim`; deployment offers `dev.skyhost.skyrim.t<lowercase-team-id>` when `--bundle-id` is omitted, reducing collisions between signing teams. A terminal prompts with that default; a noninteractive run uses it automatically and requires `--team` and an unambiguous device selection. Use the same `--work-dir /absolute/path` as preparation for a custom workspace. The default RomFS comes from that workspace's base-cartridge extraction; `--romfs /absolute/path/to/RomFS` is a developer-only override for an existing extraction or a regression run. See [the iOS frontend guide](../../../skyhost/ios/README.md) and [device test instructions](../../../AGENTS.md); use the matching `--bundle-id` for that dedicated device-test configuration. Deployment does not establish base-cartridge gameplay compatibility, which remains unverified under the user's decryption-testing waiver.

For standalone developer builds, `skyhost/ios/build.py` and `skyhost/audio_renderer/build_ios.py` select the bootstrapped local tools automatically. Both accept `--work-dir /absolute/path` or `SKY_WORK_DIR` and run build commands from the repository root, even when invoked from another directory.

The tool versions are .NET SDK **10.0.401**, hactool **1.4.0**, SPIRV-Cross **1.4.363.0**, a Python virtual environment with `lz4` **4.4.5** and `capstone` **5.0.9**, and XcodeGen **2.46.0** for iOS. Downloaded archives are checksum-verified and installed under the ignored `skyrim-work/build/tools/` directory. SPIRV-Cross is built automatically from verified Khronos source with Apple's `clang++` and `ar`; Homebrew and CMake are not required.

The repository's `global.json` pins the official .NET SDK. The root [NuGet.Config](../../../NuGet.Config) selects official nuget.org packages, and audio restore uses checked-in lockfiles for each runtime identifier. The tool environment isolates NuGet caches and .NET CLI state inside the selected workspace; it does not reuse global or Homebrew package caches. Regenerate dependency locks with the official SDK and a clean package cache when intentionally updating them.

Other modes:

```sh
./setup-skyrim.sh --tools-only          # Install local tools without game inputs
./setup-skyrim.sh --tools-only --ios    # Also install XcodeGen
./setup-skyrim.sh --check               # Read-only setup checks
./setup-skyrim.sh --check --ios         # Include iOS requirements
```

`--check` and `--tools-only` never prompt for game paths.

Add `--work-dir /absolute/path` to keep local tools, extracted files, inventory, and rewrites in another workspace. The private shader cache remains in this checkout's ignored `skyrim-work/build/shaders/`. When launching macOS with a custom workspace, pass the same path as `SKY_WORK_DIR`.

The cartridge extractor is `tools/sky_extract_xci.py`, split-dump reader is `tools/sky_dump.py`, inventory generator is `tools/sky_inventory.py`, and rewriter is `tools/sky_patch_nso.py`. The setup command coordinates them; new users do not need to run each manually. The older `tools/sky_extract.sh` is a separate base-plus-update developer workflow, outside the original-cartridge setup path.

## Optional developer verification

`tools/sky_verify_rewrite.sh` runs static rewrite checks. It discovers LLVM's `llvm-objcopy` and `llvm-objdump` on PATH, or accepts `LLVM_BIN` pointing to their directory. LLVM is not needed for normal setup. Activate the local tool environment before manual Python/.NET commands:

```sh
eval "$(python3 tools/sky_tools.py --env)"
tools/sky_verify_rewrite.sh skyrim-work/build/rewrite-native \
  skyrim-work/data/switch/skyrim/base/Update/ExeFS
```

For a custom workspace, add `--work-dir /absolute/path` to `sky_tools.py`. The full host test suite also includes loading an existing save; that extra regression test requires a private `SKY_SAVE_FIXTURE_DIR/Saves/*.sav0` fixture, defaulting to `skyrim-work/saves-fixture/Saves/*.sav0`. Missing test inputs fail the test. A saved-game fixture is not required to prepare the app or start a new game.

## Shaders

Normal setup prepares shaders automatically. For a separate developer rebuild of
the recorded `v327680` cache, use existing game files and installed tools:

```sh
python3 skyhost/tools/rebuild_shaders.py \
  --archive "skyrim-work/data/switch/skyrim/base/Update/RomFS/Data/Skyrim - Shaders.bsa" \
  --main skyrim-work/build/rewrite-native/main.patched.bin \
  --output skyrim-work/build/shaders/rebuild-check
```

This reconstructs 13,232 unique inputs from the archive and 46 embedded in `main`,
translates with the pinned tools, and verifies all 26,556 file hashes through the
recorded aggregate fingerprint before publishing. No runtime captures, old cache,
or additional game files are downloaded. The original `ExeFS/main` NSO is also
accepted. Verification requires game files that reproduce the recorded cache;
base-cartridge compatibility remains unverified. An existing output is never
replaced: for regression checks use a new `--output` under the ignored
`skyrim-work/build/shaders/`. Failed builds retain private diagnostics and are not
published as a verified cache. Setup's `--ensure` mode verifies the source shader
identities before reuse, validates every existing pair against the reference, and
rebuilds only when the destination is missing or empty.

Shared shader source must be independently authored. The renderer's helper shaders and synthetic GPU fixtures meet that requirement. Game captures and their translations do not become project-authored through translation.

The game creates NVN programs while it runs. The host records one only when the game submits it. `skyhost/tools/shaders.py` translates that capture with the vendored Ryujinx translator and spirv-cross. The `.metal` and matching `.json` output is the game's shader in Metal form. These pairs remain private in the ignored `skyrim-work/build/shaders/`, where both macOS and iOS builds look for them. A clone does not include them.

The workflow below can extend a local cache and capture programs reached before a rendering failure. It is not a verified way to bootstrap all game shaders from zero. Missing translations can stop GPU submission before the game reaches menus; do not promise a navigable blank capture run or automatic recovery.

The first run records. `./skyhost/run.sh` builds the host and launches the game. Quit after the required scene or when missing translations prevent progress. A shader that is never submitted is never captured.

```sh
SKY_SHADER_CAPTURE=1 \
SKY_SHADER_CAPTURE_DIR=skyrim-work/build/shader-captures \
./skyhost/run.sh
```

Translate without launching the game. Setup already installs `spirv-cross` and the other translation tools. Activate the local tool environment first:

```sh
eval "$(python3 tools/sky_tools.py --env)"
python3 skyhost/tools/shaders.py \
  skyrim-work/build/shader-captures \
  skyrim-work/build/shaders \
  --validate-metal
```

Verify that captures exist before translating; an empty translation run does not establish a cache. Relaunch `./skyhost/run.sh` with capture unset. The host reads `SKY_SHADER_CACHE_DIR` (default `skyrim-work/build/shaders`) and loads `<shader-key>.metal` plus the matching `.json`. A miss logs `NVN_PROGRAM_SHADER_CACHE_MISSING` and can stop rendering until that program is captured and translated. Repeat as needed; full scene coverage is not guaranteed. Never commit translated game shader pairs, raw captures, translation diagnostics, or logs.

## Run

macOS: `./skyhost/run.sh` selects the local tools after setup. Defaults are `SKY_WORK_DIR=skyrim-work`, rewrite dir `build/rewrite-native`, cartridge RomFS under `data/switch/skyrim/base/Update/RomFS`, and saves in `skyrim-work/run/save`. The private shader cache defaults to this checkout's ignored directory even with another workspace.

iOS: prepare with `./setup-skyrim.sh --ios` and the dump arguments above, then use `./setup-skyrim.sh --deploy-ios` with your team/device choices to sign, install, verify the RomFS transfer, and launch. Signing and device test commands are in `AGENTS.md`; this machine's device UDID, signing team, and bundle ID stay in the untracked `AGENTS.local.md`.
