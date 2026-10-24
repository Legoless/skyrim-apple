# Native iPhone and iPad frontend

This target uses the Skyrim host, translated Metal shaders, and the real audio DSP. UIKit owns the main thread and starts the guest on an 8 MiB thread after the window and audio session are ready. The touch overlay sends a separate Touch source to the same controller composer used on macOS.

The controls have been checked in iPhone and iPad Simulators and on a physical
iPhone and iPad with actual HID readback. Those controls tests run without
starting Skyrim; they do not establish iOS gameplay compatibility or performance.

## Build

Requirements: Xcode with an iOS SDK, `xcodegen`, Python 3, .NET 10 NativeAOT, rewritten `main`/`sdk` modules, and the translated shader source/reflection cache. Private game data and generated projects remain in the ignored `skyhost/build` folder.

Game shader translations are private and are not included in a clone.
Setup automatically reconstructs and verifies the recorded shader cache before
iOS preparation, or verifies and reuses a matching existing cache. No separate
macOS setup or shader command is required. The recorded reference is from
`v327680`; differing cartridge shader inputs stop setup, and base compatibility
remains unverified. See the [shader workflow](../README.md#local-game-shaders).

From the repository root, prepare the cartridge once and deploy the workspace:

```sh
./setup-skyrim.sh '/path/to/game.xci' --keys /path/to/prod.keys --ios
./setup-skyrim.sh --deploy-ios --team YOUR_TEAM_ID --device DEVICE_UDID_OR_NAME \
  --bundle-id your.bundle.id
```

Deployment requires an account configured in Xcode and a trusted, unlocked device with Developer Mode enabled. It uses the pinned tools, prepares the project for the chosen bundle ID, signs a Release build, installs it using `devicectl`, syncs only `Documents/Skyrim/RomFS`, verifies the files by SHA-256 readback, and launches. Installing over the same bundle ID preserves its container and saves. Transfer or verification failures stop deployment before launch.

Before installation, deployment makes an independent local backup of `Documents/save` under the selected workspace's `build/deploy-ios/save-backups/<unique-run>`. After RomFS verification it requires the save inventory and hashes to remain unchanged before launching; failure reports the backup path. RomFS synchronization never deletes files: unexpected files stop before transfer and suggest a new bundle ID, while missing or mismatched known files are repaired.

The dump and keys are not needed again for deployment. Missing `--team` or `--device` values are prompted in a terminal; a single connected device is selected automatically. `--device` accepts a UDID or name. Preparation defaults to bundle ID `dev.skyhost.skyrim`; deployment offers `dev.skyhost.skyrim.t<lowercase-team-id>` when `--bundle-id` is omitted. A terminal prompts with that default; a noninteractive run uses it automatically and requires `--team` and an unambiguous device selection. Pass the same `--work-dir /absolute/workspace` to both steps for a custom workspace. `--romfs PATH` is a developer-only override for an existing local extraction or regression testing. This does not establish base-cartridge decryption or gameplay compatibility; that validation remains waived by the user.

For lower-level builds, invoke the unsigned app helper directly.
From `skyhost`:

```sh
python3 ios/build.py --sdk simulator \
  --rewrite-dir /path/to/rewrite-native \
  --shader-cache /path/to/shaders

python3 ios/build.py --sdk device --configuration Release \
  --rewrite-dir /path/to/rewrite-native \
  --shader-cache /path/to/shaders
```

The helper publishes the real DSP for the selected platform, embeds signed guest code, prepares metadata and shader resources, generates `build/ios/SkyrimIOS.xcodeproj`, and prints the resulting `.app` path. Direct helper device builds are unsigned; use the deployment command above or configure signing and installation manually in Xcode. `--prepare-only` generates inputs and the project; `--reuse-audio` reuses previously published DSP archives for that platform.

After setup, standalone `ios/build.py` and `audio_renderer/build_ios.py` select the bootstrapped local tools automatically. Pass `--work-dir /absolute/workspace` or set `SKY_WORK_DIR` to select a custom tool workspace; the default is the repository's `skyrim-work`. The app helper also reads its default rewrite inputs from that workspace. Both helpers run build commands from the repository root regardless of the invoking directory.

For a local development install, choose your own team and a unique bundle ID in
Xcode, or build the generated project with automatic signing:

```sh
xcodebuild -project build/ios/SkyrimIOS.xcodeproj -scheme SkyrimIOS \
  -configuration Release -sdk iphoneos -destination 'generic/platform=iOS' \
  -allowProvisioningUpdates CODE_SIGNING_ALLOWED=YES \
  DEVELOPMENT_TEAM=YOUR_TEAM_ID PRODUCT_BUNDLE_IDENTIFIER=your.bundle.id build
```

The iOS Simulator lacks the native BC texture compression used by Skyrim. The
renderer decodes BC blocks on the CPU after Tegra deswizzle and mirrors
buffer-backed linear video planes into private textures. These paths support
real-game rendering in the Simulator, including the touch-driven Journal
scenario. Writable/render-target BC usage remains unsupported, and physical
devices without native BC support are not verified. Passing controls tests or
signed-module startup alone still does not establish game rendering.

`SKY_REWRITE_DIR` and `SKY_SHADER_CACHE_DIR` provide defaults for the input paths. Otherwise rewritten modules come from the selected workspace's `build/rewrite-native`, while private game shaders remain in this checkout's ignored `skyrim-work/build/shaders`. No RomFS is copied into the app by the helper.

## Game files

`--deploy-ios` transfers and verifies the extracted RomFS automatically. For a manual install, copy RomFS into the app's Documents folder as `Skyrim/RomFS`, preserving `Skyrim.ini`, `Data`, and all other contents. File Sharing is enabled for Finder and Files. The app shows setup instructions until those files are present. Return to the app after copying, or tap Retry; it rechecks the files without requiring a force quit. Saves are stored separately in `Documents/save`.

Simulator runs can use an existing extracted directory:

```sh
SIMCTL_CHILD_SKY_ROMFS=/absolute/path/to/RomFS \
  xcrun simctl launch DEVICE_UUID dev.skyhost.skyrim
```

The app uses bundled rewritten-module metadata and translated shader resources. `SKY_SAVE_DIR` and `SKY_SHADER_CACHE_DIR` can override their runtime paths for development.

## Controls and verification

The landscape overlay shows icon buttons for Sneak, Jump, Use and Attack in a column on the right edge, above a lower-right corner kept clear for Look, plus Pause. Buttons follow Skyrim's menu frames: ivory corner brackets on translucent black squares. Move and Look are floating sticks: a touch on the left half of the screen that misses a button places the Move stick under that finger, and the right half does the same for Look. Each stick is drawn only while held. The character sprints whenever Move is deflected, except while Sneak is toggled on, because sprinting would make the player stand up. Pause sits in the top-left corner. In menus, the gameplay overlay hides except for a Back button in that same corner, and other screen touches dispatch directly to Scaleform menus (tap to select or confirm, drag to navigate, two-finger tap to return). Game text and character-name requests open a native text field with Done and Cancel; the SDK’s UTF-16 length limits are preserved. Resizing, safe-area changes, interruptions, and loss of activity release only touch input.
The app keeps the display awake during active play and restores normal idle
behavior when inactive, during setup, or after the guest finishes.
Direct menu taps and the menu-specific overlay above require the verified
update profile. Unrecognized cartridge Build IDs use generic controller input
with Back (B), Pause, Use (A), and sticks; direct tapping of their menus has not
been established.
The action labels describe gameplay; the game retains its Switch button prompts
(with the verified update profile's title prompt translated to `Touch to begin`). Add-on shop
requests close immediately in this offline build.

```sh
make build/t_touch
build/t_touch

python3 ios/build.py --sdk simulator --reuse-audio --test \
  --destination 'platform=iOS Simulator,id=DEVICE_UUID'
```

The XCTest scheme explicitly passes `--controls-test-mode` and `SKY_CONTROLS_TEST_MODE=1`. That mode displays the UIKit frontend and does not load or start the guest. It tests control composition, multi-touch ownership, accessibility, bounds, resize cancellation, frontend lifecycle behavior, native text confirmation/cancellation, Unicode limits, and retrying after game files are copied. It never serves as game-rendering evidence.

UIKit accessibility identifiers begin with `sky.touch.`. In-process diagnostic hooks in `frontend.h` use overlay points and must run on the main thread.

The app requests only the increased memory limit and extended virtual address space entitlements. Guest code is embedded in the signed executable; iOS does not use the macOS executable-memory mapper or mutable Havok observer trampolines.
