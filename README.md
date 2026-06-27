[![CI](https://github.com/sebseb7/usbZebraAgent/actions/workflows/ci.yml/badge.svg)](https://github.com/sebseb7/usbZebraAgent/actions/workflows/ci.yml)

SSE based network printer agent for Zebra USB printers.

Built on libuv, libcurl, and libusb.

```bash
sudo ./installDeps.sh
make
```

```bash
sudo ./install.sh --enable
```

```bash
sudo AGENT_SERVER_URL=http://host:3847 ./install.sh --enable
```

```bash
AGENT_SERVER_URL=http://127.0.0.1:3847 ./usbprintagent
```
