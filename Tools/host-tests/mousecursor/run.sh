#!/bin/bash
# Host test of Drivers/MouseCursorDxe/MouseCursorDxe.c against a fake GOP framebuffer and fake pointer.
set -e
cd "$(dirname "$0")"
gcc -w -I shim -o /tmp/mct t.c && /tmp/mct
