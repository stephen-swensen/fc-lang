#!/usr/bin/env bash
# Install (or uninstall) the FC VSCode extension.
#
# Modern VSCode tracks installed extensions in ~/.vscode/extensions/extensions.json
# and ignores folders that were merely copied in, so we package a real .vsix and
# install it through the editor CLI (`code --install-extension`). The .vsix is
# built by hand (a plain zip) — no `vsce`/npm needed. If no editor CLI is found
# we fall back to a folder copy and ask the user to restart.
#
# Usage: install.sh [install|uninstall]   (default: install)
set -euo pipefail

ACTION="${1:-install}"
HERE="$(cd "$(dirname "$0")" && pwd)"
EXT_ID="fc-lang.fc-lang"
VERSION="1.0.0"
EXT_DIR="${VSCODE_EXT_DIR:-$HOME/.vscode/extensions}"

if [ "$(id -u)" = "0" ] && [ -z "${FORCE_ROOT_VSCODE:-}" ]; then
    echo "install-vscode: run WITHOUT sudo — it installs into your \$HOME." >&2
    echo "  (re-run as your normal user; set FORCE_ROOT_VSCODE=1 only if you really mean root.)" >&2
    exit 1
fi

# Locate an editor CLI: explicit override, else common VSCode-family names.
find_cli() {
    if [ -n "${FC_CODE_CLI:-}" ]; then echo "$FC_CODE_CLI"; return; fi
    for c in code codium code-insiders cursor windsurf; do
        if command -v "$c" >/dev/null 2>&1; then echo "$c"; return; fi
    done
}
CLI="$(find_cli || true)"

if [ "$ACTION" = "uninstall" ]; then
    if [ -n "$CLI" ]; then "$CLI" --uninstall-extension "$EXT_ID" >/dev/null 2>&1 || true; fi
    rm -rf "$EXT_DIR/$EXT_ID" "$EXT_DIR/$EXT_ID-$VERSION"
    echo "Removed FC extension."
    exit 0
fi

# --- build a .vsix (OPC zip) in a temp dir ---
command -v zip >/dev/null 2>&1 || { echo "install-vscode: 'zip' is required" >&2; exit 1; }
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/extension"
cp -R "$HERE/package.json" "$HERE/extension.js" "$HERE/language-configuration.json" \
      "$HERE/syntaxes" "$HERE/README.md" "$STAGE/extension/"

cat > "$STAGE/extension.vsixmanifest" <<XML
<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">
  <Metadata>
    <Identity Language="en-US" Id="fc-lang" Version="$VERSION" Publisher="fc-lang" />
    <DisplayName>FC Language</DisplayName>
    <Description xml:space="preserve">Syntax highlighting, diagnostics, hover, completion and type CodeLens for the FC language.</Description>
    <Tags>fc</Tags>
    <Categories>Programming Languages</Categories>
    <GalleryFlags>Public</GalleryFlags>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="^1.75.0" />
    </Properties>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code" />
  </Installation>
  <Dependencies/>
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" />
  </Assets>
</PackageManifest>
XML

cat > "$STAGE/[Content_Types].xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="json" ContentType="application/json" />
  <Default Extension="js" ContentType="application/javascript" />
  <Default Extension="md" ContentType="text/markdown" />
  <Default Extension="vsixmanifest" ContentType="text/xml" />
</Types>
XML

VSIX="$STAGE/fc-lang-$VERSION.vsix"
( cd "$STAGE" && zip -r -q "$VSIX" 'extension.vsixmanifest' '[Content_Types].xml' extension )

# Drop any stale unsuffixed manual copy from older installs.
rm -rf "$EXT_DIR/$EXT_ID"

if [ -n "$CLI" ]; then
    # --no-deprecation silences a noisy DEP0169 url.parse() warning emitted by
    # the editor CLI's own internals (not ours); harmless if the CLI ignores it.
    NODE_OPTIONS=--no-deprecation "$CLI" --install-extension "$VSIX" --force
    echo ""
    echo "Installed via '$CLI'. Reload the VSCode window (Developer: Reload Window)."
else
    # No CLI: unpack the vsix into the extensions dir as a fallback.
    DEST="$EXT_DIR/$EXT_ID-$VERSION"
    rm -rf "$DEST"; mkdir -p "$DEST"
    cp -R "$STAGE/extension/." "$DEST/"
    echo "No VSCode CLI found; copied to $DEST."
    echo "Fully quit and reopen VSCode (a window reload may not pick it up)."
fi

echo "Ensure 'fcc' is on PATH (sudo make install) or set fc.serverPath in settings."
