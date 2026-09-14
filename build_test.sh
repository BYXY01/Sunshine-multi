#!/bin/bash
export PATH="/c/Program Files/nodejs:$PATH"
export NPM="$(which npm)"
ninja -C cmake-build-windows tests/test_sunshine.exe
