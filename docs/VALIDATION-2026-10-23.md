# Shader setup and Simulator validation — 2026-10-23

The user requested finishing the pending setup/shader work and using Simulator
for this validation. No physical device was installed, launched, or modified.

## Changes

The pending batch keeps game shader translations private, replaces the GPU test
fixtures with independently authored programs, rebuilds the recorded cache from
existing game inputs during setup, and discovers optional dump/key inputs in
the ignored repository input directory.

The readiness check now uses the same dependency-free fingerprint and reference
as shader rebuilding. It rejects incomplete or corrupted paired caches,
unexpected shader names, and symbolic links. Only absent or genuinely empty
caches are allowed through the initial bootstrap check. Verification reads the
cache without repairing it or writing bytecode. Independent review found no
remaining issue in this fix.

## Results

| Check | Result |
| --- | --- |
| Pinned Python tooling suite | 169 tests passed |
| Shader Python suites | 31 tests passed; the 14 rebuild tests were rerun after the shared-helper change |
| Native authored GPU fixtures | `t_nvn_commands`, `t_nvn_render_state`, `bench_nvn_pipeline`, and `t_touch` passed with Metal validation |
| GPU render-state and pipeline coverage | 78 render-state cases and 10,496 benchmark draws, including pixel/depth checks; not gameplay FPS evidence |
| iPad Pro 11-inch Simulator, iOS 27.0, Xcode 27.1 | 11 UIKit controls tests passed, zero skipped |
| Release Journal gameplay smoke on that Simulator | One test failed, zero skipped |
| Private shader cache after testing | All 26,556 reference files matched; 13,278 pairs and 323,194,827 bytes |
| Original save fixture | Unchanged; Simulator used an independent copy |

The game smoke test used the existing prepared `v327680` workspace. It failed
because the app exited before reaching the Journal. The runtime log records
unsupported BC texture-compression formats on the Simulator GPU and a Metal
assertion requiring private storage for buffer-backed linear textures. These
are the Simulator rendering limitations already documented in the iOS guide.
No failure was converted into a skip or a successful gameplay claim.

All six XCUITest screenshots were reviewed: they show Home Screen/orientation
transitions, one white frame, and the final Home Screen after the app exited.
None establishes a game menu, gameplay, or Journal interaction. The separate
controls-mode screenshot confirms only the native overlay test host.

The default `./setup-skyrim.sh --check --ios` still reports missing cartridge
`ExeFS/main`: this local workspace contains the restored RomFS and rewritten
modules, not a complete original-cartridge extraction. Original-cartridge
decryption/gameplay validation remains waived and unverified.

## Evidence and remaining scope

Private logs, result bundles, screenshots, commands, input-copy receipts, and
preservation checks are under
`skyrim-work/build/simulator-validation-20261023/`. Controls XCTest output is also
under `skyhost/build/ios/DerivedData-SimulatorValidation/Logs/Test/`.

Full Simulator gameplay requires separate renderer compatibility work for the
reported texture limitations. This validation does not replace a supported
device gameplay result. The shader cache remains on disk and ignored by Git.
The private history that contained game-derived shaders is excluded from this
sanitized source distribution.
