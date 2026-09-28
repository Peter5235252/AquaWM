#!/usr/bin/env bash
# aquawm one-line setup (ML4W style): detect distro, install git, clone
# aquawm, hand off to install.sh. Run from anywhere, even a bare machine:
#
#   bash <(curl -s https://raw.githubusercontent.com/Peter5235252/AquaWM/main/setup.sh)
#
# Any arguments are forwarded to install.sh (try --yes for automation).
# Honors AQUAWM_DEST to override the checkout location (default ~/aquawm).

set -euo pipefail

REPO_URL="https://github.com/Peter5235252/AquaWM.git"
# AQUAWM_DEST overrides; TILEWM_DEST still honored once with a warning.
if [ -n "${TILEWM_DEST:-}" ] && [ -z "${AQUAWM_DEST:-}" ]; then
    echo "setup: note: TILEWM_DEST is deprecated, use AQUAWM_DEST." >&2
fi
DEST="${AQUAWM_DEST:-${TILEWM_DEST:-$HOME/aquawm}}"

die() { printf 'setup: error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -ne 0 ] || die "do not run as root."

DISTRO=""
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    case "${ID:-} ${ID_LIKE:-}" in
        *arch*) DISTRO="arch" ;;
        *fedora*) DISTRO="fedora" ;;
        *nixos*) DISTRO="nixos" ;;
    esac
fi
[ -n "$DISTRO" ] || die "unsupported distro: aquawm supports Arch Linux, Fedora and NixOS only, for now."

# sudo is only needed for system packages (Arch/Fedora). NixOS installs
# everything user-local via profiles, so skip it there entirely.
if [ "$DISTRO" != "nixos" ]; then
    if ! sudo -n true 2>/dev/null; then
        if [ ! -t 0 ]; then
            die "sudo needs a password but there is no terminal. Authenticate (sudo -v), then re-run."
        fi
        sudo -v || die "sudo authentication failed."
    fi
fi

if [ "$DISTRO" = "nixos" ]; then
    # On NixOS there are no system packages to install: the flake provides
    # the toolchain. We only need nix itself (with flakes) and a git.
    command -v nix >/dev/null 2>&1 || die "Nix is not installed. Install it first: bash <(curl -s https://install.determinate.systems/nix) -- then re-run this script."
    if ! command -v git >/dev/null 2>&1; then
        echo "setup: installing git into your nix profile ..."
        nix profile install nixpkgs#git
    fi
else
    if ! command -v git >/dev/null 2>&1; then
        echo "setup: installing git ..."
        if [ "$DISTRO" = "arch" ]; then
            sudo pacman -S --needed --noconfirm git
        else
            sudo dnf install -y git
        fi
    fi
fi

if [ -d "$DEST/.git" ]; then
    echo "setup: using existing checkout at $DEST"
else
    # Shallow, bounded, retried: an installer needs files, not history,
    # and a smaller transfer window dodges transient stalls - including
    # the classic hang after "Resolving deltas: 100%". Full history later:
    #   git -C "$DEST" fetch --unshallow
    attempt=1
    while [ "$attempt" -le 2 ]; do
        if timeout 300 git clone --depth 1 "$REPO_URL" "$DEST"; then
            break
        fi
        echo "setup: clone attempt $attempt failed or timed out; retrying ..."
        rm -rf "$DEST"
        attempt=$((attempt + 1))
        sleep 3
    done
    [ -d "$DEST/.git" ] || die "could not clone $REPO_URL. Check network/proxy (env | grep -i proxy), antivirus scanning the target dir, free disk space, and git version (git --version). Then re-run."
fi

[ -f "$DEST/install.sh" ] || die "checkout at $DEST looks incomplete (install.sh missing); remove $DEST and re-run."
exec "$DEST/install.sh" "$@"
