# Simulator gameplay parity

## Goal

Run the same prepared `v327680` save and real touch-driven Journal scenario used
on iPad in the iOS Simulator. The app must load the game, open the Journal, and
respond visibly to its tab controls. FPS is not an acceptance criterion for this
work. Controls-test mode, a blank window, or an app that merely stays alive do
not establish gameplay parity.

Baseline: commit `44928a25`. The previous Simulator run passed all 11 native
controls tests but the real game stopped on unsupported BC texture compression
and shared-buffer linear-texture restrictions. Evidence is retained privately in
`skyrim-work/build/simulator-validation-20261023/`.

## Implementation constraints

- Preserve native texture paths on hardware that already supports them.
- On devices without BC sampling, decode the compressed blocks after the
  existing Tegra deswizzle step. Preserve logical BC format identity separately
  from physical Metal storage, including sRGB, signed channels, mip levels,
  partial writes and compatible views.
- Use a pinned, independently authored BC decoder with its upstream license.
  No game shader or texture content is introduced into source control.
- For Simulator video planes, retain the source buffer and refresh a private R8
  texture in the same GPU command stream before use. Preserve later CPU writes,
  view lifetime, padding and copy-back behavior; a one-time upload is insufficient.
- Preserve command ordering. CPU decoding must not read a buffer before queued
  GPU writes complete. A conservative synchronization boundary is acceptable.
- Reject unsupported operations explicitly. Do not replace missing textures
  with fake images or count a stopped guest as success.
- Use independent Simulator save copies. Preserve existing game inputs, private
  caches and original saves. Back up and verify physical-device saves before
  validation, and leave the device with a playable Release build.

## Verification

1. Synthetic decoder tests cover formats, signed values, tiny/odd mip edges,
   strides, input bounds and output bounds.
2. Native GPU tests force the compatibility paths and verify sampled pixels,
   sRGB behavior, views, partial updates, lifetimes and ordered transfers.
3. Rebuild the Simulator app and rerun the native controls suite.
4. Run the Release `testJournalTabsSwitchWithTouch` scenario with real game
   inputs. Review screenshots and runtime logs for actual gameplay and Journal
   transitions. Investigate further concrete compatibility failures if encountered.
5. When physical validation is required, run the physical-iPad controls and
   Release Journal schemes, review their artifacts, verify save preservation and
   relaunch Release. If the user explicitly selects Simulator-only acceptance,
   record that choice and leave physical hardware coverage unverified.

The source/base-cartridge verification waiver is unchanged: this task uses the
existing prepared update-profile inputs and does not establish base-cartridge
decryption or compatibility.

## Results — 2026-10-24

- All 172 setup/deployment tooling tests passed. Native Metal checks passed,
  including a fixed 16x16 packaged-BC deswizzle/decode/view regression and a
  40-level linear-mirror view regression. The complete `make -C skyhost test`
  suite passed after `tools/sky_inventory.py` regenerated metadata directly from
  `skyrim-work/build/rewrite-native`: all 24 map tests and both load hashes were
  stable. Real macOS startup completed three frames in 6.6 seconds; save-load
  passed in 84.3 seconds. Regenerated metadata is private under
  `skyrim-work/build/inventory-from-rewritten/meta`.
- All 12 Simulator UIKit controls tests passed with zero skipped. The Release
  Journal smoke completed in 80.2 seconds and exited successfully with zero
  skipped. Reviewed screenshots show gameplay, the Journal tutorial and its
  dismissal, General Stats, Quests and System. The runtime log contains matching
  menu-stack and tab-touch transitions and no GPU errors. Evidence is under
  `skyrim-work/build/candidate-validation/controls-02.xcresult` and
  `skyrim-work/build/candidate-validation/journal-06-*`.
- The current unsigned device Release app built successfully.
- Physical iPhone 17 Pro Max validation also passed all 12 controls tests and the
  85.2-second Release Journal smoke with zero skipped. Reviewed screenshots and
  runtime logs confirmed the same Journal and tab transitions with no GPU errors.
  All 185 RomFS files (13,277,166,913 bytes) matched with zero copied files;
  three saves were backed up before installation and verified unchanged afterward.
  Evidence is retained privately under `skyrim-work/build/candidate-validation/`.
