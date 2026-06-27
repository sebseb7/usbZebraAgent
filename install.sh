#!/usr/bin/env bash
# Install usbprintagent binary, udev rules, and systemd unit.
set -euo pipefail

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-/usr/local}"
BINDIR="${PREFIX}/bin"
UNITDIR="/etc/systemd/system"
UDEVDIR="/etc/udev/rules.d"
DEFAULTS="/etc/default/usbprintagent"
SERVICE_USER="${SERVICE_USER:-usbprintagent}"
ENABLE_NOW=0

usage() {
    cat <<EOF
Usage: $0 [--enable]

  --enable    enable and start the systemd service after install

Environment:
  PREFIX            install prefix (default: /usr/local)
  SERVICE_USER      unprivileged user for the daemon (default: usbprintagent)
  AGENT_SERVER_URL  skip prompt and use this URL
EOF
}

for arg in "$@"; do
    case "$arg" in
        --enable) ENABLE_NOW=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 1 ;;
    esac
done

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Missing required command: $1" >&2
        exit 1
    }
}

need install
need make
need pkg-config
need systemctl
need udevadm

for pkg in libuv libcurl libusb-1.0; do
    pkg-config --exists "$pkg" || {
        echo "Missing pkg-config package: $pkg" >&2
        exit 1
    }
done

DEFAULT_AGENT_URL="http://127.0.0.1:3847"
if [[ -f "$DEFAULTS" ]]; then
    existing="$(grep -E '^AGENT_SERVER_URL=' "$DEFAULTS" 2>/dev/null | tail -1 | cut -d= -f2- || true)"
    [[ -n "$existing" ]] && DEFAULT_AGENT_URL="$existing"
fi

if [[ -n "${AGENT_SERVER_URL:-}" ]]; then
    AGENT_URL="$AGENT_SERVER_URL"
elif [[ -t 0 ]]; then
    read -r -p "Agent server URL [${DEFAULT_AGENT_URL}]: " AGENT_URL
    AGENT_URL="${AGENT_URL:-$DEFAULT_AGENT_URL}"
else
    AGENT_URL="$DEFAULT_AGENT_URL"
fi

while [[ "$AGENT_URL" == */ ]]; do
    AGENT_URL="${AGENT_URL%/}"
done

if [[ ! "$AGENT_URL" =~ ^https?:// ]]; then
    echo "Agent server URL must start with http:// or https://" >&2
    exit 1
fi

echo "==> Building"
make -C "$SCRIPT_DIR"

echo "==> Ensuring group plugdev"
getent group plugdev >/dev/null || groupadd --system plugdev

echo "==> Ensuring user ${SERVICE_USER}"
if ! id "$SERVICE_USER" >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /usr/sbin/nologin \
        -G plugdev "$SERVICE_USER"
else
    usermod -aG plugdev "$SERVICE_USER" 2>/dev/null || true
fi

echo "==> Installing binary to ${BINDIR}"
install -d "$BINDIR"
install -m 755 "${SCRIPT_DIR}/usbprintagent" "${BINDIR}/usbprintagent"

echo "==> Installing config (${DEFAULTS})"
install -d "$(dirname "$DEFAULTS")"
cat >"$DEFAULTS" <<EOF
# usbprintagent configuration (sourced by systemd)
AGENT_SERVER_URL=${AGENT_URL}
EOF
chmod 644 "$DEFAULTS"

echo "==> Installing udev rules"
install -m 644 "${SCRIPT_DIR}/99-usbprintagent.rules" \
    "${UDEVDIR}/99-usbprintagent.rules"
udevadm control --reload-rules
udevadm trigger

echo "==> Installing systemd unit"
sed "s|/usr/local|${PREFIX}|g; s|^User=.*|User=${SERVICE_USER}|" \
    "${SCRIPT_DIR}/usbprintagent.service" \
    > "${UNITDIR}/usbprintagent.service"
systemctl daemon-reload

if [[ "$ENABLE_NOW" -eq 1 ]]; then
    echo "==> Enabling and starting usbprintagent.service"
    systemctl enable --now usbprintagent.service
    systemctl --no-pager status usbprintagent.service
else
    cat <<EOF

Installed (AGENT_SERVER_URL=${AGENT_URL}).
  Plug the printer in, then:
    systemctl enable --now usbprintagent
  Logs:
    journalctl -u usbprintagent -f
EOF
fi
