# ESP32-S3-Zero USB Wi-Fi adapter for Android

This firmware turns a Waveshare ESP32-S3-Zero into a **USB Ethernet adapter** that gets its internet from Wi-Fi. Use it to get an Android phone or tablet online when the device has no working Wi-Fi of its own.

```
Android ──USB (CDC-NCM)── ESP32-S3 192.168.7.1 ──NAT── Wi-Fi ── Internet
         gets 192.168.7.x by DHCP
```

## Using it
1. Plug the board into the Android device's USB-C port. Use an OTG adapter if the device needs one.
2. **First time:** Android shows **"Sign in to network"**. Tap it and the setup page opens. If nothing appears, open `http://192.168.7.1` in a browser.
3. Pick your Wi-Fi network, enter the password and tap **Connect**.
4. After a few seconds the adapter reconnects to the phone, and Android has internet.

The board saves the Wi-Fi details. On the next plug-in it connects within about 15 s before the phone even sees the adapter. If that Wi-Fi network isn't available, the setup page appears again. The setup and status page is always at `http://192.168.7.1`. Use **Forget this Wi-Fi** there to switch networks.

| LED | Meaning |
|---|---|
| blue | Setup page active, waiting for Wi-Fi details |
| yellow | Connecting to Wi-Fi |
| green | Online |
| red | The last connection attempt failed (wrong password, network not found) |

## Build and flash
Requires PlatformIO (`pipx install platformio`).

```sh
pio run                      # build
pio run -t upload            # flash (see the download-mode note below)
```

**Download mode:** the S3-Zero's USB-C port is the chip's native USB, and this firmware takes it over for networking. As a result the board does **not** appear as a serial port while the firmware runs. To flash it, hold **BOOT**, plug in the board (or tap **RESET** while holding BOOT), release BOOT, then run `pio run -t upload`. Press RESET afterwards.

**Logs:** the firmware logs to UART0 at 115200 baud, TX on **GPIO43**. Connect a USB-TTL adapter (GND plus TX→RX) and run `pio device monitor -p <port>`.

## If Android doesn't detect the adapter
- You need Android 9 or newer. Most Android 11+ devices include the `cdc_ncm` driver.
- Some vendors only ship the CDC-ECM driver. To try it, edit `sdkconfig.defaults`: replace `CONFIG_TINYUSB_NET_MODE_NCM=y` with `CONFIG_TINYUSB_NET_MODE_ECM_RNDIS=y`. Then delete `sdkconfig.esp32s3zero` and rebuild.
- If you have a Mac or Linux PC, check the adapter there first. It should appear as a new network interface and get a 192.168.7.x address.

## Limitations
- 2.4 GHz Wi-Fi only (an ESP32-S3 limitation). Throughput is a few Mbit/s, because USB full-speed and the ESP32 doing NAT both limit it.
- IPv4 only.
- If your Wi-Fi network itself uses 192.168.7.0/24, the addresses clash. Change `USB_NET_IP*` in `src/app_config.h`.
- Wi-Fi networks that need their own web login (hotels and similar) are not supported.
- Enterprise (802.1X) Wi-Fi is not supported.

## Code layout
| File | Purpose |
|---|---|
| `src/main.c` | Startup order: Wi-Fi first, then USB once the device is online or the setup page is needed |
| `src/usb_ncm_netif.c` | Connects TinyUSB NCM to esp_netif; runs the DHCP server and NAT on the USB side; re-enumerates USB |
| `src/wifi_sta.c` | Wi-Fi state machine (one task), saved credentials in NVS, the setup-page on/off logic |
| `src/dns_server.c` | DNS server: in setup mode it answers every name with 192.168.7.1; otherwise it forwards queries to the Wi-Fi network's DNS |
| `src/web_server.c` | Setup page, JSON API (`/api/status`, `/api/scan`, `/api/connect`, `/api/forget`), redirects that trigger Android's "Sign in to network" |
| `web/index.html` | Setup and status page (embedded into the firmware) |

## How Android finds the setup page
Android checks each new network by requesting `http://connectivitycheck.gstatic.com/generate_204`. In setup mode the ESP32's DNS server points that name at itself, and the web server answers with a `302` redirect to `http://192.168.7.1/`. Android treats that as a captive portal and offers "Sign in to network".

When the ESP32 gets online, the DNS server switches to forwarding, so Android's check reaches Google and gets a real `204`. The ESP32 also briefly drops off USB and reconnects. That makes Android check the network again straight away, instead of waiting for its next scheduled check.
