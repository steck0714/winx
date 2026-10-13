winx-bl33.patch: unified diff (git diff --binary) of the winx tree against
https://github.com/imbushuo/NintendoSwitchPkg commit 09c3ade.
  git clone https://github.com/imbushuo/NintendoSwitchPkg && cd NintendoSwitchPkg
  git checkout 09c3ade && git apply --whitespace=nowarn /path/to/winx-bl33.patch
(Include/FwReleaseInfo.h is generated at build time, see README.winx.md. The docs NOTICE.winx.md /
README.winx.md / LICENSES are only in the tree/zip, not in the patch.)
