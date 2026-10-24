# Agent instructions

## New machine

A clone does not include the game. Follow `.cursor/skills/skyrim-setup/SKILL.md`. Place exactly one `.xci` file or split-dump `.xci` directory and `prod.keys` in the repository-root `inputs/` folder, then run `./setup-skyrim.sh`; omitted paths are discovered there automatically. Everything in `inputs/` except its placement guide is ignored by Git. Explicit paths override discovery, multiple dump candidates fail, and missing inputs prompt only in a terminal. For external paths, use `./setup-skyrim.sh '/path/to/game.xci' --keys /path/to/prod.keys`; missing inputs fail without prompting outside a terminal. The positional input (or `--base`) accepts any XCI file or split-dump directory, including a nested `.xci` directory with numbered parts; setup joins and verifies a local cache without changing the original or requiring manual repacking. The target is the base cartridge, content version `0`; no update NSP is required or downloaded. Setup downloads verified, pinned local tools, including SPIRV-Cross 1.4.363.0, extracts ExeFS, RomFS and Control NACP, records module identities and inventory, rewrites `main`/`sdk` using the cartridge's save settings, then builds the macOS host. Add `--ios` to prepare the device Release project and DSP with default bundle ID `dev.skyhost.skyrim` (override with `--bundle-id`); signing credentials and copying RomFS to the device are separate. Unrecognized cartridge Build IDs use generic controller input with on-screen Back (B), Pause, Use (A), and sticks; private touch-menu hooks remain restricted to the verified update profile. Base-game decryption/gameplay has not been verified with user-provided keys; the user waived that verification, so report build and synthetic checks separately. Use `--check [--ios]` for read-only checks or `--tools-only [--ios]` to install tools without game inputs; these modes never prompt. The supported platform is an Apple Silicon Mac with full Xcode and its license accepted, Python 3.9+, and internet for initial tool/NuGet downloads. The wrapper uses `SKY_PYTHON` when explicitly set, otherwise `python3` on PATH, with `/usr/bin/python3` as a fallback. No Homebrew setup is needed. Keep translated game `.metal` shaders and matching `.json` metadata private in the ignored `skyrim-work/build/shaders/`, even when `--work-dir` selects another local workspace. Only independently authored helper/test shaders belong in source control. Both macOS and iOS setup automatically call `skyhost/tools/rebuild_shaders.py --ensure` after extraction and rewriting. It validates source shader identities, rebuilds missing/empty caches, or verifies and reuses matching existing files against the recorded `v327680` reference (26,556 files). Existing mismatched or incomplete caches are preserved and rejected. No separate shader command or macOS preparation pass is required for iOS. `--check` remains read-only and requires local translations. Base-cartridge cache compatibility and runtime capture from zero remain unverified; different game shader inputs must fail instead of reusing an incompatible cache. Missing shaders can stop rendering before menus. Never download game packages or keys as tooling. Leave keys, RomFS, `*.patched.bin`, raw shader captures, translated game shader pairs, and runtime logs out of Git. Preserve local caches when changing tracking. Never publish game-derived shaders or any repository history containing them. Touch controls are already source in `skyhost/`.

After iOS preparation, `./setup-skyrim.sh --deploy-ios --team TEAMID --device UDID_OR_NAME --bundle-id your.bundle.id` deploys the prepared workspace without needing the dump or console keys again. It uses the pinned tools, prepares the chosen bundle ID, signs a Release build, installs over the matching app while preserving its container and saves, syncs only `Documents/Skyrim/RomFS`, verifies the files by SHA-256 readback, then launches. A failed transfer or verification must stop deployment and must not be reported as verified. Xcode must have the user's signing account configured, and the device must be trusted, unlocked, and in Developer Mode. Missing team/device choices prompt in a terminal; one connected device is selected automatically. Deployment offers bundle ID `dev.skyhost.skyrim.t<lowercase-team-id>` when `--bundle-id` is omitted, prompting with that default in a terminal and using it automatically outside a terminal. Noninteractive deployment requires `--team` and an unambiguous device selection. Pass the same `--work-dir` as preparation for a custom workspace; `--romfs PATH` is a developer-only existing-extraction/regression override. This deployment flow does not establish original-cartridge gameplay compatibility.

Deployment must make an independent local backup of `Documents/save` before installation at `<work-dir>/build/deploy-ios/save-backups/<unique-run>`, then verify the save inventory and hashes remain unchanged after RomFS verification and before launch. If saves are lost or changed, stop and report the backup path. RomFS synchronization never deletes files or uses `--remove-existing-content`: unexpected files must fail before transfer and suggest a new bundle ID; repair only missing or mismatched known files. Keep save backups and recovery data private and out of Git.

## Testing

Read `AGENTS.local.md` (untracked) for this machine's device UDID, signing team,
and installed bundle ID. Keep those values out of tracked files, docs, and
commit messages.

- Always test iOS changes on the user's physical iPad Pro. Simulator runs are
  optional extras, never a substitute.
- The device runs the configured bundle ID with the game files already copied.
  Installing over it keeps the data container.
- Run both schemes from the repository root with the generated project in
  `skyhost/build/ios`. Run the Release smoke scheme last, so the device is left
  with a playable Release build instead of the Debug test host:

  ```sh
  # UIKit controls tests (controls test mode; the guest is not started)
  xcodebuild test -project skyhost/build/ios/SkyrimIOS.xcodeproj -scheme SkyrimIOS \
    -configuration Debug -destination 'id=<DEVICE_UDID>' \
    -derivedDataPath skyhost/build/ios/DerivedData -allowProvisioningUpdates \
    DEVELOPMENT_TEAM=<TEAM_ID> CODE_SIGN_STYLE=Automatic CODE_SIGN_IDENTITY="Apple Development"
  # Real game: continues the save, taps Pause, captures Journal screenshots
  xcodebuild test -project skyhost/build/ios/SkyrimIOS.xcodeproj -scheme SkyrimGameControlsSmoke \
    -destination 'id=<DEVICE_UDID>' \
    -derivedDataPath skyhost/build/ios/DerivedData -allowProvisioningUpdates \
    DEVELOPMENT_TEAM=<TEAM_ID> CODE_SIGN_STYLE=Automatic CODE_SIGN_IDENTITY="Apple Development" \
    -only-testing:GameControlsUITests/GameControlsUITests/testJournalTabsSwitchWithTouch
  ```

  Then relaunch the game for the user:
  `xcrun devicectl device process launch --device <DEVICE_UDID> --terminate-existing <BUNDLE_ID>`.

- Review screenshots from the XCUITest attachments
  (`xcrun xcresulttool export attachments --path <xcresult> --output-path <dir>`).
  `idevicescreenshot` does not work on the test iPad's iOS version.
