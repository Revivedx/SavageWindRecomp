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
                  "luts"
                  "venv"
                  "__pycache__"
                ])
                && !(lib.hasSuffix ".keys" base)
                && !(lib.hasSuffix ".nsp" base)
                && !(lib.hasSuffix ".xci" base)
                && lib.cleanSourceFilter path type;
            };
            nativeBuildInputs = with pkgs; [ clang lld pkg-config addDriverRunpath ];
            buildInputs = with pkgs; [ sdl3 ffmpeg shaderc vulkan-loader vulkan-headers zlib ];
            dontConfigure = true;
            buildPhase = ''
              runHook preBuild
              make -j''${NIX_BUILD_CORES:-1}
              runHook postBuild
            '';
            installPhase = ''
              runHook preInstall
              mkdir -p "$out/bin"
              cp build/savagewind "$out/bin/savagewind"
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
