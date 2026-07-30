# 📡 Local Flight Radar

Local Flight Radar is a fork of [Anthony Sturdy's Micro Radar project](https://github.com/AnthonySturdy/micro-radar?tab=readme-ov-file) and the TechTalkies Flight Radar fork. It keeps the hardware small and inexpensive while replacing the cloud dependency with a local ADS-B receiver on your own network.

This project is meant to be a glanceable radar display for a 240x240 round screen. It tells you what is around you right now, without ads, subscriptions, or a cloud account.

---
![alt](Images/sample.gif)

## This fork adds

- Local Flight Radar branding and setup flow
- Local ADS-B receiver support over HTTP
- ESP32-S3 support
- Round GC9A01 display support
- Rotary encoder navigation
- Aircraft selection and details screen
- Aircraft detail field selection
- Aviation, imperial, and metric unit systems
- Screen density presets with live aircraft counts
- Custom distance selection in whole numbers
- Sleep schedule with wake-on-knob support
- Device status and manual time sync
- Wi-Fi reset and factory reset recovery
- Onboard RGB status LED
- Simplified hardware requirements

---
## Hardware

- ESP32-S3 board used for this build
- 240x240 round GC9A01 display
- Rotary encoder
- 3D printed parts

---
## Wiring

### GC9A01 240x240 round display

| ESP32-S3 | Display |
| --- | --- |
| GP2 | SCL |
| GP3 | SDA |
| GP4 | DC |
| GP5 | CS |
| GP6 | RST |
| 5V | VCC |
| GND | GND |

### KY-040 rotary encoder

| ESP32-S3 | KY-040 |
| --- | --- |
| GP9 | CLK |
| GP8 | DT |
| GP7 | SW |
| 3V3 | + |
| GND | GND |

---
## Flashing the firmware

The cleanest way to build and flash this project is with PlatformIO.

1. Clone or download this repository.
2. Install Visual Studio Code.
3. Install the PlatformIO extension.
4. Open the project folder in VS Code.
5. Let PlatformIO download the required dependencies.
6. Build and upload the firmware to your ESP32-S3.

If you are flashing a previously used board, it is often worth erasing the flash first so you know you are testing the current RC image and not old settings.

---
## First-time setup

The setup flow has two parts:

1. Connect the ESP32 to your home Wi-Fi.
2. Open the local setup page and point the display at your ADS-B receiver.

### Step 1: Wi-Fi setup

If the device does not already know your Wi-Fi credentials, it starts a setup access point named:

`LocalFlightRadar-Setup`

Connect to that access point from your phone or laptop and enter your home Wi-Fi SSID and password.

### Step 2: Local configuration page

After the ESP32 joins your Wi-Fi, it shows a QR code on the round display for the local setup page.

Open the setup page using whichever path is easiest on your network:

- Scan the QR code shown on the device
- Open the device IP address in a browser
- Use mDNS if your network supports it

The setup page is served by the ESP32 itself on port `8080`.

### Step 3: Configure the receiver

On the setup page, enter the IP address or hostname of your local flight tracker.

You can then:

- Click **Find receiver** to test common dump1090, SkyAware, readsb, and tar1090-style endpoints
- Leave the receiver address in place and save it manually if you already know it
- Tune distance, units, sleep hours, and detail fields

The device is designed to work with compatible HTTP-accessible `receiver.json` and `aircraft.json` endpoints.

---
## Receiver compatibility

This fork has been tested with **dump1090-fa 10.2**.

It should also work with other dump1090-compatible receivers, provided they expose compatible `receiver.json` and `aircraft.json` files over HTTP. That includes readsb-style and tar1090-style setups when those files are available on the network.

If you know the receiver IP or hostname, entering it directly is the simplest path.

---
## Using the device

Once configured, the device behaves like a small local radar display:

- Rotate the encoder to move between aircraft
- Press the encoder to show aircraft details
- Use the configuration page to change units, distance, and other settings later
- Use the status card to see Wi-Fi, receiver, time, and sleep information
- Click **Sync time now** if you want to force an NTP refresh

### Distance presets

The setup page includes density presets with live aircraft counts so you can pick the display density that fits your receiver.

If none of the presets fit, use the advanced whole-number distance field.

### Units

Choose one of the following unit systems:

- **Aviation**: feet, knots, nautical miles
- **Imperial**: feet, mph, statute miles
- **Metric**: meters, km/h, kilometers

### Sleep schedule

You can schedule the screen and onboard LED to sleep during quiet hours.

The display can also wake briefly when you press the encoder knob.

---
## Recovery

The project has two different reset paths:

- **Hold the encoder button for 10 seconds during boot** to clear Wi-Fi only
- **Use Factory reset in the settings page** to clear all saved configuration and Wi-Fi credentials

That split is intentional. Wi-Fi recovery should be easy, while full factory reset should remain a deliberate action from the settings page.

---
## Troubleshooting

- If the QR code is not showing, wait until the device finishes joining Wi-Fi
- If no aircraft are visible, check the receiver address and confirm the receiver is exposing compatible JSON files
- If the display stays awake overnight, confirm the sleep schedule is enabled and the device time is correct
- If the time looks wrong, use **Sync time now**
- If the receiver cannot be discovered automatically, enter the receiver IP manually

---
## Credits

This fork builds on the original work from Anthony Sturdy and the TechTalkies Flight Radar fork.

Many thanks to both projects for making this build possible.

---
## License

See the repository license for details.
