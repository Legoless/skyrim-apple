# Put your game inputs here

Place your own Switch Skyrim cartridge dump and console keys in this directory:

```text
inputs/
  Skyrim.xci
  prod.keys
```

The XCI can have any filename. Keep exactly one `.xci` file or split-dump `.xci`
directory here. For a split dump, keep the numbered parts together:

```text
inputs/
  Skyrim.xci/
    00
    01
    02
    03
  prod.keys
```

From the repository root, run `./prepare-skyrim.sh` (to choose Mac, iOS prepare,
iOS deploy, or all interactively) or `./setup-skyrim.sh` / `./setup-skyrim.sh --ios`.
Both discover these inputs automatically, or prompt in a terminal if `inputs/` is
empty. Explicit dump paths and `--keys` override this location.

Everything in this directory except this guide is ignored by Git. Do not force-add
game files or keys. Setup reads your originals without moving or modifying them;
generated files go into the ignored `skyrim-work/` workspace.

The exact shader reference is from `v327680`; base-cartridge compatibility remains
unverified, and mismatching shader inputs stop setup.
