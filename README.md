# AquaWM — a tiny tiling Wayland compositor in C++ (wlroots 0.20)

Built and tested on Fedora 44 under WSL2 / WSLg, but it should run on any
Linux with wlroots 0.20: nested under another Wayland/X11 session for
development, or on DRM/KMS on real hardware.

## Status

Working: scene rendering, wallpaper backgrounds, master-stack tiling,
workspaces, floating toggle, Lua config with hot-reload, layer-shell bars
with exclusive zone, nested backends under WSLg, installer for
Arch/Fedora/NixOS.

In progress: XWayland support for legacy X11 apps (Phase 3c). Bar
pointer/keyboard input is not forwarded yet, so bars display and reserve
space but are not clickable.

Supported distros: **Arch Linux, Fedora and NixOS.** The installer and
the dependency lists cover exactly these three; anything else is
unverified. On Arch and Fedora every dependency comes from the official
repositories (no AUR, no COPR); on NixOS the flake provides the whole
toolchain, so no system packages are needed at all.

## Install (recommended)

From a bare machine, one line (Arch, Fedora or NixOS) — detects your
distro, installs git, clones, and hands off to the installer:

```
bash <(curl -s https://raw.githubusercontent.com/Peter5235252/tilewm/main/setup.sh)
```

Or the classic way:

```
git clone https://github.com/Peter5235252/AquaWM.git
cd aquawm
./install.sh
```

On NixOS, everything above works too, with two differences: you need Nix
itself with flakes enabled first (the scripts tell you exactly what to
run if either is missing — on a fresh machine that means installing Nix,
then re-running the one-liner), and nothing touches system packages:
`nix build` produces `./result/bin/aquawm`, while `nix develop` drops
you into a shell with every build dependency. My ThinkPad T480 runs
NixOS, so I test this path on real hardware firsthand — if you try it
elsewhere, reports are welcome. A NixOS module is future work.

The script detects Arch vs Fedora vs NixOS, installs system packages on
Arch/Fedora (sudo is used only for that step — never run the script
itself as root; NixOS needs no system packages since the flake provides
the toolchain), clones or updates the source, builds, runs the test
suite, and installs the example `aquawm.lua`, `foot.ini` and wallpaper
into `~/.config` (existing files are backed up, never silently
overwritten). It uses `gum` menus when available and plain prompts
otherwise. Useful flags: `--yes` (non-interactive), `--no-config`
(leave `~/.config` alone), `--source DIR` (use an existing checkout),
`--prefix DIR` (clone location), `--testmode` (full dry run with HOME
redirected to a temp dir).

Prefer doing it by hand? The exact package sets are listed below, then the
same `cmake` build as everywhere.

## NixOS

A flake provides a pinned dev shell and package (nixpkgs unstable,
wlroots 0.20.x — the same `wlroots-0.20.pc`, so no CMake changes):

```
nix develop   # shell with every build dependency
nix build     # ./result/bin/aquawm (tests run as part of the build)
```

Status: builds green via `nix build` (test suite runs inside the build).
My ThinkPad T480 runs NixOS, so real-hardware verification happens
firsthand. The long-term goal is a proper NixOS module/home-manager
story; that part will take a while.

## Dependencies (Fedora 44)

```
sudo dnf install gcc gcc-c++ cmake ninja-build pkgconf-pkg-config git \
  wlroots-devel wayland-devel wayland-protocols-devel libxkbcommon-devel \
  libinput-devel pixman-devel libseat-devel mesa-libEGL-devel \
  mesa-libGLES-devel libdrm-devel systemd-devel lua-devel \
  libjpeg-turbo-devel libpng-devel \
  foot wayland-utils wlr-randr
```

## Dependencies (Arch Linux)

Same stack, Arch package names (no CMake changes needed: Arch's
`wlroots0.20` ships the same `wlroots-0.20.pc`). Tested target: ThinkPad
T480 and friends with Intel graphics.

```
sudo pacman -S base-devel cmake ninja pkgconf git \
  wlroots0.20 wayland wayland-protocols libxkbcommon libinput libseat \
  mesa libdrm lua libjpeg-turbo libpng \
  foot
```

## Run on real hardware

Log out to a TTY (e.g. `Ctrl+Alt+F3`), log in, and run `./build/aquawm`
from there so backend autocreate picks DRM/KMS (with real GLES2/Vulkan
rendering instead of the nested pixman fallback). A normal TTY login
gives you the logind session compositors need for input and DRM access;
on hybrid-GPU laptops stick to the Intel iGPU. Nested testing under an
existing Wayland/X11 session works exactly like under WSLg.

### Launch from a TTY on NixOS

1. Switch to a free console with `Ctrl+Alt+F3` and log in as yourself
   (not root). If a graphical login manager owns F1/F2, leave it alone —
   another TTY is fine.
2. From your checkout, run `./run-tty.sh`. It unsets `WAYLAND_DISPLAY`
   (so backend autocreate takes the display instead of nesting into
   another session) and starts `./result/bin/aquawm`, falling back to
   `./build/aquawm` for non-Nix builds.
3. Checklist, in the order things usually bite:
   - `foot` installed (`nix profile install nixpkgs#foot`) — without it,
     `Alt+Return` silently does nothing and the desktop looks dead.
   - Active logind session — a normal TTY login provides it; check with
     `loginctl` if input or DRM permission is denied.
   - Intel iGPU primary — if you can see the login prompt, modesetting
     already works.
   - `~/.config/aquawm/aquawm.lua` present — the installer deploys the
     example; without it you get built-in defaults.
4. Quit with `Alt+Shift+E`. If the screen ever locks up, `Ctrl+Alt+F1/F2`
   jumps back to your other session; aquawm releases the display on
   VT switch.

## Login managers

AquaWM shows up as a Desktop Environment wherever sessions are picked —
GDM, SDDM, LightDM (with a Wayland-capable greeter) and greetd+tuigreet
all read the same `/usr/share/wayland-sessions/*.desktop` files
(XDM/LXDM don't do Wayland and are out of scope). Two pieces make it work:

- `sessions/aquawm.desktop` declares the session (`Exec=aquawm-session`).
  The wrapper exports `XDG_CURRENT_DESKTOP=aquawm`, which is what portals
  and apps key off.
- `sessions/aquawm-session` is a tiny wrapper that sets
  `XDG_CURRENT_DESKTOP=aquawm` and execs the binary from `PATH`, so one
  file covers `/usr/local`, distro, and nix-profile installs.

On Arch/Fedora the installer deploys both (binary via `cmake --install`
to `/usr/local`, session file to `/usr/share/wayland-sessions`). On
NixOS the flake package carries the session file — register it with:

```
services.displayManager.sessionPackages = [ aquawm ];
```

and for greetd point tuigreet at the sessions directory, e.g.
`--sessions ${config.services.displayManager.sessionData.desktops}/share/wayland-sessions`.

## Build

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

## Run (nested inside WSLg)

WSLg remotes every Linux window separately (RAIL), so the way to get a
contained desktop feel is one big nested window with all clients inside it:

```
./run-wslg.sh
```

This forces the **X11 backend**, which is currently the durable choice under
WSLg: maximizing a wlroots **Wayland**-backend window makes the host send a
maximized configure the backend cannot satisfy, and the host disconnects us
with an `xdg_wm_base` protocol error. The X11 backend survives maximize and
arbitrary resizes. On real hardware or other sessions, run `./build/aquawm`
directly so backend autocreate can pick Wayland or DRM.

 tips:
- Maximize the aquawm window (`Win+Up`) and open clients *inside* it with
  `Alt+Return`; host-side terminals stay outside and only add clutter.
- Optionally move it to its own Windows virtual desktop (`Win+Tab` -> New
  desktop, drag it over, `Win+Ctrl+Left/Right` to flip).

It prints `WAYLAND_DISPLAY=wayland-1` (or similar). From another terminal:

```
WAYLAND_DISPLAY=wayland-1 foot
```

## Keybindings (Phase 2)

| Keys                | Action                        |
|---------------------|-------------------------------|
| `Alt+Return`        | spawn terminal (`foot`)       |
| `Alt+J` / `Alt+K`   | focus next / previous window  |
| `Alt+Space`         | toggle floating on focused    |
| `Alt+1` … `Alt+4`   | switch workspace              |
| `Alt+Shift+1` … `4` | move focused window + refocus |
| `Alt+Q`             | close focused window          |
| `Alt+Shift+E`       | quit the compositor           |
| click               | focus window                  |
| `Alt+Left-drag`     | move window (floats it first) |
| `Alt+Right-drag`    | resize window (floats it first) |

## Configuration (Phase 3a)

Settings and keybindings live in Lua, not in C++:

```
mkdir -p ~/.config/aquawm
cp examples/aquawm.lua ~/.config/aquawm/aquawm.lua
$EDITOR ~/.config/aquawm/aquawm.lua
```

The file sets `config = { gaps, mfact, nmaster, workspaces }` and
registers keys with `bind("Alt+Shift", "e", "quit")` (modifiers Alt, Ctrl,
Shift, Super; key names are xkb keysyms; workspace actions take a 1-based
number). Apply changes with `Alt+Shift+R`, with `kill -HUP <aquawm-pid>`,
or by restarting. A custom path works too: `aquawm /path/to/aquawm.lua`.
Missing or broken files fall back to built-in defaults with a log line.

## Wallpaper

`config = { wallpaper = "/path/to/image.jpg" }` (PNG or JPEG) sets the
background, cover-fit per output behind all windows; empty means
`~/.config/aquawm/wallpaper.jpg`. Changing it and reloading (`Alt+Shift+R`
or `SIGHUP`) swaps it live. The shipped `assets/wallpaper.jpg` is the
default - copy it next to your `aquawm.lua`.

## Terminal font (foot)

foot warns when it falls back to proportional Noto Sans. Use a real
monospace font:

```
sudo dnf install dejavu-sans-mono-fonts
mkdir -p ~/.config/foot
cp examples/foot.ini ~/.config/foot/foot.ini
```

## Roadmap

- Phase 1 (done): bring-up, scene rendering, floating xdg-shell views, focus.
- Phase 2 (done): master-stack tiling, focus cycling, floating toggle,
  workspaces, clean shutdown handling.
- Phase 3a (done): embedded Lua config (`aquawm.lua`, hot-reload),
  wallpaper backgrounds, foot font fix.
- Phase 3b (done): layer-shell bar support with exclusive zone (e.g.
  run `waybar` inside the session; a top bar reserves its strip and
  tiling fills what remains. Bar input is not forwarded yet).
- Phase 3c (next): XWayland support for legacy X11 apps.
- Installer (done): one-liner `setup.sh` plus `install.sh` for Arch
  and Fedora, with package manifests and a `--testmode` dry run.

## Distro support, now and later

AquaWM supports **Arch Linux and Fedora**, where every dependency comes
from the official repositories (no AUR, no COPR), and **NixOS, which is
supported but very alpha-stage**: it installs and builds through the
flake today, gets tested on real hardware firsthand, and still has rough
edges (see the NixOS notes above). Expect the NixOS path to keep moving
fast and occasionally break while it matures.

**Debian and Ubuntu are not guaranteed at all.** Their slower-moving
release cycles ship wlroots and Wayland libraries far older than a current
compositor needs, and backporting around that is not something this
project will take on. If that ever changes, this section will say so.
