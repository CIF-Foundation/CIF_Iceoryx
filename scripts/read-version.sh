#!/usr/bin/env bash
# Print the repo VERSION with CR/LF/spaces stripped.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
tr -d ' \t\r\n' < "${ROOT}/VERSION"
