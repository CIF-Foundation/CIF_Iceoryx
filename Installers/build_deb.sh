#!/usr/bin/env bash
# End-to-end build for the cif-iox2 Debian package.
#
# dpkg-deb requires DEBIAN/ mode 0755-0775. WSL mounts of the Windows repo
# report 777 and ignore chmod, so this script copies the template to a native
# Linux temp directory, fixes permissions there, then writes the .deb to
# Installers/output/.
#
# This script does not compile the native library. Build it first:
#   powershell -ExecutionPolicy Bypass -File .\scripts\build-linuxrt.ps1
#
# Usage:
#   bash Installers/build_deb.sh
#   SO_SOURCE=/path/to/libiox2_cif.so bash Installers/build_deb.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEB_TEMPLATE="${SCRIPT_DIR}/cif_iox2_deb"
OUTPUT_DIR="${SCRIPT_DIR}/output"
SO_NAME="libiox2_cif.so"
TEMPLATE_SO="${DEB_TEMPLATE}/usr/lib/cif/${SO_NAME}"
DIST_SO="${REPO_ROOT}/dist/${SO_NAME}"

if [[ ! -d "${DEB_TEMPLATE}/DEBIAN" ]]; then
  echo "Error: Debian template not found at ${DEB_TEMPLATE}" >&2
  exit 1
fi

if ! command -v dpkg-deb >/dev/null 2>&1; then
  echo "Error: dpkg-deb not found. Install dpkg (e.g. apt install dpkg)." >&2
  exit 1
fi

if [[ -n "${SO_SOURCE:-}" ]]; then
  if [[ ! -f "${SO_SOURCE}" ]]; then
    echo "Error: SO_SOURCE is set but file not found: ${SO_SOURCE}" >&2
    exit 1
  fi
elif [[ -f "${TEMPLATE_SO}" ]]; then
  SO_SOURCE="${TEMPLATE_SO}"
elif [[ -f "${DIST_SO}" ]]; then
  SO_SOURCE="${DIST_SO}"
  echo "Using shared library from dist: ${SO_SOURCE}" >&2
else
  echo "Error: Shared library not found at ${TEMPLATE_SO} or ${DIST_SO}" >&2
  echo "Build the Linux library first (scripts/build-linuxrt.ps1), place" >&2
  echo "${SO_NAME} in the Debian template, or set SO_SOURCE." >&2
  exit 1
fi

PKG="$(grep '^Package:' "${DEB_TEMPLATE}/DEBIAN/control" | awk '{print $2}')"
VERSION="$(grep '^Version:' "${DEB_TEMPLATE}/DEBIAN/control" | awk '{print $2}')"
ARCH="$(grep '^Architecture:' "${DEB_TEMPLATE}/DEBIAN/control" | awk '{print $2}')"
if [[ -z "${PKG}" || -z "${VERSION}" || -z "${ARCH}" ]]; then
  echo "Error: Could not read Package, Version, and Architecture from ${DEB_TEMPLATE}/DEBIAN/control" >&2
  exit 1
fi

STAGE_ROOT="$(mktemp -d)"
trap 'rm -rf "${STAGE_ROOT}"' EXIT
STAGING_DIR="${STAGE_ROOT}/cif_iox2_deb"

echo "Staging Debian template on a native Linux filesystem..."
mkdir -p "${STAGING_DIR}"
cp -a "${DEB_TEMPLATE}/." "${STAGING_DIR}/"
mkdir -p "${STAGING_DIR}/usr/lib/cif"
cp -a "${SO_SOURCE}" "${STAGING_DIR}/usr/lib/cif/${SO_NAME}"

# Repository placeholders must not install on the target.
find "${STAGING_DIR}" -name '.gitkeep' -type f -delete

if command -v python3 >/dev/null 2>&1; then
  python3 "${SCRIPT_DIR}/normalize_ipk_line_endings.py" "${STAGING_DIR}"
fi

# dpkg-deb rejects DEBIAN/ at 0777, which is what WSL/drvfs reports.
find "${STAGING_DIR}" -type d -exec chmod 0755 {} +
find "${STAGING_DIR}" -type f -exec chmod 0644 {} +
chmod 0755 "${STAGING_DIR}/DEBIAN"
chmod 0644 "${STAGING_DIR}/DEBIAN/control"
for script in preinst postinst prerm postrm; do
  if [[ -f "${STAGING_DIR}/DEBIAN/${script}" ]]; then
    chmod 0755 "${STAGING_DIR}/DEBIAN/${script}"
  fi
done
chmod 0755 "${STAGING_DIR}/usr/lib/cif/${SO_NAME}"

mkdir -p "${OUTPUT_DIR}"
DEB_FILENAME="${OUTPUT_DIR}/${PKG}_${VERSION}_${ARCH}.deb"

echo "Building Debian package..."
dpkg-deb --build --root-owner-group "${STAGING_DIR}" "${DEB_FILENAME}"

echo "Successfully created ${DEB_FILENAME}"
