ESP32 Satellite & TV Antenna Rotator Controller (`SatFullContl`) — GitHub Write-Up & Repository Guide
Author: Brad Vaughn (N0SSE)
License: MIT
An open-source ESP32-based Wi-Fi Web Server, Infrared (IR) Transmitter/Receiver, 24V AC Relay Monitor, and Hamlib rotctld Satellite Tracking Controller built to automate IR-controlled TV antenna rotators (such as Channel Master models) for amateur radio satellite passes (RS-44, SO-50, ISS, etc.) and terrestrial directional pointing.
1. Key Features
Hamlib rotctld Network Interface (TCP Port 4533): Connects directly over Wi-Fi to satellite tracking software such as SkyRoof and Gpredict without serial cables or USB-to-UART bridges.
Automatic AOS Pre-Pointing: When a tracking client connects while a satellite is still below the horizon (El < 0.0°), the controller immediately pre-points the antenna to the Acquisition of Signal (AOS) azimuth so the Yagi is waiting before the pass starts.
Smart Deadband & Rate-Limiting: During active passes (El >= 0.0°), incoming azimuth updates are filtered by a configurable 5° step threshold (MIN_STEP_DEG) and 4000 ms cooldown (MIN_INTERVAL_MS) so the IR rotator control box is never flooded with redundant digit sequences.
Native 38 kHz Hardware PWM IR Transmitter: Bypasses ESP32 Arduino Core 3.x timer conflicts in IRremote by driving a TSAL6100 940 nm IR emitter directly via ledcAttach(4, 38000, 8) to transmit clean 32-bit NEC frames (Address 0xAC).
Built-In IR Remote Code Capture (GPIO 15): Uses a TSOP38238 38 kHz IR receiver module to decode and print any unknown remote button's protocol, address, and command hex values to the Serial Monitor (115200 baud).
Galvanically Isolated 24V AC Activity Sensing (GPIO 5): Monitors a mechanical 24V AC coil relay across the rotator control box's AC output to display live active/idle power state on the web dashboard.
Responsive Mobile & Desktop Web UI (Port 80): Displays live rotctld connection state, target satellite Azimuth and Elevation, commanded 3-digit rotator heading, 24V AC status badge, direct heading input, cardinal presets, and CW/CCW jog controls.
2. Hardware Bill of Materials (BOM)
Component
Specification / Model
Purpose
 
Microcontroller
ESP32 Dev Module (38-pin DevKitC / ESP32S)
Wi-Fi web server, rotctld server, and 38 kHz PWM generation
IR Emitter LED
Vishay TSAL6100 (940 nm, high-power)
Transmits 38 kHz NEC IR commands to the Channel Master control box
IR Driver Transistor
2N2222 NPN (or BS170 N-MOSFET) + 1 kΩ base resistor
Switches peak current (~50–80 mA) through the TSAL6100 from 3.3V GPIO 4
LED Current Resistor
33 Ω – 47 Ω (1/2W)
Limits peak pulsed current to the TSAL6100 from 5V (VIN)
IR Receiver Module
Vishay TSOP38238 (38 kHz demodulator)
Captures/decodes handheld IR remote codes for setup & diagnostics
24V AC Sense Relay
24V AC Coil Mechanical Relay (e.g., Omron MY2N-J 24VAC)
Provides dry contact closure (NO to COM) when 24V AC is present
Status LED (Optional)
Standard 5 mm LED + 220 Ω resistor
Visual blink confirmation whenever an IR burst is transmitted

 
3. Pinout & Wiring Guide
Peripheral Signal
ESP32 Pin
Silkscreen
Wiring Notes
 
TSAL6100 IR Driver
GPIO 4
D4 / G4
Drives 2N2222 base through a 1 kΩ resistor (38 kHz hardware PWM)
TSOP38238 Data Out
GPIO 15
D15 / G15
OUT pin of TSOP38238 (VCC to 3V3, GND to GND)
24V AC Relay NO
GPIO 5
D5 / G5
Configured as INPUT_PULLUP; pulls LOW when relay energizes
24V AC Relay COM
GND
GND
Dry contact return only — keep strictly isolated from 24V AC
Status LED Anode (+)
GPIO 18
D18 / G18
Optional indicator LED via 220 Ω resistor to GND

# ESP32-ir-satellite-rotator
This is for the old Chanel Master IR rotor
