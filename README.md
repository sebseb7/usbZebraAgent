# usbprintagent (C)

SSE based network pritner agent for Zebra USB printers.

Built on libuv, libcurl, and libusb.

On Ubuntu:

```bash
sudo apt install build-essential libuv1-dev libusb-1.0-0-dev libcurl4-openssl-dev
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

