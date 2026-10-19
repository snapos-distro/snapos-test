{ stdenv, pkg-config, wrapGAppsHook3, gtk3, src }:

# Carapace, the SnapOS store: the programs from Nix, installed with
# `snapos add` through pkexec, and Snappy's scenes while it works.
stdenv.mkDerivation {
  pname = "carapace";
  version = "0.1";
  inherit src;

  nativeBuildInputs = [ pkg-config wrapGAppsHook3 ];
  buildInputs = [ gtk3 ];

  buildPhase = ''
    runHook preBuild
    make bin/carapace CARAPACE_DATA=$out/share/carapace
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 bin/carapace $out/bin/carapace
    install -Dm644 branding/carapace/apps.tsv $out/share/carapace/apps.tsv
    for g in branding/carapace/scenes/*.gif; do install -Dm644 "$g" $out/share/carapace/scenes/$(basename "$g"); done
    install -Dm644 branding/carapace/carapace.desktop $out/share/applications/carapace.desktop
    install -Dm644 branding/carapace/carapace.svg $out/share/icons/hicolor/scalable/apps/carapace.svg
    for s in 16 24 32 48 64 128 256 512; do
      install -Dm644 branding/carapace/icons/carapace-$s.png $out/share/icons/hicolor/''${s}x''${s}/apps/carapace.png
    done
    runHook postInstall
  '';

  meta = {
    description = "Carapace, the SnapOS store";
    mainProgram = "carapace";
    platforms = [ "x86_64-linux" "aarch64-linux" ];
  };
}
