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
          # The same Zig 0.17.0 the AppImage ships (tools/fetch_zig.sh). The binary is static, so it runs on NixOS.
          zigSrc = pkgs.fetchurl {
            url = "https://ziglang.org/download/0.17.0/zig-${pkgs.stdenv.hostPlatform.system}-0.17.0.tar.xz";
            hash = {
              x86_64-linux = "sha256-HL6d+fJ+a3jRTMvKQ7ZwOkBO957xxGPekB1/CI1OICY=";
              aarch64-linux = "sha256-no0RZh1K471XcCo4MngeI60VHd5XmOFqXM1QP2UjT/g=";
            }.${pkgs.stdenv.hostPlatform.system};
          };
          # tools/gen.py, with the same libraries the dev shell uses. Shipped as bin/swgen/swgen.
          swgenPy = pkgs.python3.withPackages (ps: [ ps.lz4 ps.capstone ps.unicorn ]);
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
            nativeBuildInputs = with pkgs; [ clang lld pkg-config imagemagick makeWrapper ];
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
              # What Set up game files builds the game with (runtime/gamebuild.c), next to the executable,
              # the same places the AppImage uses.
              mkdir -p "$out/bin/gamelib-src" "$out/bin/swgen"
              cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/simd_softfma.h runtime/gamelib.h \
                tools/gamelib_entry.c runtime/hle/hle_symbols.txt "$out/bin/gamelib-src/"
              cp tools/analyze.py tools/flags.py tools/game_sites.py tools/gen.py tools/layout.py \
                tools/lifter.py tools/nso.py tools/patches.py tools/regs.py tools/simd_c.py \
                "$out/bin/swgen/"
              cat > "$out/bin/swgen/swgen" << EOF
              #!${swgenPy}/bin/python3
              import os, sys
              here = os.path.dirname(os.path.realpath(__file__))
              sys.path.insert(0, here)
              os.execv(sys.executable, [sys.executable, os.path.join(here, "gen.py"), *sys.argv[1:]])
              EOF
              sed -i 's/^              //' "$out/bin/swgen/swgen"
              chmod +x "$out/bin/swgen/swgen"
              tar -xJf ${zigSrc} -C "$out/bin"
              mv "$out/bin"/zig-* "$out/bin/zig"
              L="$out/bin/zig/lib"
              for d in "$L"/libc/include/*; do
                echo "$(basename "$d")" | grep -qE 'linux|glibc' || rm -rf "$d"
              done
              for d in "$L"/libc/*; do
                n=$(basename "$d")
                [ "$n" = include ] || echo "$n" | grep -qE '^(glibc)$' || rm -rf "$d"
              done
              rm -rf "$L/libcxx" "$L/libcxxabi" "$L/libtsan" "$L/docs" "$out/bin/zig/doc"
              cat > "$out/share/applications/savagewind.desktop" << EOF
              [Desktop Entry]
              Type=Application
              Name=Savage Wind
              Comment=Static recompilation runtime (bring your own game files)
              Exec=savagewind
              Icon=savagewind
              Categories=Game;
              Terminal=false
              Actions=Configuration;

              [Desktop Action Configuration]
              Name=Savage Wind Configuration
              Exec=savagewind --config
              EOF
              sed -i 's/^              //' "$out/share/applications/savagewind.desktop"
              # Plasma resolves the profile symlink into the store, outside its applications
              # directories. A desktop file there is launched only if it is executable.
              chmod +x "$out/share/applications/savagewind.desktop"
              runHook postInstall
            '';
            # -fuse-ld=lld skips Nix's linker wrapper, so the binary is left with no RUNPATH.
            # The DLSS feature library is mapped from a memfd and then dlopens libnvidia-ml.so.1 by soname.
            # That lookup uses LD_LIBRARY_PATH, not this binary's RUNPATH, so the driver directory has to be there.
            postFixup = ''
              patchelf --set-rpath "${lib.makeLibraryPath (with pkgs; [
                sdl3 ffmpeg shaderc vulkan-loader zlib stdenv.cc.cc.lib
              ])}" "$out/bin/savagewind"
              wrapProgram "$out/bin/savagewind" \
                --prefix LD_LIBRARY_PATH : /run/opengl-driver/lib \
                --prefix PATH : ${lib.makeBinPath [ pkgs.curl ]}
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
          # ./build/savagewind is a raw link: the Makefile stays free of Nix paths. This shell is what
          # finds SDL and libstdc++ when that binary is launched from here, the same libraries the package gets.
          shellHook = ''
            export LD_LIBRARY_PATH="${pkgs.lib.makeLibraryPath (with pkgs; [
              sdl3 ffmpeg shaderc vulkan-loader zlib stdenv.cc.cc.lib
            ])}:/run/opengl-driver/lib''${LD_LIBRARY_PATH:+:}$LD_LIBRARY_PATH"
          '';
        };
      });
    };
}
