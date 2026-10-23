#!/bin/bash
# Fresh scratch save for test runs: tools/debug/fresh_save.sh <dir under build/> (copies save_backup_20261007/save_0)
D=build/$1
rm -rf "$D" && mkdir -p "$D/save_0" && cp save_backup_20261007/save_0/savedata0.bin save_backup_20261007/save_0/screenshot0.bin "$D/save_0/"
