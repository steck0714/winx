#!/bin/bash
# Host test of Library/FbExceptionHandlerLib/FbText.c: renders a sample dump for each rotation mode into PGM files.
set -e
cd "$(dirname "$0")"
for R in "" 1 2; do gcc -w -I shim -I ../../../Include ${R:+-DEXC_FB_ROTATE=$R} -o /tmp/fbt$R t.c; (cd /tmp && ./fbt$R && mv out.pgm fbtext_out${R:-N}.pgm); done
echo "wrote /tmp/fbtext_out{N,1,2}.pgm"
