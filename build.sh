#!/bin/bash

echo "--- 1. Building Docker Environment ---"
docker build -t omnisave-builder ./sysmodule

echo "--- 2. Compiling Sysmodule ---"
docker run --rm -v "$(pwd)/sysmodule:/src" omnisave-builder make
