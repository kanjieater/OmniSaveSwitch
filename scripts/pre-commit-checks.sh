#!/bin/sh
# Blocks commits where dist/config.ini has a non-empty server_address.
# Inspects the STAGED version, not the working tree, to prevent bypass via
# "git add sensitive-file; edit file back; git commit".

staged=$(git show :dist/config.ini 2>/dev/null) || exit 0

addr=$(printf '%s' "$staged" | sed -n 's/^server_address=//p')
addr_trimmed=$(printf '%s' "$addr" | tr -d '[:space:]')

if [ -n "$addr_trimmed" ]; then
    echo "ERROR: dist/config.ini has a non-empty server_address in the staged version."
    echo "       Revert it to 'server_address=' before committing."
    exit 1
fi
