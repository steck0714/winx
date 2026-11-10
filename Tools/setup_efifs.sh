#!/bin/bash
# Prepares the upstream EfiFsPkg sources for `-D SWITCH_FSDRV=1`.
# Usage: Tools/setup_efifs.sh <edk2 dir>
# Upstream: pbatard/EfiFs v1.13 (GPLv3+), pinned below; GRUB is fetched from its upstream submodule.
set -e
EDK2=${1:?usage: setup_efifs.sh <edk2 dir>}
EDK2=$(cd "$EDK2" && pwd)
DEST="$EDK2/EfiFsPkg"
EFIFS_TAG_COMMIT=0f1b63b92f61f23fe17697d24df7164f1d13fa1f

# Never replace an existing package: preserve any local/user data and require an explicit
# manual backup/relocation before a version change.
if [ -e "$DEST" ]; then
  echo "Refusing to modify existing path: $DEST" >&2
  exit 1
fi

git clone --depth 1 --branch v1.13 --recurse-submodules \
  https://github.com/pbatard/efifs.git "$DEST"
ACTUAL_COMMIT=$(git -C "$DEST" rev-parse HEAD)
if [ "$ACTUAL_COMMIT" != "$EFIFS_TAG_COMMIT" ]; then
  echo "Unexpected EfiFs v1.13 commit: $ACTUAL_COMMIT" >&2
  exit 1
fi

# The upstream EDK2 build uses this patch for GRUB's firmware-facing code.
(cd "$DEST/grub" && git apply --check ../0001-GRUB-fixes.patch && git apply ../0001-GRUB-fixes.patch)
# The upstream helper removes an existing `cpu` path before linking it. Keep this
# setup non-destructive: create the architecture link only when the path is absent.
GRUB_CPU_DIR="$DEST/grub/include/grub"
mkdir -p "$GRUB_CPU_DIR"
if [ -e "$GRUB_CPU_DIR/cpu" ] || [ -L "$GRUB_CPU_DIR/cpu" ]; then
  echo "Refusing to replace existing path: $GRUB_CPU_DIR/cpu" >&2
  exit 1
fi
ln -s arm64 "$GRUB_CPU_DIR/cpu"
echo "EfiFs v1.13 ready in $DEST. Build with -D SWITCH_FSDRV=1 -D SWITCH_BIGFD=1 (read-only drivers; see README.fsdrv)."
