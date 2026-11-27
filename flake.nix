{
  description = "Savage Wind runtime, without game files";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      each = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
      versionOf = pkgs:
        let
          lines = pkgs.lib.splitString "\n" (builtins.readFile ./runtime/ui/launcher.mm);
          hit = pkgs.lib.findFirst (l: pkgs.lib.hasPrefix "#define SAVAGEWIND_VERSION " (pkgs.lib.trim l)) "" lines;
          matched = builtins.match "#define SAVAGEWIND_VERSION \"([^\"]+)\".*" hit;
        in
        if matched == null then "0" else builtins.head matched;
    in
    {
      packages = each (pkgs:
        let
          inherit (pkgs) lib;
        in
        {
          default = pkgs.stdenv.mkDerivation {
            pname = "savagewind";
            version = versionOf pkgs;
            src = lib.cleanSourceWith {
              src = ./.;
              filter = path: type:
                let base = baseNameOf (toString path);
                in
                !(builtins.elem base [
                  "extracted"
                  "generated"
                  "generated-aarch64"
                  "build"
                  "save"
                  "dump"
                  "keys"
                  "achievements"
                  "amiibo"
                  "amiibo_data"
                  "venv"
                  "__pycache__"
                ])
                && !(base == "luts" && dirOf (toString path) == toString ./.)
                && !(lib.hasSuffix ".keys" base)
                && !(lib.hasSuffix ".nsp" base)
                && !(lib.hasSuffix ".xci" base)
                && lib.cleanSourceFilter path type;
            };
            nativeBuildInputs = with pkgs; [ clang lld pkg-config addDriverRunpath imagemagick ];
            buildInputs = with pkgs; [ sdl3 ffmpeg shaderc vulkan-loader vulkan-headers zlib ];
            dontConfigure = true;
            buildPhase = ''
              runHook preBuild
              make -j''${NIX_BUILD_CORES:-1}
              runHook postBuild
            '';
            installPhase = ''
              runHook preInstall
              mkdir -p "$out/bin" "$out/bin/colour-luts" "$out/share/applications"
              cp build/savagewind "$out/bin/savagewind"
              # Next to the executable, where the runtime looks (paths.c rt_resource): the same set the AppImage ships.
              cp docs/icon/icon_1024.png "$out/bin/icon.png"
              # Plasma's hicolor index has no 1024x1024/apps, so a single icon of that size shows up blank.
              for size in 48 64 128 256 512; do
                mkdir -p "$out/share/icons/hicolor/''${size}x''${size}/apps"
                magick docs/icon/icon_1024.png -resize ''${size}x''${size} \
                  "$out/share/icons/hicolor/''${size}x''${size}/apps/savagewind.png"
              done
              cp docs/media/wordmark.png docs/media/wordmark_config.png docs/media/wordmark_boot.png \
                docs/media/default_splash.jpg "$out/bin/"
              cp third_party/fonts/FiraSans/FiraSans-SemiBold.ttf \
                third_party/fonts/FiraSans/FiraSans-Medium.ttf "$out/bin/"
              cp third_party/fonts/FiraSans/OFL.txt "$out/bin/FiraSans-OFL.txt"
              cp third_party/fonts/NotoSansThai/NotoSansThai-Medium.ttf "$out/bin/"
              cp third_party/fonts/NotoSansThai/OFL.txt "$out/bin/NotoSansThai-OFL.txt"
              cp third_party/fonts/NotoSansHebrew/NotoSansHebrew-Medium.ttf "$out/bin/"
              cp third_party/fonts/NotoSansHebrew/OFL.txt "$out/bin/NotoSansHebrew-OFL.txt"
              cp third_party/fonts/FontAwesome/fa-solid-900.ttf "$out/bin/"
              cp third_party/fonts/FontAwesome/LICENSE.txt "$out/bin/FontAwesome-LICENSE.txt"
              cp runtime/nvn/vulkan/vk_pipelines.seed "$out/bin/"
              cp third_party/luts/*.cube third_party/luts/README.md "$out/bin/colour-luts/"
              cat > "$out/share/applications/savagewind.desktop" << EOF
              [Desktop Entry]
              Type=Application
              Name=Savage Wind
              Comment=Static recompilation runtime (bring your own game files)
              Exec=$out/bin/savagewind
              Icon=savagewind
              Categories=Game;
              Terminal=false
              Actions=Configuration;

              [Desktop Action Configuration]
              Name=Savage Wind Configuration
              Exec=$out/bin/savagewind --config
              EOF
              sed -i 's/^              //' "$out/share/applications/savagewind.desktop"
              runHook postInstall
            '';
            # -fuse-ld=lld skips Nix's linker wrapper, so the binary is left with no RUNPATH.
            postFixup = ''
              patchelf --set-rpath "${lib.makeLibraryPath (with pkgs; [
                sdl3 ffmpeg shaderc vulkan-loader zlib stdenv.cc.cc.lib
              ])}" "$out/bin/savagewind"
              addDriverRunpath "$out/bin/savagewind"
            '';
            meta = {
              description = "Savage Wind runtime, without game files";
              mainProgram = "savagewind";
              platforms = systems;
            };
          };
        });

      devShells = each (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            clang
            lld
            pkg-config
            gnumake
            sdl3
            ffmpeg
            shaderc
            vulkan-loader
            vulkan-headers
            zlib
            (python3.withPackages (ps:
              [ ps.lz4 ps.capstone ]
              ++ pkgs.lib.optionals (ps ? unicorn) [ ps.unicorn ]))
          ];
        };
      });
    };
}
