# aquawm on NixOS (experimental branch): reproducible dev shell + package.
#
#   nix develop          # drop into a shell with every build dependency
#   nix build            # build ./result/bin/aquawm (+ run ./result tests)
#
# Design notes:
# - nixpkgs unstable is pinned via flake.lock, so the wlroots 0.20.x and
#   Lua toolchain stay exactly as tested. Refresh with `nix flake update`.
# - Only committed sources enter the build (fileset), never build/ or .git.
# - wlroots_0_20 exposes the same wlroots-0.20.pc pkg-config module as on
#   Fedora/Arch, so CMakeLists.txt needs no Nix-specific changes.

{
  description = "aquawm: minimal tiling Wayland compositor in C++ (wlroots 0.20, Lua config)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forEachSystem = nixpkgs.lib.genAttrs systems;
      pkgsFor = system: import nixpkgs { inherit system; };

      aquawmDeps = pkgs: [
        pkgs.wlroots_0_20
        pkgs.wayland
        pkgs.wayland-protocols
        pkgs.libxkbcommon
        pkgs.libinput
        pkgs.pixman
        pkgs.seatd # provides libseat
        pkgs.mesa
        pkgs.libdrm
        pkgs.lua
        pkgs.libjpeg_turbo
        pkgs.libpng
      ];

      aquawmSrc = {
        root = ./.;
        fileset = nixpkgs.lib.fileset.unions [
          ./CMakeLists.txt
          ./src
          ./tests
          ./examples
          ./assets
          ./sessions
          ./protocol
        ];
      };
    in
    {
      packages = forEachSystem (
        system:
        let
          pkgs = pkgsFor system;
        in
        {
          default = pkgs.stdenv.mkDerivation {
            pname = "aquawm";
            version = "0.1.0";
            src = nixpkgs.lib.fileset.toSource aquawmSrc;

            nativeBuildInputs = with pkgs; [
              cmake
              ninja
              pkg-config
              wayland-scanner
            ];

            buildInputs = aquawmDeps pkgs;

            # The test suite decodes the shipped asset; keep it enabled so
            # `nix build` fails rather than shipping an untested binary.
            doCheck = true;

            # Advertises share/wayland-sessions/aquawm.desktop to
            # services.displayManager.sessionPackages on NixOS.
            passthru.providedSessions = [ "aquawm" ];

            meta = with nixpkgs.lib; {
              description = "Minimal tiling Wayland compositor in C++ (wlroots 0.20, Lua config)";
              platforms = platforms.linux;
            };
          };
        }
      );

      devShells = forEachSystem (
        system:
        let
          pkgs = pkgsFor system;
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.default ];
            packages = with pkgs; [
              foot
              wayland-utils
              waybar # Phase 3b manual test client (top bar + exclusive zone)
              gdb
            ];
            shellHook = ''
              echo "aquawm dev shell: cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build"
            '';
          };
        }
      );
    };
}
