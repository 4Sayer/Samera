#!/bin/sh
# Install an extracted binary package without root privileges.
set -eu
package_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix="${SAMERA_INSTALL_PREFIX:-${HOME:?}/.local}"
case "$prefix" in
    /*) ;;
    *) printf '%s\n' 'The install prefix must be an absolute path.' >&2; exit 1 ;;
esac
if [ ! -f "$package_dir/bin/samera" ]; then
    printf '%s\n' 'Run this installer from an extracted binary release.' >&2
    exit 1
fi
install -d "$prefix/bin" "$prefix/share/applications" \
    "$prefix/share/icons/hicolor/scalable/apps" "$prefix/share/doc/samera"
install -m 755 "$package_dir/bin/samera" "$prefix/bin/samera"
install -m 644 "$package_dir/share/icons/hicolor/scalable/apps/samera.svg" \
    "$prefix/share/icons/hicolor/scalable/apps/samera.svg"
# Quote the desktop Exec path, including Desktop Entry reserved characters.
exec_path=$(printf '%s' "$prefix/bin/samera" | sed 's/\\/\\\\\\\\/g; s/"/\\\\"/g; s/`/\\\\`/g; s/\$/\\\\$/g; s/%/%%/g')
while IFS= read -r line; do
    case "$line" in
        Exec=*) printf 'Exec="%s"\n' "$exec_path" ;;
        *) printf '%s\n' "$line" ;;
    esac
done < "$package_dir/share/applications/samera.desktop" > "$prefix/share/applications/samera.desktop"
chmod 644 "$prefix/share/applications/samera.desktop"
for document in README.md LICENSE USAGE.md BUILD.md; do
    install -m 644 "$package_dir/share/doc/samera/$document" "$prefix/share/doc/samera/$document"
done
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$prefix/share/applications" || true
fi
printf '%s\n' "Installed Samera: $prefix/bin/samera" 'Open Samera from your application menu.'
