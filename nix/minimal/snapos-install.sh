#!/usr/bin/env bash
# snapos install [ROOT]: builds the system described in ROOT/etc/snapos
# (default /mnt) onto the disk, then asks for the password of each user.
set -euo pipefail
root=${1:-/mnt}
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
dir=$root/etc/snapos
[ -e "$dir/local.nix" ] || { echo "snapos: run snapos init first (handbook, step 5)." >&2; exit 1; }
attr=snapos
[ "$(uname -m)" = aarch64 ] && attr=snapos-aarch64

echo "  Installing SnapOS. This downloads several GB and can take a while."
LC_ALL=C.UTF-8 nixos-install --root "$root" --flake "path:$dir#$attr" --no-root-passwd

users=$(sed -n 's/^ *users\.users\.\([A-Za-z_][A-Za-z0-9_-]*\) *=.*/\1/p' "$dir/local.nix")
for u in $users; do
    if [ -n "${SNAPOS_PASSWORD:-}" ]; then
        printf '%s:%s\n' "$u" "$SNAPOS_PASSWORD" | nixos-enter --root "$root" -c chpasswd
    else
        echo "  Password for $u:"
        nixos-enter --root "$root" -c "passwd $u"
    fi
done
echo
echo "  ✓ SnapOS is installed. Type reboot and remove the USB stick."
