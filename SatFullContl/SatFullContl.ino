#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include "secrets.h"

// Pin definitions
#define IR_SEND_PIN     4   // TSAL6100 Driver Pin (G4 / D4)
#define RELAY_SENSE_PIN 5   // Connected to Relay NO (Relay COM connected to ESP32 GND)
#define STATUS_LED_PIN  18  // Optional external LED (+ 220 ohm resistor)

// ==========================================
// 1. WI-FI & SATELLITE TRACKING SETTINGS
// ==========================================
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Hamlib rotctld TCP port (standard is 4533)
WiFiServer rotctlServer(4533);
WiFiClient rotctlClient;

// Satellite tracking deadband & AOS state
const int MIN_STEP_DEG = 5;
const unsigned long MIN_INTERVAL_MS = 4000;
unsigned long lastSatSendMs = 0;
bool hasPrePointedAOS = false;

// Live target telemetry from SkyRoof / Gpredict
float satTargetAz = 0.0;
float satTargetEl = 0.0;
bool satDataReceived = false;

// ==========================================
// 2. CHANNEL MASTER NEC IR CODES (Address 0xAC)
// ==========================================
const uint16_t NEC_ADDR = 0xAC;

const uint8_t CMD_POWER = 0x0F;
const uint8_t CMD_CW    = 0x10;
const uint8_t CMD_CCW   = 0x11;
const uint8_t CMD_ENTER = 0x12;

const uint8_t CMD_DIGITS[10] = {
  0x00, 0x01, 0x02, 0x03, 0x04,
  0x05, 0x06, 0x07, 0x08, 0x09
};

// Re-wake Channel Master box if idle longer than 45 seconds
const unsigned long BOX_WAKE_TIMEOUT_MS = 45000;
unsigned long lastIrSentMs = 0;

// ==========================================
// 3. SERVER, RELAY SENSE & FEEDBACK STATE
// ==========================================
WebServer server(80);
int lastHeading = -1;
unsigned long cmdCount = 0;
String lastAck = "Waiting for command...";

bool is24VACDetected() {
  if (digitalRead(RELAY_SENSE_PIN) == LOW) {
    delay(20);
    return (digitalRead(RELAY_SENSE_PIN) == LOW);
  }
  return false;
}

void blinkConfirm() {
  digitalWrite(STATUS_LED_PIN, HIGH);
  delay(100);
  digitalWrite(STATUS_LED_PIN, LOW);
}

void recordCommand(String msg) {
  cmdCount++;
  lastAck = "#" + String(cmdCount) + " - " + msg + " [IR Sent]";
  blinkConfirm();
}

// ==========================================
// 4. NATIVE ESP32 38kHz NEC IR TRANSMITTER
// ==========================================
void carrierOn(uint32_t us) {
  ledcWrite(IR_SEND_PIN, 128);
  delayMicroseconds(us);
}

void carrierOff(uint32_t us) {
  ledcWrite(IR_SEND_PIN, 0);
  delayMicroseconds(us);
}

void sendNecByte(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    carrierOn(562);
    if (b & (1 << i)) {
      carrierOff(1687);
    } else {
      carrierOff(562);
    }
  }
}

void sendNecCode(uint8_t command, uint8_t repeats = 0) {
  uint8_t addrLow  = NEC_ADDR & 0xFF;
  uint8_t addrHigh = (NEC_ADDR > 0xFF) ? (NEC_ADDR >> 8) : (~addrLow & 0xFF);

  // Disable interrupts briefly during each 67ms NEC frame for exact 38kHz timing
  noInterrupts();

  carrierOn(9000);
  carrierOff(4500);

  sendNecByte(addrLow);
  sendNecByte(addrHigh);
  sendNecByte(command);
  sendNecByte(~command);

  carrierOn(562);
  carrierOff(0);

  interrupts();

  for (uint8_t r = 0; r < repeats; r++) {
    delay(40);
    noInterrupts();
    carrierOn(9000);
    carrierOff(2250);
    carrierOn(562);
    carrierOff(0);
    interrupts();
  }

  lastIrSentMs = millis();
  yield();
  delay(80);
  yield();
}

void sendHeading(int heading, String sourceTag = "") {
  if (heading < 0) heading = 0;
  if (heading > 360) heading = 360;

  char buf[4];
  snprintf(buf, sizeof(buf), "%03d", heading);

  // Wake Channel Master box if never woken or idle > 45s
  if (lastIrSentMs == 0 || (millis() - lastIrSentMs) > BOX_WAKE_TIMEOUT_MS) {
    int wakeDigit = buf[0] - '0';
    sendNecCode(CMD_DIGITS[wakeDigit], 0);
    unsigned long wakeStart = millis();
    while (millis() - wakeStart < 4500) {
      server.handleClient();
      delay(15);
    }
  }

  delay(80);
  for (int i = 0; i < 3; i++) {
    int digit = buf[i] - '0';
    sendNecCode(CMD_DIGITS[digit], 0);
    delay(320);
    yield();
  }

  // sendNecCode(CMD_ENTER, 0); // Uncomment if your box requires ENTER
  lastHeading = heading;
  lastSatSendMs = millis();
  recordCommand(sourceTag + "Heading " + String(buf) + "&deg;");
}

int angularDiff(int a, int b) {
  if (b < 0) return 180;
  int d = abs(a - b) % 360;
  return (d > 180) ? (360 - d) : d;
}

// ==========================================
// 5. HAMLIB ROTCTLD HANDLER (SkyRoof / Gpredict)
// ==========================================
void handleRotctl() {
  WiFiClient newClient = rotctlServer.available();
  if (newClient) {
    if (rotctlClient && rotctlClient.connected()) {
      rotctlClient.stop();
    }
    rotctlClient = newClient;
    hasPrePointedAOS = false;
    Serial.println("Satellite tracker connected to rotctld (port 4533)");
  }

  if (!rotctlClient || !rotctlClient.connected() || !rotctlClient.available()) {
    return;
  }

  bool gotNewPos = false;

  while (rotctlClient.available()) {
    String cmd = rotctlClient.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0) continue;

    if (cmd == "p" || cmd == "\\get_pos") {
      float reportedAz = (lastHeading >= 0) ? (float)lastHeading : 0.0;
      rotctlClient.printf("%.1f\n0.0\n", reportedAz);
    }
    else if (cmd.startsWith("P ") || cmd.startsWith("\\set_pos ")) {
      int firstSpace = cmd.indexOf(' ');
      int secondSpace = cmd.indexOf(' ', firstSpace + 1);
      if (firstSpace > 0 && secondSpace > firstSpace) {
        satTargetAz = cmd.substring(firstSpace + 1, secondSpace).toFloat();
        satTargetEl = cmd.substring(secondSpace + 1).toFloat();

        // Auto-unflip overhead pass coordinates if tracker sends El > 90 deg
        if (satTargetEl > 90.0 && satTargetEl <= 180.0) {
          satTargetEl = 180.0 - satTargetEl;
          satTargetAz = fmod(satTargetAz + 180.0, 360.0);
        }

        satDataReceived = true;
        gotNewPos = true;
        rotctlClient.println("RPRT 0");
      } else {
        rotctlClient.println("RPRT -1");
      }
    }
    else if (cmd == "S" || cmd == "\\stop") {
      rotctlClient.println("RPRT 0");
    }
    else if (cmd == "q" || cmd == "\\quit") {
      rotctlClient.stop();
      return;
    }
    else if (cmd == "\\dump_state") {
      rotctlClient.println("0\n2\n0.0\n360.0\n0.0\n0.0");
    }
    else {
      rotctlClient.println("RPRT 0");
    }
  }

  if (gotNewPos) {
    int targetAz = (int)round(satTargetAz) % 360;
    if (targetAz < 0) targetAz += 360;

    // 1. Immediate AOS Pre-Point when satellite is below the horizon (El < 0)
    if (satTargetEl < 0.0 && !hasPrePointedAOS && angularDiff(targetAz, lastHeading) >= 2) {
      Serial.printf("AOS PRE-POINT -> Moving to %03d deg (El: %.1f)\n", targetAz, satTargetEl);
      sendHeading(targetAz, "[AOS Pre-Point] ");
      hasPrePointedAOS = true;
    }
    // 2. Active Pass Tracking (El >= 0) with 5-degree / 4-second deadband
    else if (satTargetEl >= 0.0) {
      hasPrePointedAOS = false;
      if (angularDiff(targetAz, lastHeading) >= MIN_STEP_DEG &&
          (millis() - lastSatSendMs) >= MIN_INTERVAL_MS) {
        Serial.printf("SAT TRACK -> Target Az: %03d (El: %.1f)\n", targetAz, satTargetEl);
        sendHeading(targetAz, "[SAT] ");
      }
    }
  }
}

// ==========================================
// 6. WEB SERVER ROUTES & UI
// ==========================================
void handleStatus() {
  bool active = is24VACDetected();
  bool trackerConnected = (rotctlClient && rotctlClient.connected());

  char cmdBuf[5];
  if (lastHeading >= 0) {
    snprintf(cmdBuf, sizeof(cmdBuf), "%03d", lastHeading);
  } else {
    snprintf(cmdBuf, sizeof(cmdBuf), "---");
  }

  String json = "{";
  json += "\"active\":" + String(active ? "true" : "false") + ",";
  json += "\"ack\":\"" + lastAck + "\",";
  json += "\"tracker\":" + String(trackerConnected ? "true" : "false") + ",";
  json += "\"hasSat\":" + String(satDataReceived ? "true" : "false") + ",";
  json += "\"satAz\":" + String(satTargetAz, 1) + ",";
  json += "\"satEl\":" + String(satTargetEl, 1) + ",";
  json += "\"cmdHeading\":\"" + String(cmdBuf) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleRoot() {
  bool active = is24VACDetected();
  String pwrText  = active ? "24V AC DETECTED (Active)" : "NO 24V AC (Off / Idle)";
  String pwrColor = active ? "#2e7d32" : "#c62828";
  int inputVal = (lastHeading >= 0) ? lastHeading : 0;

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>N0SSE Satellite &amp; Rotator Control</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; background: #121212; color: #e0e0e0; text-align: center; padding: 20px; }
    .card { max-width: 400px; margin: auto; background: #1e1e1e; padding: 20px; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.4); }
    h2 { margin-top: 0; color: #4fc3f7; }
    .pwr-badge { padding: 10px; border-radius: 6px; margin-bottom: 12px; font-weight: bold; color: #fff; background: )rawliteral" + pwrColor + R"rawliteral(; }
    .status { background: #263238; padding: 10px; border-radius: 6px; margin-bottom: 14px; font-size: 0.95rem; transition: background 0.3s; }
    .sat-box { background: #17202a; border: 1px solid #2c3e50; border-radius: 8px; padding: 12px; margin-bottom: 16px; }
    .sat-hdr { font-size: 0.78rem; color: #90caf9; text-transform: uppercase; letter-spacing: 0.5px; margin-bottom: 8px; display: flex; justify-content: space-between; }
    .sat-grid { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 8px; }
    .sat-cell { background: #212f3d; padding: 8px 4px; border-radius: 6px; }
    .sat-lbl { font-size: 0.7rem; color: #b0bec5; }
    .sat-val { font-size: 1.15rem; font-weight: bold; font-family: monospace; color: #fff; margin-top: 2px; }
    input[type=number] { width: 120px; padding: 10px; font-size: 1.2rem; text-align: center; border-radius: 6px; border: 1px solid #444; background: #2c2c2c; color: #fff; }
    button { padding: 12px 18px; font-size: 1rem; font-weight: bold; border: none; border-radius: 6px; cursor: pointer; margin: 6px; }
    .btn-go { background: #0288d1; color: white; }
    .btn-pwr { background: #6a1b9a; color: white; width: 90%; margin-bottom: 12px; }
    .btn-jog { background: #37474f; color: white; width: 42%; }
    .presets { margin-top: 15px; }
    .btn-pre { background: #2e7d32; color: white; padding: 8px 12px; font-size: 0.9rem; }
  </style>
</head>
<body>
  <div class="card">
    <h2>N0SSE Rotator Control</h2>

    <!-- <div id="pwrBadge" class="pwr-badge">)rawliteral" + pwrText + R"rawliteral(</div> -->
    <div id="ackBox" class="status">)rawliteral" + lastAck + R"rawliteral(</div>

    <div class="sat-box">
      <div class="sat-hdr">
        <span>Satellite Telemetry</span>
        <span id="trkState" style="color:#ef5350;">rotctld: Idle</span>
      </div>
      <div class="sat-grid">
        <div class="sat-cell">
          <div class="sat-lbl">Target Az</div>
          <div id="satAz" class="sat-val">---.-&deg;</div>
        </div>
        <div class="sat-cell">
          <div class="sat-lbl">Target El</div>
          <div id="satEl" class="sat-val">--.-&deg;</div>
        </div>
        <div class="sat-cell">
          <div class="sat-lbl">Rotator Cmd</div>
          <div id="cmdHdg" class="sat-val">---&deg;</div>
        </div>
      </div>
    </div>

    <!-- <a href="/power"><button class="btn-pwr">Toggle Box Power (IR)</button></a> -->

    <form action="/set" method="GET">
      <input type="number" name="deg" min="0" max="360" value=")rawliteral" + String(inputVal) + R"rawliteral(" required>
      <button type="submit" class="btn-go">Rotate</button>
    </form>

    <div class="presets">
      <a href="/set?deg=000"><button class="btn-pre">N (000&deg;)</button></a>
      <a href="/set?deg=090"><button class="btn-pre">E (090&deg;)</button></a>
      <a href="/set?deg=180"><button class="btn-pre">S (180&deg;)</button></a>
      <a href="/set?deg=270"><button class="btn-pre">W (270&deg;)</button></a>
    </div>

    <hr style="border-color:#333; margin: 20px 0;">

    <div>
      <a href="/jog?dir=ccw"><button class="btn-jog">&#9664; CCW</button></a>
      <a href="/jog?dir=cw"><button class="btn-jog">CW &#9654;</button></a>
    </div>
  </div>

  <script>
    let prevAck = "";
    setInterval(() => {
      fetch('/status')
        .then(r => r.json())
        .then(data => {
          const badge = document.getElementById('pwrBadge');
          if (badge) {
            if (data.active) {
              badge.textContent = '24V AC DETECTED (Active)';
              badge.style.background = '#2e7d32';
            } else {
              badge.textContent = 'NO 24V AC (Off / Idle)';
              badge.style.background = '#c62828';
            }
          }

          const trk = document.getElementById('trkState');
          if (data.tracker) {
            trk.textContent = 'rotctld: Connected';
            trk.style.color = '#66bb6a';
          } else {
            trk.textContent = 'rotctld: Idle';
            trk.style.color = '#ef5350';
          }

          if (data.hasSat) {
            document.getElementById('satAz').innerHTML = data.satAz.toFixed(1) + '&deg;';
            const elEl = document.getElementById('satEl');
            elEl.innerHTML = data.satEl.toFixed(1) + '&deg;';
            elEl.style.color = (data.satEl >= 0) ? '#66bb6a' : '#ffa726';
          }
          document.getElementById('cmdHdg').innerHTML = data.cmdHeading + '&deg;';

          const ackBox = document.getElementById('ackBox');
          if (data.ack && data.ack !== prevAck) {
            ackBox.innerHTML = data.ack;
            ackBox.style.background = '#1b5e20';
            setTimeout(() => { ackBox.style.background = '#263238'; }, 800);
            prevAck = data.ack;
          }
        });
    }, 2000);
  </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handlePower() {
  sendNecCode(CMD_POWER, 0);
  recordCommand("Power Toggle");
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSet() {
  if (server.hasArg("deg")) {
    sendHeading(server.arg("deg").toInt());
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleJog() {
  if (server.hasArg("dir")) {
    String dir = server.arg("dir");
    if (dir == "cw") {
      sendNecCode(CMD_CW, 2);
      recordCommand("Jog CW");
    } else if (dir == "ccw") {
      sendNecCode(CMD_CCW, 2);
      recordCommand("Jog CCW");
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

// ==========================================
// 7. SETUP & MAIN LOOP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(RELAY_SENSE_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  ledcAttach(IR_SEND_PIN, 38000, 8);
  ledcWrite(IR_SEND_PIN, 0);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  // Lower Wi-Fi TX power (11 dBm) to minimize 3.3V rail spikes & RF hash near SDR
  WiFi.setTxPower(WIFI_POWER_11dBm);

  Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/power", handlePower);
  server.on("/set", handleSet);
  server.on("/jog", handleJog);
  server.begin();

  rotctlServer.begin();
  Serial.println("Hamlib rotctld server listening on port 4533");
}

void loop() {
  server.handleClient();
  handleRotctl();
  delay(5);
}