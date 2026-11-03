#!/usr/bin/env bash
# Colgate by Hiccup Audio
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Turns a finished build into dist/Colgate-<version>-macOS.pkg.
#
#   bash packaging/macos/build_pkg.sh <build folder> <output folder>
#
# Without an Apple Developer ID the plug-ins get an ad-hoc signature, which is
# what Apple silicon requires to load them at all. The installer itself is then
# unsigned, and macOS asks for one extra confirmation the first time (see the
# README). With the APPLE_* variables set, everything is signed and notarized.
set -euo pipefail

build_dir="${1:?build folder}"
out_dir="${2:?output folder}"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/../.." && pwd)"

version="$(sed -n 's/^project(Colgate VERSION \([0-9.]*\).*/\1/p' "${repo_dir}/CMakeLists.txt")"
if [[ -z "${version}" ]]; then
    echo "Could not read the version from CMakeLists.txt" >&2
    exit 1
fi

artefacts="${build_dir}/source/plugin/Colgate_artefacts/Release"
au_bundle="${artefacts}/AU/Colgate.component"
vst3_bundle="${artefacts}/VST3/Colgate.vst3"
for bundle in "${au_bundle}" "${vst3_bundle}"; do
    if [[ ! -d "${bundle}" ]]; then
        echo "Missing ${bundle}. Build the Colgate_All target first." >&2
        exit 1
    fi
done

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
mkdir -p "${work}/au" "${work}/vst3" "${work}/packages" "${work}/scripts" "${out_dir}"

# Hidden files can't get in a bundle's way, and ditto keeps its structure intact.
ditto "${au_bundle}" "${work}/au/Colgate.component"
ditto "${vst3_bundle}" "${work}/vst3/Colgate.vst3"

# Signing -------------------------------------------------------------------

app_identity="${APPLE_APP_IDENTITY:-}"
installer_identity="${APPLE_INSTALLER_IDENTITY:-}"
keychain=""

if [[ -n "${APPLE_CERTIFICATES_P12:-}" ]]; then
    keychain="${work}/signing.keychain-db"
    security create-keychain -p "colgate" "${keychain}"
    security set-keychain-settings -lut 3600 "${keychain}"
    security unlock-keychain -p "colgate" "${keychain}"
    echo "${APPLE_CERTIFICATES_P12}" | base64 --decode > "${work}/certificates.p12"
    security import "${work}/certificates.p12" -k "${keychain}" -P "${APPLE_CERTIFICATES_PASSWORD:-}" \
        -T /usr/bin/codesign -T /usr/bin/productsign
    security set-key-partition-list -S apple-tool:,apple: -s -k "colgate" "${keychain}" > /dev/null
    security list-keychains -d user -s "${keychain}" login.keychain-db
fi

sign_bundle() {
    if [[ -n "${app_identity}" ]]; then
        codesign --force --deep --options runtime --timestamp --sign "${app_identity}" "$1"
    else
        codesign --force --deep --sign - "$1"
    fi
    codesign --verify --deep "$1"
}

sign_bundle "${work}/au/Colgate.component"
sign_bundle "${work}/vst3/Colgate.vst3"

# Packaging -----------------------------------------------------------------

# A browser upload to GitHub drops the executable bit, so set it on a copy here.
cp "${script_dir}/scripts/postinstall" "${work}/scripts/postinstall"
chmod 755 "${work}/scripts/postinstall"

# By default the installer "relocates" a bundle if the same one exists anywhere
# else on the disk, such as a build folder. Plug-ins must land in the one place
# hosts look.
build_component() {
    local name="$1" root="$2" location="$3" identifier="$4"
    shift 4
    local plist="${work}/${name}.plist"
    pkgbuild --analyze --root "${root}" "${plist}"
    local count
    count="$(/usr/libexec/PlistBuddy -c "Print" "${plist}" | grep -c "RootRelativeBundlePath" || true)"
    local i=0
    while [[ "${i}" -lt "${count}" ]]; do
        /usr/libexec/PlistBuddy -c "Set :${i}:BundleIsRelocatable false" "${plist}"
        i=$((i + 1))
    done
    pkgbuild --root "${root}" --component-plist "${plist}" --identifier "${identifier}" \
        --version "${version}" --install-location "${location}" ${1+"$@"} "${work}/packages/${name}.pkg"
}

build_component "Colgate-AU" "${work}/au" "/Library/Audio/Plug-Ins/Components" \
    "com.hiccupaudio.colgate.au" --scripts "${work}/scripts"
build_component "Colgate-VST3" "${work}/vst3" "/Library/Audio/Plug-Ins/VST3" \
    "com.hiccupaudio.colgate.vst3"

sed "s/__VERSION__/${version}/g" "${script_dir}/distribution.xml" > "${work}/distribution.xml"
mkdir -p "${work}/resources"
cp "${script_dir}/resources/"* "${work}/resources/"
cp "${repo_dir}/LICENSE" "${work}/resources/LICENSE.txt"

output="${out_dir}/Colgate-${version}-macOS.pkg"
productbuild --distribution "${work}/distribution.xml" --resources "${work}/resources" \
    --package-path "${work}/packages" "${work}/unsigned.pkg"

if [[ -n "${installer_identity}" ]]; then
    productsign --sign "${installer_identity}" "${work}/unsigned.pkg" "${output}"
else
    cp "${work}/unsigned.pkg" "${output}"
fi

if [[ -n "${installer_identity}" && -n "${APPLE_NOTARY_ID:-}" ]]; then
    xcrun notarytool submit "${output}" --apple-id "${APPLE_NOTARY_ID}" \
        --password "${APPLE_NOTARY_PASSWORD:-}" --team-id "${APPLE_TEAM_ID:-}" --wait
    xcrun stapler staple "${output}"
fi

if [[ -n "${keychain}" ]]; then
    security delete-keychain "${keychain}" || true
fi

pkgutil --check-signature "${output}" || true
echo "Made ${output}"
