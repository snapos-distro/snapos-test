{ config, lib, pkgs, ... }:

# Carapace, the SnapOS store. Installing asks for the password through
# polkit (pkexec): every desktop brings an agent for that window.
with lib;
{
  config = mkIf (config.snapos.packs.store.enable && config.services.xserver.enable) {
    environment.systemPackages = [ pkgs.carapace ];
    security.polkit.enable = true;
    # Carapace's scenes live in share/carapace; desktops that do not link the
    # whole share tree (Hyprland) still need it.
    environment.pathsToLink = [ "/share/carapace" ];
  };
}
