#!/usr/bin/env bash
# snapos init [ROOT]: puts a SnapOS system description on a mounted disk
# (default /mnt): the hardware it found, the SnapOS files and local.nix,
# the file with your choices. Then edit local.nix and run `snapos install`.
set -euo pipefail
root=${1:-/mnt}
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
if ! mountpoint -q "$root"; then
    echo "snapos: nothing is mounted at $root. Mount the new system there first (handbook, step 4)." >&2
    exit 1
fi
if [ -e "$root/etc/snapos/configuration.nix" ]; then
    echo "snapos: $root/etc/snapos already exists; edit $root/etc/snapos/local.nix and run: snapos install" >&2
    exit 1
fi

echo "  Detecting the hardware..."
nixos-generate-config --root "$root" >/dev/null
mkdir -p "$root/etc/snapos"
mv "$root/etc/nixos/hardware-configuration.nix" "$root/etc/snapos/"
rm -rf "$root/etc/nixos"

echo "  Copying SnapOS..."
cp -a /etc/snapos-src/. "$root/etc/snapos/"
chmod -R u+w "$root/etc/snapos"
ln -s snapos "$root/etc/nixos"

firmware=bios
[ -d /sys/firmware/efi ] && firmware=uefi
disk=$(lsblk -no PKNAME "$(findmnt -no SOURCE "$root")" 2>/dev/null | head -1)
grub_device=nodev
[ "$firmware" = bios ] && [ -n "$disk" ] && grub_device=/dev/$disk

cat > "$root/etc/snapos/local.nix" <<EOF
# Your choices. Change what you want, save, then run: snapos install
{ ... }: {
  networking.hostName = "snapos";
  time.timeZone = "UTC";                      # e.g. "America/Sao_Paulo"
  i18n.defaultLocale = "en_US.UTF-8";         # e.g. "pt_BR.UTF-8"
  console.keyMap = "us";                      # e.g. "br-abnt2"
  services.xserver.xkb.layout = "us";         # e.g. "br"

  snapos.desktop = "budgie";                  # budgie, plasma, xfce or hyprland
  snapos.appearance = "dark";                 # dark or light

  # your user (the password is set by snapos install)
  users.users.snap = {
    isNormalUser = true;
    extraGroups = [ "wheel" "networkmanager" ];
  };

  # this computer starts with $firmware
  boot.loader.grub.device = "$grub_device";
}
EOF
printf '{ ... }: { }\n' > "$root/etc/snapos/graphics.nix"

echo
echo "  ✓ SnapOS is ready at $root/etc/snapos"
echo "    Now:  nano $root/etc/snapos/local.nix"
echo "    Then: snapos install"
