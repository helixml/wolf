# Vendored inputtino

This is a vendored copy of [inputtino](https://github.com/games-on-whales/inputtino)
from commit `fd136cfe492b4375b4507718bcca1f044588fc6f`.

## Why vendored?

We needed to fix a thread-safety bug in the keyboard auto-repeat implementation
that was causing keys to get stuck or stop working on RHEL (kernel 5.14).

## Changes from upstream

See git history for this directory. Key changes:
- Removed the buggy auto-repeat thread in `src/uinput/keyboard.cpp`
