# Skyrim on Apple platforms

`skyhost` loads the rewritten ARM64 Skyrim and SDK modules and replaces Switch
OS, NVN, display, audio, and input boundaries with native host implementations.
Game code, assets, and translated game shaders remain local in the ignored
`skyrim-work` directory. Only project-authored helper and test shaders are shared.
This is an
unfinished port. The prior `v327680` update build reached the main menu, New
Game confirmation, and moving opening cart intro with verified native Metal
rendering and CoreAudio output. Its latest
offscreen intro sample averaged 5.1 completed frames per second while compiling
new shaders; performance varies by scene, and later gameplay is unverified.
The same route is also verified offscreen using the background controls below.
The Switch account applet is not implemented; Change User immediately selects
the sole local offline account.
Add-on shop requests close immediately in this offline build.

## Setup and run

```sh
./setup-skyrim.sh
./skyhost/run.sh
```

Place one `.xci` file or split-dump `.xci` directory and `prod.keys` in the
repository's ignored `inputs/` folder, then run these from the repository root.
Setup discovers those files automatically and only asks for paths still missing
when running in a terminal. For explicit paths, use
`./setup-skyrim.sh '/path/to/game.xci' --keys /path/to/prod.keys`; missing paths
fail without prompting outside a terminal. The positional input accepts any
split-dump directory, including a nested `.xci` directory containing numbered
parts, or a single XCI file at any path. Setup joins split parts into a verified local
cache and ignores certificate/initial-data sidecars; no manual repacking is
required. `--base` is an alternative flag for the same input.

Setup targets the original base cartridge, content version `0`, using your
console's `prod.keys` for encrypted NCAs. No update NSP is required or
downloaded. Bootstrap does not download game packages or keys. The setup
command installs verified, pinned local tools, extracts ExeFS/RomFS/Control,
records the cartridge's NSO identities, generates inventory, rewrites
`main`/`sdk` using its Control NACP save settings, and builds the macOS host.

Base-cartridge decryption and gameplay have not been verified in this
environment; the user waived testing with their keys. The setup validation
covers tool builds and synthetic checks. Unrecognized cartridge Build IDs use
generic controller input, with on-screen Back (B), Pause, Use (A), and sticks.
Private touch-menu hooks remain restricted to the matching verified update
profile; direct tapping of base-game menus is not established.

The supported build platform is an Apple Silicon Mac with full Xcode and its
license accepted, Python 3.9+, and internet for initial tool downloads and
NuGet restore. Linux and Windows are not supported. The wrapper uses an
explicit `SKY_PYTHON` override or `python3` on PATH, falling back to
`/usr/bin/python3`. The bootstrap installs .NET SDK 10.0.401, hactool 1.4.0,
SPIRV-Cross 1.4.363.0, and a virtual environment with lz4 4.4.5 and capstone
5.0.9 under ignored `skyrim-work/build/tools/`. It does not require Homebrew or manual installation
of those tools. SPIRV-Cross builds from verified Khronos source using Apple's
`clang++` and `ar`, with no CMake dependency. `global.json` pins the .NET SDK;
the official SDK restores audio dependencies using checked-in per-runtime
lockfiles and the repository's [official nuget.org configuration](../NuGet.Config).
The tool environment keeps NuGet caches and .NET CLI state in the selected
workspace, so global or Homebrew package caches do not supply build dependencies.
`./setup-skyrim.sh --tools-only` installs tools without game inputs;
`./setup-skyrim.sh --check` checks setup without making changes. Both modes
run without game-path prompts.

The launcher builds the host, opens a native macOS Metal window, and runs the
original game on a guest thread. It selects the bootstrapped local tools.
The default paths match the extracted base-cartridge workspace at
`skyrim-work/data/switch/skyrim/base/Update`; `Update` is a retained directory
name and does not imply an update package. Use
`--work-dir /absolute/path` during setup and `SKY_WORK_DIR=/absolute/path` when
launching to choose another local workspace. `SKY_REWRITE_DIR`, `SKY_ROMFS`,
`SKY_SAVE_DIR`, and `SKY_SHADER_CACHE_DIR` override individual runtime paths.
The private shader cache defaults to this checkout's `skyrim-work/build/shaders/`
even with a custom workspace. Saves are written separately from the read-only
game assets; no existing save is needed to set up or start a new game.

The audio renderer is built as a native library automatically;
its source provenance and ABI are documented in [audio_renderer](audio_renderer/README.md).

Add `--ios` to the setup command to prepare the device Release DSP and
`skyhost/build/ios/SkyrimIOS.xcodeproj` instead. This also installs XcodeGen
2.46.0. The bundle ID defaults to `dev.skyhost.skyrim`; override it with
`--bundle-id your.bundle.id`. After preparation, deploy without supplying the
dump or keys again:

```sh
./setup-skyrim.sh --deploy-ios --team YOUR_TEAM_ID --device DEVICE_UDID_OR_NAME \
  --bundle-id your.bundle.id
```

Configure your Xcode account and connect a trusted, unlocked device with
Developer Mode enabled. Deployment selects the local pinned tools, prepares the
project for the chosen bundle ID, signs a Release build, and installs it while
preserving an existing matching app's container and saves. It syncs only
`Documents/Skyrim/RomFS`, verifies SHA-256 hashes by reading the files back, then
launches. Before installation, it makes an independent local `Documents/save`
backup under the selected workspace's `build/deploy-ios/save-backups/<unique-run>`.
After RomFS verification, it checks that the save inventory and hashes are
unchanged before launching; failure stops deployment and reports the backup path.
RomFS sync never deletes files. Unexpected files stop before transfer and suggest
a new bundle ID; only missing or mismatched known files are repaired.
Transfer or verification failures stop deployment. Missing team/device
choices are prompted in a terminal; a single connected device is selected
automatically. Deployment's default bundle ID is
`dev.skyhost.skyrim.t<lowercase-team-id>`; a terminal prompts with this default,
while noninteractive deployment uses it automatically and requires `--team`
and an unambiguous device selection. Pass the same `--work-dir` used during
preparation. `--romfs PATH` is a developer-only override for an existing
extraction or a regression run.

`--tools-only --ios` and `--check --ios` include the iOS tooling. See
[the setup guide](../.cursor/skills/skyrim-setup/SKILL.md) and
[iOS frontend guide](ios/README.md).

WASD moves and F1 toggles mouse look. With the cursor captured, left/right click
uses the right/left hand; Escape releases the cursor and opens the journal.
With the cursor free, arrows or the wheel navigate menus, Return/E/left click
confirms the selected item, and Escape/right click goes back. Menu text does not
have native pointer hit testing. Tab opens the character menu and releases the
cursor. Losing focus releases desktop input and restores the cursor.

Space jumps, E interacts, R readies/sheathes, F changes perspective, Shift
sprints, Ctrl/C sneaks, Z shouts, T waits, Q opens favorites, and 1/2 select
hotkeys. IJKL also controls look; =/- are Plus/Minus. The game retains its Switch
button prompts. Set `SKY_MOUSE_SENSITIVITY` to a positive stick-units-per-pixel
value to change the default 1600. See [CONTROLS.md](CONTROLS.md) for input behavior.
The game's software-keyboard requests use a native text dialog, including
Unicode text, confirmation and cancellation.

Cmd-Q or closing the window exits. Interactive runs have no watchdog by default; set
`SKY_RUNTIME_TIMEOUT` to a positive number of seconds for a bounded diagnostic
run. `./skyhost/run.sh --headless` suppresses the window while retaining real
Metal rendering and the game's requested presentation interval.

For background control without desktop focus, run
`SKY_INPUT_STDIN=1 ./skyhost/run.sh --headless` and keep its standard input open.
Send newline-separated commands. `state BUTTONS LX LY RX RY` holds buttons and
sticks, with stick values from -32768 to 32767. Button masks are `0x1` for A,
`0x2` for B, `0x4` for X (Change User), and `0x8000` / `0x2000` for down / up.
Menu tests use a normal press: for example, send `state 0x8000 0 0 0 0`, wait
about 0.3 seconds, then send `state 0 0 0 0 0` to release. `tap MASK` queues a
single-sample pulse; these very short pulses can occasionally skip menu entries.
`state 0 0 0 32767 0` holds the right stick to look right.
`reset` releases all input and `quit` exits normally. EOF releases input without
exiting the game. Frame capture below provides the actual offscreen image.

## Local game shaders

The game creates NVN programs while it
runs, and the host records a program only when that program is submitted.
Translation uses the vendored Ryujinx translator and spirv-cross. It does not
author a new shader. The resulting `.metal` files and matching `.json` metadata
are private game-derived files, kept in the ignored `skyrim-work/build/shaders`.
Both macOS and iOS builds use this location by default; a clone has no game cache.

Normal `./setup-skyrim.sh` and `./setup-skyrim.sh --ios` runs prepare the shader
cache automatically after extraction and rewriting, before building the host or
device project. Existing cache contents and input shader identities are verified
before reuse. Missing or empty caches are rebuilt. A changed or incomplete cache
is preserved and causes setup to stop; move it to a backup directory and rerun.

The recorded `v327680` cache is reproduced **byte-for-byte from the game's shader
archive and main module**, without runtime captures or playing through scenes.
For a separate developer rebuild using existing extracted files and installed tools:

```sh
python3 skyhost/tools/rebuild_shaders.py \
  --archive "skyrim-work/data/switch/skyrim/base/Update/RomFS/Data/Skyrim - Shaders.bsa" \
  --main skyrim-work/build/rewrite-native/main.patched.bin \
  --output skyrim-work/build/shaders/rebuild-check
```

The command extracts 13,232 unique programs from the archive and 46 from `main`,
translates them with the pinned tools, and verifies all 26,556 output files against
the recorded cache fingerprint before publishing the new cache. It accepts the
original `ExeFS/main` NSO as an alternative to `main.patched.bin`. It distributes
only the reference fingerprint, not shader contents or captures. Different game
files or changed translator output fail verification; this is not a promise that
the base cartridge has identical shaders. Base-cartridge extraction and gameplay
remain unverified.

Use a new output directory for each developer rebuild. `--ensure`, used by setup,
allows verification/reuse of an existing matching cache or filling an empty
directory; it never replaces existing files. `--work-dir` selects the installed
local tools; the default cache destination remains in this checkout. Failed
attempts retain private artifacts at the path printed by the command.

The following runtime capture workflow is for development beyond the recorded
cache. It is not part of normal setup. Rendering needs translated shaders, and
runtime capture from an empty cache remains unverified. Missing shaders can stop GPU
command submission before the game reaches an interactive menu.
The following developer workflow can capture programs reached before that failure
and extend an existing cache. It does not guarantee progress through every scene.

First, record programs from your own game. `./skyhost/run.sh` builds the host
and launches the game. Submitted programs land in the capture directory.
Quit after the required scene or when a missing shader prevents progress.

```sh
SKY_SHADER_CAPTURE=1 \
SKY_SHADER_CAPTURE_DIR=skyrim-work/build/shader-captures \
./skyhost/run.sh
```

Translate without launching the game. The bootstrap supplies `spirv-cross`;
activate its tool environment below for this optional developer workflow.
LLVM is needed only for the separate independent rewrite verification helper.
The translator binary is built from the repo on the first run.

```sh
eval "$(python3 tools/sky_tools.py --env)"
python3 skyhost/tools/shaders.py \
  skyrim-work/build/shader-captures \
  skyrim-work/build/shaders \
  --validate-metal
```

Check that the capture directory contains shader metadata and matching binary
programs before translating; an empty translation run is not evidence of a cache.
Then launch `./skyhost/run.sh` with capture unset. The host
loads `$SKY_SHADER_CACHE_DIR/<shader-key>.metal` and the matching `.json`
(default `skyrim-work/build/shaders`). A shader that was not captured logs
`NVN_PROGRAM_SHADER_CACHE_MISSING` and can stop rendering until it is captured
and translated. Repeat when further missing programs are encountered. Keep
all game-derived shader pairs, raw captures, translation diagnostics, game files,
and runtime logs private and out of Git.

## Verification target

Completion requires observing the real rendered main menu, navigating to New
Game, confirming it, and seeing the opening cart scene progress with game input
and audio. Preserve runtime logs and screenshots or video in the external run
directory. Check that they come from the current source build. Neither passing
unit tests, an empty window, translated shader files, nor reaching an explicit
unsupported-call boundary proves this target.

`make -C skyhost test` runs the host tests and the real-input integration test.
The integration test must fail on missing inputs, crashes, unsupported calls,
or watchdog timeouts; do not skip unavailable inputs or count an intentional
stop at an old implementation boundary as game completion.
The full suite's save-load regression additionally needs a private directory
containing `Saves/*.sav0`, selected with `SKY_SAVE_FIXTURE_DIR` (default
`skyrim-work/saves-fixture`). This fixture is an extra test input, not a setup
or play requirement.

`SKY_TRACE_SVC=1` enables every post-entry syscall diagnostic. By default these
logs are bounded per syscall while the latest checkpoint is still updated on
every call, so guest polling does not flood the log or stall startup.

For visual diagnostics, `SKY_FRAME_CAPTURE=/path/frame.ppm` captures the actual
completed render target. `SKY_FRAME_CAPTURE_EVERY=15` refreshes it every fifteen
presentations. Native window presentation applies the game's crop; the diagnostic
file contains the whole backing texture. `SKY_PRESENT_LIMIT=3` exits after the
requested number of completed frames with genuine GPU work and no GPU errors.

## iPhone and iPad

The [iOS frontend](ios/README.md) builds a separate native app with landscape
on-screen Move/Look sticks and action buttons. It uses the same guest loader,
Metal renderer and audio DSP. Build instructions, game-file setup and the UIKit
test command are in that guide. Phone and tablet controls tests pass; full iOS
Skyrim gameplay was confirmed by the user on a physical iPad at approximately 5 fps.
The current performance target is 30+ fps; later-game compatibility remains
unverified.

## Performance measurement

The renderer shares a Metal render encoder across compatible consecutive draws,
preserving explicit barriers and ending the pass for copies, compute, clears,
counters, synchronization and attachment changes. Draws with writable shader
resources or attachment feedback remain isolated. Depth/stencil descriptors are
cached, and descriptor resolution retains a registry snapshot under one lock
per used registry. Set `SKY_METAL_REUSE_ENCODERS=0` to compare the same build with
per-draw render passes.

`SKY_METAL_PROFILE=1` enables bounded `NVN_METAL_PROFILE` aggregate diagnostics:
CPU encoding time, synchronous GPU waits, completed command-buffer GPU time,
encoder counts and merge exclusions. These counters are cumulative per queue;
compare differences within a warm scene. GPU command-buffer duration is not a
frame-time or FPS measurement. On a visible Metal layer, use differences in
`NVN_DRAWABLE_PRESENTED` count and `time_ns` to measure actual presentation FPS.
Disable frame readback during performance runs.

`make -C skyhost build/bench_nvn_pipeline` builds the controlled renderer
benchmark. Run it with encoder reuse enabled and disabled in separate processes.
An initial Mac comparison reduced render encoders from 10,508 to 28 over 10,499
draws while preserving the rendered color/depth results. This small-target
workload does not establish gameplay FPS on iPad. The measured iPad baseline
remains approximately 5–6 fps in gameplay; 30+ fps requires a new device run.

macOS startup intermittently hits Havok's "Too many threads" assertion, including
one performance-baseline attempt followed by a successful retry. Its cause is
not established. For diagnostics,
`SKY_HAVOK_TRACE=buffered` retains thread registration history without per-call
logging and dumps it on a fatal signal; `SKY_HAVOK_DIAG=1` also dumps slot state.
