#!/bin/sh
# One-time setup on your Mac: installs XcodeGen, generates Rounds.xcodeproj
# and opens it. Re-run after pulling changes that add or move files.
set -e
cd "$(dirname "$0")/.."
if ! command -v xcodegen >/dev/null 2>&1; then
  if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is needed to install XcodeGen: https://brew.sh" >&2
    exit 1
  fi
  brew install xcodegen
fi
xcodegen generate
open Rounds.xcodeproj
