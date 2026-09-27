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
        pkgs.libxcb
        pkgs.libxcb-wm # xcb-ewmh + xcb-icccm for wlr/xwayland.h
        pkgs.freetype # warning-bar text rasterization
        pkgs.fontconfig # warning-bar font lookup
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
              makeWrapper
            ];

            buildInputs = aquawmDeps pkgs;

            # The test suite decodes the shipped asset; keep it enabled so
            # `nix build` fails rather than shipping an untested binary.
            doCheck = true;

            # Login managers run aquawm-session with a bare environment, so
            # the wrapper must find its own binary without relying on PATH.
            # Display managers also resolve TryExec at login time and reject
            # the session when it does not resolve (seen as an empty session
            # / freeze on Plasma Login Manager), so pin absolute store paths
            # in the installed desktop file. FHS installs keep the bare
            # names via CMake.
            postInstall = ''
              wrapProgram $out/bin/aquawm-session \
                --prefix PATH : $out/bin
              # NOTE: TryExec first — plain 'Exec=' is a substring of it.
              substituteInPlace $out/share/wayland-sessions/aquawm.desktop \
                --replace-fail 'TryExec=aquawm-session' "TryExec=$out/bin/aquawm-session" \
                --replace-fail 'Exec=aquawm-session' "Exec=$out/bin/aquawm-session"
            '';

            # Advertises share/wayland-sessions/aquawm.desktop to
            # services.displayManager.sessionPackages on NixOS.
            passthru.providedSessions = [ "aquawm" ];

            meta = with nixpkgs.lib; {
              description = "Minimal tiling Wayland compositor in C++ (wlroots 0.20, Lua config)";
              license = licenses.mit;
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
              kitty
              wayland-utils
              waybar # Phase 3b manual test client (top bar + exclusive zone)
              xwayland # Phase 3c: X server binary wlroots spawns on demand
              xterm # Phase 3c manual test client
              xeyes # Phase 3c manual test client
              gdb
            ];
            shellHook = ''
              echo "aquawm dev shell: cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build"
            '';
          };
        }
      );

      # NixOS login-manager integration: registers the wayland-session file
      # with services.displayManager.sessionPackages so AquaWM appears in
      # SDDM, Plasma Login Manager, GDM, ly and tuigreet pickers.
      # Usage in your system flake:
      #   imports = [ aquawm.nixosModules.aquawm ];
      #   programs.aquawm.enable = true;
      # Optionally preselect it: services.displayManager.defaultSession = "aquawm";
      nixosModules.aquawm =
        { config, lib, pkgs, ... }:
        let
          cfg = config.programs.aquawm;
        in
        {
          options.programs.aquawm = {
            enable = lib.mkEnableOption "AquaWM tiling Wayland compositor (login-manager session)";

            package = lib.mkOption {
              type = lib.types.package;
              default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
              description = "AquaWM package providing bin/aquawm and the wayland-session file.";
            };
          };

          config = lib.mkIf cfg.enable {
            services.displayManager.sessionPackages = [ cfg.package ];
          };
        };

      nixosModules.default = self.nixosModules.aquawm;
    };
}
