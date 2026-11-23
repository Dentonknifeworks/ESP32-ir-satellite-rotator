# ESP32 Satellite & TV Antenna Rotator Controller (`SatFullContl`)

**Author:** Brad Vaughn (N0SSE)  
**License:** MIT  

An open-source **ESP32-based Wi-Fi Web Server, Infrared (IR) Transmitter/Receiver, 24V AC Relay Monitor, and Hamlib `rotctld` Satellite Tracking Controller** built to automate IR-controlled TV antenna rotators (such as Channel Master models) for amateur radio satellite passes (RS-44, SO-50, ISS, etc.) and terrestrial directional pointing.

---

## Features

- **Hamlib `rotctld` Network Interface (TCP Port `4533`):** Connects directly over Wi-Fi to satellite tracking software such as **SkyRoof** and **Gpredict** without serial cables or USB-to-UART bridges.
- **Automatic AOS Pre-Pointing:** When a tracking client connects while a satellite is still below the horizon (`El < 0.0°`), the controller immediately pre-points the antenna to the Acquisition of Signal (AOS) azimuth so the Yagi is waiting before the pass starts.
- **Smart Deadband & Rate-Limiting:** During active passes (`El >= 0.0°`), incoming azimuth updates are filtered by a configurable `5°` step threshold (`MIN_STEP_DEG`) and `4000 ms` cooldown (`MIN_INTERVAL_MS`) so the IR rotator control box is never flooded with redundant digit sequences.
- **Native 38 kHz Hardware PWM IR Transmitter:** Bypasses ESP32 Arduino Core 3.x timer conflicts in `IRremote` by driving a **TSAL6100** 940 nm IR emitter directly via `ledcAttach(4, 38000, 8)` to transmit clean 32-bit **NEC** frames (`Address 0xAC`).
- **Built-In IR Remote Code Capture (`GPIO 15`):** Uses a **TSOP38238** 38 kHz IR receiver module to decode and print any unknown remote button's protocol, address, and command hex values to the Serial Monitor (`115200` baud).
- **Galvanically Isolated 24V AC Activity Sensing (`GPIO 5`):** Monitors a mechanical 24V AC coil relay across the rotator control box's AC output to display live active/idle power state on the web dashboard.
- **Responsive Mobile & Desktop Web UI (Port `80`):** Displays live `rotctld` connection state, target satellite Azimuth and Elevation, commanded 3-digit rotator heading, 24V AC status badge, direct heading input, cardinal presets, and CW/CCW jog controls.

---

## System Architecture & Signal Flow

```text
<img width="1024" height="576" alt="RotorControl" src="https://github.com/user-attachments/assets/10534e43-0d4b-47bb-a787-50dec882ffa6" />

+-----------------------------------+       Wi-Fi TCP :4533       +-----------------------------------+
|  Satellite Tracking Software      | --------------------------> |  ESP32 Controller                 |
|  (SkyRoof / Gpredict)             |   Hamlib rotctld (P az el)  |  - Hamlib rotctld Server (:4533)  |
+-----------------------------------+                             |  - AOS Pre-Point & 5° Filter      |
                                                                  |  - Web UI & JSON Telemetry (:80)  |
+-----------------------------------+       Wi-Fi HTTP :80        |  - Native 38 kHz PWM (ledc)       |
|  Web Browser UI (PC / Mobile)     | <-------------------------> +-----------------------------------+
|  - Live Az/El & 24V AC Status     |                                |             ^             ^
|  - Manual Heading & CW/CCW Jog    |                                | GPIO 4      | GPIO 15     | GPIO 5
+-----------------------------------+                                v             |             |
                                                            +--------------+  +-----------+  +---------------+
                                                            | TSAL6100 IR  |  | TSOP38238 |  | 24V AC Relay  |
                                                            | + 2N2222 Drv |  | IR Rcvr   |  | Dry NO / COM  |
                                                            +--------------+  +-----------+  +---------------+
                                                                   |                 ^             ^
                                                            38 kHz NEC (0xAC)   Remote IR     24V AC Output
                                                                   v                 |             |
                                                            +--------------------------------------------+
                                                            |  Channel Master TV Antenna Rotator Box     |
                                                            |  -> 3-Wire / 4-Wire Rotor Drive (0°-360°)  |
                                                            +--------------------------------------------+
```

---

## Hardware Bill of Materials (BOM)

| Component | Specification / Model | Purpose |
| :--- | :--- | :--- |
| **Microcontroller** | ESP32 Dev Module (38-pin DevKitC / ESP32S) | Wi-Fi web server, `rotctld` server, and 38 kHz PWM generation |
| **IR Emitter LED** | Vishay TSAL6100 (940 nm, high-power) | Transmits 38 kHz NEC IR commands to the Channel Master control box |
| **IR Driver Transistor** | 2N2222 NPN (or BS170 N-MOSFET) + 1 kΩ base resistor | Switches high current (~50–80 mA) through the TSAL6100 from 3.3V GPIO 4 |
| **LED Current Resistor** | 33 Ω – 47 Ω (1/2W) | Limits peak pulsed current to the TSAL6100 from 5V (`VIN`) |
| **IR Receiver Module** | Vishay TSOP38238 (38 kHz demodulator) | Captures/decodes handheld IR remote codes for setup & diagnostics |
| **24V AC Sense Relay** | 24V AC Coil DPDT/SPDT Mechanical Relay (e.g., Omron MY2N-J 24VAC) | Provides dry contact closure (`NO` to `COM`) when 24V AC is present |
| **Status LED (Optional)** | Standard 5 mm LED + 220 Ω resistor | Visual blink confirmation whenever an IR burst is transmitted |

---

## Pinout & Wiring Guide

### 1. ESP32 GPIO Connections

| Peripheral Signal | ESP32 Pin | Silkscreen Label | Notes |
| :--- | :--- | :--- | :--- |
| **TSAL6100 IR Driver** | `GPIO 4` | `D4` / `G4` | Drives 2N2222 base through a 1 kΩ resistor (`38 kHz` hardware PWM) |
| **TSOP38238 Data Out** | `GPIO 15` | `D15` / `G15` | `OUT` pin of TSOP38238 (`VCC` to `3V3`, `GND` to `GND`) |
| **24V AC Relay `NO`** | `GPIO 5` | `D5` / `G5` | Configured as `INPUT_PULLUP`; pulls `LOW` when relay energizes |
| **24V AC Relay `COM`** | `GND` | `GND` | Dry contact return **only** — keep strictly isolated from 24V AC |
| **Status LED Anode (+)** | `GPIO 18` | `D18` / `G18` | Optional indicator LED via 220 Ω resistor to `GND` |

### 2. 24V AC Relay Isolation Wiring

> **Safety Warning:** Never connect 24V AC directly to any ESP32 pin. Only the **dry switch contacts** (`COM` and `NO`) of the 24V AC coil relay may connect to the ESP32.

- **Relay Coil Terminal 1:** Channel Master 24V AC Hot
- **Relay Coil Terminal 2:** Channel Master 24V AC Return *(optional RC snubber or 30V–40V MOV across coil terminals to suppress inductive clicks)*
- **Relay `COM`:** ESP32 `GND`
- **Relay `NO`:** ESP32 `GPIO 5`

---

## Software Setup & Installation

### 1. Arduino IDE Prerequisites
1. Install **ESP32 Board Support** (`esp32` by Espressif Systems, v3.0.0 or newer) via **Boards Manager**.
2. Install the **IRremote** library (by Armin Joachimsmeyer, v4.x or newer) via **Library Manager**.

### 2. Configure Wi-Fi & IR Codes
1. Open `SatFullContl/SatFullContl.ino` in the Arduino IDE.
2. Copy `SatFullContl/secrets.example.h` to `SatFullContl/secrets.h` (which is ignored by `.gitignore`) and enter your Wi-Fi SSID and password:
   ```cpp
   #define WIFI_SSID     "Your_WiFi_SSID"
   #define WIFI_PASSWORD "Your_WiFi_Password"
   ```
3. Confirm the Channel Master NEC codes in `SatFullContl.ino` (default `NEC_ADDR = 0xAC`). If your remote uses different command bytes, open the **Serial Monitor at `115200` baud**, point your remote at the **TSOP38238 (`GPIO 15`)**, and press each button to read its exact hex values.
4. Select **ESP32 Dev Module** and upload the sketch. Note the IP address printed in the Serial Monitor.

---

## Connecting Satellite Tracking Software

### SkyRoof
1. Open **Settings → Rotator**.
2. Set Protocol to **Hamlib (`rotctld`)**.
3. Enter your ESP32's IP address and TCP port **`4533`**.
4. Select your target satellite (such as **RS-44**) and click **Track** 2–5 minutes before AOS to allow automatic pre-pointing.

### Gpredict
1. Open **Edit → Preferences → Interfaces → Rotators → Add New**.
2. Configure:
   - **Host:** `<ESP32_IP_ADDRESS>`
   - **Port:** `4533`
   - **Az Type:** `0° -> 360°`
3. Open **Antenna Control**, select your rotator, and click **Track** and **Engage** prior to AOS.

---

## Channel Master Operation Notes

- **Three-Digit Heading Format:** Channel Master digital control boxes require three digits for every heading entry (e.g., `090` for East rather than `90`). `sendHeading()` automatically zero-pads every heading (`%03d`) before transmitting the three NEC digit frames.
- **North Mechanical Stop (`0° / 360°`):** Because TV antenna rotators use a mechanical end-stop at North (`0° / 360°`), any satellite pass crossing due North (`355° -> 005°`) will cause the rotator to unwind counter-clockwise around the dial.

---

## License

This project is released under the [MIT License](LICENSE).
