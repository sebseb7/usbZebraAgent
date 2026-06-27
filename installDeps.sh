#!/usr/bin/env bash
# Install build dependencies for usbprintagent.
set -euo pipefail

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

usage() {
    cat <<EOF
Usage: $0

Install compiler and library packages needed to build usbprintagent.

Supported distributions: Alpine, Debian, Ubuntu (and Debian/Ubuntu derivatives).
EOF
}

for arg in "$@"; do
    case "$arg" in
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ ! -r /etc/os-release ]]; then
    echo "Cannot detect OS: /etc/os-release not found" >&2
    exit 1
fi

# shellcheck source=/dev/null
source /etc/os-release

ID="${ID:-}"
ID_LIKE="${ID_LIKE:-}"

is_debian_family() {
    case "$ID" in
        debian|ubuntu|linuxmint|pop|elementary|zorin|kali|raspbian) return 0 ;;
    esac
    [[ " $ID_LIKE " == *" debian "* || " $ID_LIKE " == *" ubuntu "* ]]
}

install_debian() {
  need apt-get
  echo "==> Installing build dependencies (apt)"
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y --no-install-recommends \
    build-essential \
    pkg-config \
    libuv1-dev \
    libusb-1.0-0-dev \
    libcurl4-openssl-dev
}

install_alpine() {
  need apk
  echo "==> Installing build dependencies (apk)"
  apk add --no-cache \
    build-base \
    pkgconf \
    libuv-dev \
    libusb-dev \
    curl-dev
}

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Missing required command: $1" >&2
        exit 1
    }
}

case "$ID" in
    alpine)
        install_alpine
        ;;
    *)
        if is_debian_family; then
            install_debian
        else
            echo "Unsupported distribution: ${PRETTY_NAME:-$ID}" >&2
            echo "Supported: Alpine, Debian, Ubuntu" >&2
            exit 1
        fi
        ;;
esac

echo "==> Done. Run: make"
