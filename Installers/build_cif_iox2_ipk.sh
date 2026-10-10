#!/usr/bin/env bash
# End-to-end build for the cif_iox2_ipk LabVIEW RT package.
#
# Workflow:
#   1. Copy the checked-in IPK template into a staging directory
#   2. Copy libiox2_cif.so from SO_SOURCE, the template, or dist/
#   3. Normalize text files to LF line endings for Linux/RT
#   4. Create the .ipk archive with opkg-build and move it to Installers/output/
#
# This script does not compile the native library. Build it first:
#   powershell -ExecutionPolicy Bypass -File .\scripts\build-linuxrt.ps1
#
# Uses Installers/opkg-utils/opkg-build when opkg-build is not installed system-wide.
# Requires binutils (ar), tar, gzip, and python3.
#
# Usage:
#   bash Installers/build_cif_iox2_ipk.sh
#
# Override the shared library source path:
#   SO_SOURCE=/path/to/libiox2_cif.so bash Installers/build_cif_iox2_ipk.sh
#
# On Windows, prefer Installers/build_cif_iox2_ipk.bat which invokes this
# script through WSL.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
IPK_TEMPLATE="${SCRIPT_DIR}/cif_iox2_ipk"
STAGING_DIR="${SCRIPT_DIR}/staging/cif_iox2_ipk"
OUTPUT_DIR="${SCRIPT_DIR}/output"
BUILD_IPK="${SCRIPT_DIR}/build_ipk.sh"
SO_NAME="libiox2_cif.so"
TEMPLATE_SO="${IPK_TEMPLATE}/usr/lib/cif/${SO_NAME}"
STAGING_SO="${STAGING_DIR}/usr/lib/cif/${SO_NAME}"
DIST_SO="${REPO_ROOT}/dist/${SO_NAME}"

if ! command -v python3 >/dev/null 2>&1; then
  echo "Error: python3 is required for line-ending normalization" >&2
  exit 1
fi

if ! command -v ar >/dev/null 2>&1; then
  echo "Error: ar not found. Install binutils (e.g. apt install binutils)." >&2
  exit 1
fi

BUNDLED_OPKG_BUILD="${SCRIPT_DIR}/opkg-utils/opkg-build"
if ! command -v opkg-build >/dev/null 2>&1 && [[ ! -f "${BUNDLED_OPKG_BUILD}" ]]; then
  echo "Error: opkg-build not found and bundled script missing at ${BUNDLED_OPKG_BUILD}" >&2
  exit 1
fi

if [[ ! -d "${IPK_TEMPLATE}" ]]; then
  echo "Error: IPK template not found at ${IPK_TEMPLATE}" >&2
  exit 1
fi

if [[ -n "${SO_SOURCE:-}" ]]; then
  if [[ ! -f "${SO_SOURCE}" ]]; then
    echo "Error: SO_SOURCE is set but file not found: ${SO_SOURCE}" >&2
    exit 1
  fi
elif [[ ! -f "${TEMPLATE_SO}" ]]; then
  if [[ -f "${DIST_SO}" ]]; then
    SO_SOURCE="${DIST_SO}"
    echo "Using shared library from dist: ${SO_SOURCE}" >&2
  else
    echo "Error: Shared library not found at ${TEMPLATE_SO} or ${DIST_SO}" >&2
    echo "Build the RT library first:" >&2
    echo "  powershell -ExecutionPolicy Bypass -File .\\scripts\\build-linuxrt.ps1" >&2
    echo "Or place ${SO_NAME} in the IPK template, or set SO_SOURCE." >&2
    exit 1
  fi
fi

# Stage a clean copy of the template so the source tree is never modified.
echo "Staging IPK template..."
rm -rf "${SCRIPT_DIR}/staging"
mkdir -p "${STAGING_DIR}"
cp -a "${IPK_TEMPLATE}/." "${STAGING_DIR}/"

if [[ -n "${SO_SOURCE:-}" ]]; then
  echo "Copying ${SO_SOURCE}..."
  mkdir -p "$(dirname "${STAGING_SO}")"
  cp -a "${SO_SOURCE}" "${STAGING_SO}"
fi

if [[ ! -f "${STAGING_SO}" ]]; then
  echo "Error: Staged shared library not found at ${STAGING_SO}" >&2
  exit 1
fi

# Ensure shell scripts and control files use Linux line endings.
echo "Normalizing line endings to LF..."
python3 "${SCRIPT_DIR}/normalize_ipk_line_endings.py" "${STAGING_DIR}"

# Build the IPK from the staged tree.
echo "Building IPK..."
OUTPUT_PARENT="$(dirname "${STAGING_DIR}")"
IPK_PATH="$(
  bash "${BUILD_IPK}" "${STAGING_DIR}" "${OUTPUT_PARENT}"
)"

mkdir -p "${OUTPUT_DIR}"
FINAL_IPK="${OUTPUT_DIR}/$(basename "${IPK_PATH}")"
mv -f "${IPK_PATH}" "${FINAL_IPK}"

echo "Successfully created ${FINAL_IPK}"
