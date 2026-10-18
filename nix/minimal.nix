{ config, pkgs, lib, modulesPath, self, ... }:

# The SnapOS minimal USB: like the NixOS minimal ISO, a terminal and no
# installer with questions. You install SnapOS by hand, following the
# handbook (https://snapos-distro.github.io): partition, format, mount,
# `snapos init`, edit local.nix, `snapos install`.
let
  init = pkgs.writeShellScriptBin "snapos-init" (builtins.readFile ./minimal/snapos-init.sh);
  install = pkgs.writeShellScriptBin "snapos-install" (builtins.readFile ./minimal/snapos-install.sh);
in
{
  imports = [
    (modulesPath + "/installer/cd-dvd/installation-cd-minimal.nix")
    ./modules/snapos.nix
  ];

  services.xserver.enable = lib.mkForce false;
  services.desktopManager.budgie.enable = lib.mkForce false;
  services.xserver.displayManager.lightdm.enable = lib.mkForce false;
  networking.networkmanager.enable = true;
  i18n.supportedLocales = [ "all" ];

  environment.systemPackages = [ init install pkgs.parted pkgs.git pkgs.pciutils ];
  environment.etc."snapos-build".text = self.shortRev or self.dirtyShortRev or "unknown";
  environment.etc."snapos-src".source = ../.;

  # What you see after it starts: where you are and the first step.
  services.getty.helpLine = lib.mkForce ''

    SnapOS minimal — a terminal, and SnapOS installed by hand.
    The handbook walks you through it:  https://snapos-distro.github.io
      1. Connect:     nmtui
      2. Partition:   lsblk, then parted (handbook, step 2)
      3. Format:      mkfs.fat / mkfs.ext4 (step 3)
      4. Mount:       mount ... /mnt (step 4)
      5. Prepare:     snapos init
      6. Choose:      nano /mnt/etc/snapos/local.nix
      7. Install:     snapos install
  '';

  boot.kernelParams = [ "console=tty0" ]
    ++ lib.optionals pkgs.stdenv.hostPlatform.isx86_64 [ "console=ttyS0,115200" ]
    ++ lib.optionals pkgs.stdenv.hostPlatform.isAarch64 [ "console=ttyAMA0,115200" ];

  isoImage.isoName = lib.mkForce (if pkgs.stdenv.hostPlatform.isAarch64 then "snapos-minimal-aarch64.iso" else "snapos-minimal.iso");
  isoImage.volumeID = lib.mkForce "SNAPOS_MINIMAL";
  isoImage.appendToMenuLabel = " SnapOS minimal";
  isoImage.splashImage = ../branding/splash/boot-800x600.png;
  isoImage.efiSplashImage = ../branding/splash/efi-1920x1080.png;
  isoImage.grubTheme = null;

  system.stateVersion = "26.05";
}
