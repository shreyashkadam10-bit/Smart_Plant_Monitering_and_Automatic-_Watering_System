  // ============================================================
//  Smart Plant Monitor & Watering System — ESP32
//  Complete Code with All Features
// ============================================================
//
//  PIN SUMMARY:
//    OLED (I2C)  → SDA=21, SCL=22 (default ESP32 I2C)
//    DHT11       → GPIO 13
//    Soil Sensor → GPIO 34 (ADC)
//    Relay/Pump  → GPIO 15
//    Ultrasonic  → TRIG=25, ECHO=26
//    Buzzer      → GPIO 12
//    IR Sensor   → GPIO 14  (active LOW — LOW when object detected)
//    Touch Pad   → GPIO 4   (ESP32 built-in capacitive touch T0)
//
//  FEATURES ADDED vs. original:
//    1. IR Sensor detection with OLED reaction
//    2. Capacitive Touch Sensor to cycle OLED modes
//    3. Animated OLED Eyes (Normal / Happy / Sleepy / Angry / Surprised)
//    4. WiFi Web Dashboard with live sensor readings
//    5. REST endpoint /data → JSON for remote monitoring
//    6. Water level % + display on OLED (was missing)
//    7. Smart buzzer — 3-beep pattern (non-blocking) only when water low
//    8. Soil moisture percentage calculation
//    9. NaN guard for DHT11 failures
// ============================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>

// ============================================================
//  *** CHANGE THESE TO YOUR WIFI CREDENTIALS ***
// ============================================================
const char* WIFI_SSID = "arsc";
const char* WIFI_PASS = "12345678";

// ============================================================
//  Hardware Pins
// ============================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1

#define DHTPIN        13
#define DHTTYPE       DHT11

#define SOIL_PIN      34    // ADC input-only GPIO
#define RELAY_PIN     15
#define TRIG_PIN      25
#define ECHO_PIN      26
#define BUZZ_PIN      12
#define IR_PIN        14    // INPUT_PULLUP; LOW = someone detected
#define TOUCH_PIN     27    // GPIO4 — ESP32 capacitive touch

// ============================================================
//  Tunable Thresholds
// ============================================================
#define SOIL_DRY_THRESHOLD  3000   // Raw ADC > this → dry soil
#define WATER_EMPTY_CM      7.10f  // Tank empty when sensor reads ≥ this
#define WATER_LOW_CM        6.75f  // Buzzer alert threshold
#define WATER_FULL_CM       1.0f   // Tank full when sensor reads ≤ this
#define TOUCH_THRESHOLD     35     // Capacitive value < this → touched

// ============================================================
//  Instances
// ============================================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);

// ============================================================
//  Global Sensor State
// ============================================================
int   soilRaw     = 0;
int   soilPct     = 0;       // 0=dry … 100=wet
float temperature = 0.0f;
float humidity    = 0.0f;
float waterCm     = 0.0f;    // distance in cm from ultrasonic
int   waterPct    = 0;       // 0=empty … 100=full
bool  pumpRunning = false;
bool  waterLow    = false;
bool  irDetected  = false;

// ============================================================
//  OLED Display Mode
//  Cycling order on each touch:
//    Sensors → Normal Eyes → Happy → Sleepy → Angry → (repeat)
// ============================================================
enum DisplayMode {
  DM_SENSORS = 0,
  DM_EYES_NORMAL,
  DM_EYES_HAPPY,
  DM_EYES_SLEEPY,
  DM_EYES_ANGRY,
  DM_COUNT
};
DisplayMode currentMode = DM_SENSORS;

// ============================================================
//  Timing (non-blocking, all millis-based)
// ============================================================
unsigned long tLastSensor  = 0;
unsigned long tLastBlink   = 0;
unsigned long tLastLook    = 0;
unsigned long tLastTouch   = 0;
unsigned long tBuzzerCycle = 0;

bool isBlinking    = false;
unsigned long tBlinkStart = 0;

// Eye look-around animation
int eyeLookX = 0;
int eyeLookY = 0;
int lookStep = 0;

// Buzzer state
bool  buzzerActive   = false;
int   buzzerBeeps    = 0;
unsigned long tBuzzer = 0;

// Touch debounce
bool lastTouchActive = false;

// ============================================================
//  HTML Dashboard (stored in flash via PROGMEM)
// ============================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Plant Monitor</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--g:#4CAF50;--dg:#2E7D32;--bl:#2196F3;--or:#FF9800;--rd:#f44336;--pu:#9C27B0;--cy:#00BCD4;--bg:#080f1e;--card:#0f1e35;--bd:#1a3352}
body{background:var(--bg);color:#cdd6f4;font-family:'Segoe UI',sans-serif;min-height:100vh}

/* ── Header ── */
header{background:linear-gradient(135deg,#0a1628,#142840);padding:22px 20px;text-align:center;border-bottom:2px solid var(--g);position:relative;overflow:hidden}
header::after{content:'';position:absolute;inset:0;background:radial-gradient(ellipse at 50% 0%,rgba(76,175,80,.12),transparent 70%);animation:hpulse 4s ease-in-out infinite;pointer-events:none}
@keyframes hpulse{0%,100%{opacity:.6}50%{opacity:1}}
h1{font-size:1.9rem;font-weight:800;background:linear-gradient(90deg,#69ff47,#a8ff78,#69ff47);background-size:200%;-webkit-background-clip:text;-webkit-text-fill-color:transparent;animation:shimmer 3s linear infinite;position:relative;z-index:1}
@keyframes shimmer{0%{background-position:0%}100%{background-position:200%}}
.sub{color:#7f9fba;font-size:.85rem;margin-top:5px;position:relative;z-index:1}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--g);box-shadow:0 0 8px var(--g);animation:blink 1.5s ease-in-out infinite;margin-right:5px}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.2}}

/* ── Plant hero ── */
.hero{text-align:center;padding:18px 20px 0;font-size:3.5rem;animation:sway 5s ease-in-out infinite;display:block}
@keyframes sway{0%,100%{transform:rotate(-6deg)}50%{transform:rotate(6deg)}}

/* ── Alert Banner ── */
.alert{display:none;background:rgba(244,67,54,.12);border:1px solid rgba(244,67,54,.35);border-radius:12px;padding:14px 20px;margin:16px 20px;text-align:center;color:#EF9A9A;font-size:.95rem;animation:glow 1.5s ease-in-out infinite}
@keyframes glow{0%,100%{box-shadow:0 0 8px rgba(244,67,54,.2)}50%{box-shadow:0 0 24px rgba(244,67,54,.5)}}

/* ── Grid ── */
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(248px,1fr));gap:18px;padding:22px 20px;max-width:1180px;margin:0 auto}

/* ── Card ── */
.card{background:var(--card);border:1px solid var(--bd);border-radius:16px;padding:22px 20px;position:relative;overflow:hidden;transition:transform .3s,box-shadow .3s;cursor:default}
.card:hover{transform:translateY(-5px);box-shadow:0 16px 48px rgba(0,0,0,.45)}
.card::before{content:'';position:absolute;top:0;left:0;right:0;height:3px;background:var(--ac)}
.cs{--ac:var(--g)} .ct{--ac:var(--or)} .ch{--ac:var(--bl)} .cw{--ac:var(--cy)} .cp{--ac:var(--pu)} .ci{--ac:var(--rd)}

/* Card inner */
.icon{font-size:2.2rem;margin-bottom:10px;display:block}
.label{font-size:.75rem;text-transform:uppercase;letter-spacing:1.2px;color:#546e7a;margin-bottom:6px}
.value{font-size:2.6rem;font-weight:700;background:linear-gradient(135deg,#fff,#90a4ae);-webkit-background-clip:text;-webkit-text-fill-color:transparent;line-height:1}
.unit{font-size:.95rem;font-weight:400;color:#546e7a}

/* Progress bar */
.pb-wrap{margin-top:14px}
.pb-row{display:flex;justify-content:space-between;font-size:.7rem;color:#37474f;margin-bottom:5px}
.pb-bg{height:7px;background:rgba(255,255,255,.06);border-radius:4px;overflow:hidden}
.pb-fill{height:100%;border-radius:4px;background:var(--ac);box-shadow:0 0 9px var(--ac);transition:width .9s cubic-bezier(.4,0,.2,1);position:relative}
.pb-fill::after{content:'';position:absolute;top:0;right:0;width:18px;height:100%;background:linear-gradient(90deg,transparent,rgba(255,255,255,.35));animation:shine 2s linear infinite}
@keyframes shine{0%,100%{opacity:0}50%{opacity:1}}

/* Pump card */
.pstate{display:flex;align-items:center;gap:11px;margin-top:14px;padding:12px 15px;border-radius:10px;font-weight:600;transition:all .5s}
.pon{background:rgba(156,39,176,.18);color:#CE93D8;border:1px solid rgba(156,39,176,.35)}
.poff{background:rgba(26,51,82,.4);color:#455a64;border:1px solid var(--bd)}
.pico{font-size:1.4rem}
.pon .pico{animation:spin 1.1s linear infinite}
@keyframes spin{from{transform:rotate(0)}to{transform:rotate(360deg)}}

/* IR card */
.irstate{display:flex;align-items:center;gap:11px;margin-top:14px;padding:12px 15px;border-radius:10px;font-weight:600;transition:all .5s}
.iryes{background:rgba(244,67,54,.18);color:#EF9A9A;border:1px solid rgba(244,67,54,.3);animation:irpulse 1.2s ease-in-out infinite}
.irno{background:rgba(26,51,82,.4);color:#455a64;border:1px solid var(--bd)}
@keyframes irpulse{0%,100%{box-shadow:0 0 0 0 rgba(244,67,54,.3)}50%{box-shadow:0 0 0 9px rgba(244,67,54,0)}}

/* Footer */
footer{text-align:center;padding:26px 20px;color:#1e3a5f;font-size:.78rem;border-top:1px solid var(--bd);margin-top:10px}

/* Flash */
@keyframes flash{0%{background:rgba(76,175,80,.1)}100%{background:var(--card)}}
.upd{animation:flash .6s ease-out}

/* Raw text */
.raw{margin-top:8px;font-size:.75rem;color:#37474f}
@media(max-width:560px){h1{font-size:1.35rem}.grid{padding:14px 12px}}
</style>
</head>
<body>

<header>
  <h1>&#127807; Smart Plant Monitor</h1>
  <p class="sub"><span class="dot"></span>Live &nbsp;&#8226;&nbsp; ESP32 IoT Dashboard</p>
</header>

<span class="hero">&#129716;</span>

<div class="alert" id="alertBox">&#9888;&#65039; <strong>Water Tank Low!</strong> &nbsp;Please refill the tank immediately.</div>

<div class="grid">

  <div class="card cs" id="cSoil">
    <span class="icon">&#127807;</span>
    <div class="label">Soil Moisture</div>
    <div class="value"><span id="soilPct">--</span><span class="unit"> %</span></div>
    <div class="pb-wrap">
      <div class="pb-row"><span>Dry</span><span>Wet</span></div>
      <div class="pb-bg"><div class="pb-fill" id="soilBar" style="width:0%"></div></div>
    </div>
    <div class="raw">Raw ADC: <span id="soilRaw">--</span></div>
  </div>

  <div class="card ct" id="cTemp">
    <span class="icon">&#127777;&#65039;</span>
    <div class="label">Temperature</div>
    <div class="value"><span id="tempVal">--</span><span class="unit"> &#176;C</span></div>
    <div class="pb-wrap">
      <div class="pb-row"><span>0&#176;C</span><span>50&#176;C</span></div>
      <div class="pb-bg" style="--ac:var(--or)"><div class="pb-fill" id="tempBar" style="width:0%;background:var(--or);box-shadow:0 0 9px var(--or)"></div></div>
    </div>
  </div>

  <div class="card ch" id="cHumid">
    <span class="icon">&#128167;</span>
    <div class="label">Humidity</div>
    <div class="value"><span id="humidVal">--</span><span class="unit"> %</span></div>
    <div class="pb-wrap">
      <div class="pb-row"><span>0%</span><span>100%</span></div>
      <div class="pb-bg" style="--ac:var(--bl)"><div class="pb-fill" id="humidBar" style="width:0%;background:var(--bl);box-shadow:0 0 9px var(--bl)"></div></div>
    </div>
  </div>

  <div class="card cw" id="cWater">
    <span class="icon">&#129695;</span>
    <div class="label">Water Tank Level</div>
    <div class="value"><span id="waterPct">--</span><span class="unit"> %</span></div>
    <div class="pb-wrap">
      <div class="pb-row"><span>Empty</span><span>Full</span></div>
      <div class="pb-bg" style="--ac:var(--cy)"><div class="pb-fill" id="waterBar" style="width:0%;background:var(--cy);box-shadow:0 0 9px var(--cy)"></div></div>
    </div>
    <div class="raw">Distance: <span id="distVal">--</span> cm</div>
  </div>

  <div class="card cp" id="cPump">
    <span class="icon">&#9881;&#65039;</span>
    <div class="label">Water Pump</div>
    <div class="pstate poff" id="pumpState">
      <span class="pico" id="pIco">&#128564;</span>
      <span id="pTxt">Pump OFF</span>
    </div>
    <div class="raw" style="margin-top:14px">Auto: ON when soil dry &amp; tank has water</div>
  </div>

  <div class="card ci" id="cIR">
    <span class="icon">&#128065;&#65039;</span>
    <div class="label">Presence Detection (IR)</div>
    <div class="irstate irno" id="irState">
      <span>&#128694;</span><span id="irTxt">No one nearby</span>
    </div>
    <div class="raw" style="margin-top:14px">OLED eyes react when someone approaches</div>
  </div>

</div>

<footer>
  Smart Plant Monitor v1.0 &nbsp;&#8226;&nbsp; ESP32 + DHT11 + Soil Moisture + Ultrasonic + IR + Capacitive Touch<br>
  Auto-refreshes every 2 seconds
</footer>

<script>
async function refresh(){
  try{
    const r=await fetch('/data');
    const d=await r.json();

    upd('soilPct',d.soil_pct);
    upd('soilRaw',d.soil_raw);
    bar('soilBar',d.soil_pct);
    flash('cSoil');

    upd('tempVal',d.temperature.toFixed(1));
    bar('tempBar',(d.temperature/50)*100);
    flash('cTemp');

    upd('humidVal',d.humidity.toFixed(1));
    bar('humidBar',d.humidity);
    flash('cHumid');

    upd('waterPct',d.water_pct);
    upd('distVal',d.water_cm.toFixed(2));
    bar('waterBar',d.water_pct);
    flash('cWater');

    const ps=document.getElementById('pumpState');
    const pi=document.getElementById('pIco');
    const pt=document.getElementById('pTxt');
    if(d.pump){
      ps.className='pstate pon';pi.textContent='\u{1F4A7}';pt.textContent='Pump ON \u2014 Watering...';
    }else{
      ps.className='pstate poff';pi.textContent='\uD83D\uDE34';pt.textContent='Pump OFF';
    }

    const ir=document.getElementById('irState');
    const it=document.getElementById('irTxt');
    if(d.ir){
      ir.className='irstate iryes';it.textContent='Someone is nearby!';
    }else{
      ir.className='irstate irno';it.textContent='No one nearby';
    }

    document.getElementById('alertBox').style.display=d.water_low?'block':'none';

  }catch(e){console.warn('fetch error',e)}
}

function upd(id,v){document.getElementById(id).textContent=v}
function bar(id,p){document.getElementById(id).style.width=Math.max(0,Math.min(100,p))+'%'}
function flash(id){const e=document.getElementById(id);e.classList.remove('upd');void e.offsetWidth;e.classList.add('upd')}

refresh();
setInterval(refresh,2000);
</script>
</body>
</html>
)rawliteral";

// ============================================================
//  Forward Declarations
// ============================================================
void readSoilMoisture();
void readDHTSensor();
void readWaterLevel();
void controlPump();
void updateBuzzer();
void updateOLED();
void drawSensorScreen();
void drawEyes(bool blink, int lx, int rx, int oy);
void drawHappyEyes(int lx, int rx, int oy);
void drawSleepyEyes(int lx, int rx, int oy);
void drawAngryEyes(int lx, int rx, int oy);
void drawSurprisedEyes(int lx, int rx, int oy);
void handleTouch();
void handleIR();
void handleRoot();
void handleData();

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n========== Smart Plant Monitor ==========");

  // ── Pin Modes ──
  pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, HIGH); // HIGH = pump OFF
  pinMode(TRIG_PIN,  OUTPUT);
  pinMode(ECHO_PIN,  INPUT);
  pinMode(BUZZ_PIN,  OUTPUT); digitalWrite(BUZZ_PIN, LOW);
  pinMode(IR_PIN,    INPUT_PULLUP);

  // ── DHT ──
  dht.begin();

  // ── OLED ──
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("ERROR: OLED not found! Check wiring.");
    while (1);
  }
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(20, 20);
  display.print("Connecting WiFi...");
  display.display();

  // ── WiFi ──
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // Show IP on OLED
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(10, 5);  display.print("WiFi Connected!");
    display.setCursor(10, 20); display.print("Open Browser:");
    display.setTextSize(1);
    display.setCursor(10, 35); display.print(WiFi.localIP());
    display.setCursor(10, 50); display.print("Plant is online!");
    display.display();
    delay(3000);
  } else {
    Serial.println("\nWiFi FAILED — running offline");
    display.clearDisplay();
    display.setCursor(10, 25);
    display.print("WiFi Failed.");
    display.setCursor(10, 38);
    display.print("Offline mode.");
    display.display();
    delay(2000);
  }

  // ── Web Server Routes ──
  server.on("/",     HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.begin();
  Serial.println("Web server started.");
  Serial.println("=========================================");
}

// ============================================================
//  MAIN LOOP
// ============================================================
void loop() {
  server.handleClient();

  unsigned long now = millis();

  // Read sensors every 2 seconds
  if (now - tLastSensor >= 2000) {
    tLastSensor = now;
    readSoilMoisture();
    readDHTSensor();
    readWaterLevel();
    controlPump();
  }

  // Handle IR (checked every loop — fast digital read)
  handleIR();

  // Handle touch with debounce
  handleTouch();

  // Update buzzer state machine
  updateBuzzer();

  // Update OLED display every 100ms (smooth animations)
  if (now - tLastSensor < 2100) {  // always update
    updateOLED();
  }

  // Slight yield for WiFi stack
  delay(20);
}

// ============================================================
//  SENSOR FUNCTIONS
// ============================================================
void readSoilMoisture() {
  soilRaw = analogRead(SOIL_PIN);
  // Map: 4095 (dry air) = 0%, 0 (submerged) = 100%
  soilPct = map(soilRaw, 4095, 0, 0, 100);
  soilPct = constrain(soilPct, 0, 100);
  Serial.printf("Soil Raw: %d  Moisture: %d%%\n", soilRaw, soilPct);
}

void readDHTSensor() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity    = h;

  Serial.printf("Temp: %.1f°C  Humidity: %.1f%%\n", temperature, humidity);
}

void readWaterLevel() {
  // Trigger ultrasonic pulse
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long dur = pulseIn(ECHO_PIN, HIGH, 30000); // 30ms timeout
  waterCm  = (dur * 0.0343f) / 2.0f;

  // Map distance to water %: 1cm (full) = 100%, 7.10cm (empty) = 0%
  waterPct = (int)map((long)(waterCm * 100), (long)(WATER_FULL_CM * 100),
                       (long)(WATER_EMPTY_CM * 100), 100, 0);
  waterPct = constrain(waterPct, 0, 100);

  waterLow = (waterCm >= WATER_LOW_CM);

  Serial.printf("Water: %.2f cm  Level: %d%%  Low: %s\n",
                waterCm, waterPct, waterLow ? "YES" : "no");
}

// ============================================================
//  PUMP CONTROL
// ============================================================
void controlPump() {
  // Pump ON: soil is dry AND tank has water
  if (soilRaw > SOIL_DRY_THRESHOLD && waterCm < WATER_EMPTY_CM) {
    digitalWrite(RELAY_PIN, LOW);  // LOW = relay ON = pump ON
    pumpRunning = true;
    Serial.println(">>> Pump ON");
  } else {
    digitalWrite(RELAY_PIN, HIGH); // HIGH = relay OFF = pump OFF
    pumpRunning = false;
    Serial.println(">>> Pump OFF");
  }
}

// ============================================================
//  BUZZER (non-blocking 3-beep pattern every 10 seconds)
// ============================================================
void updateBuzzer() {
  unsigned long now = millis();

  if (!waterLow) {
    // Water OK → silence buzzer, reset state
    digitalWrite(BUZZ_PIN, LOW);
    buzzerActive = false;
    buzzerBeeps  = 0;
    return;
  }

  // Water is low — run beep cycle
  if (!buzzerActive) {
    if (now - tBuzzerCycle >= 10000) { // wait 10 s between cycles
      buzzerActive = true;
      buzzerBeeps  = 0;
      tBuzzer      = now;
    }
    return;
  }

  // Inside a beep cycle: 3 × (200ms ON + 200ms OFF)
  unsigned long elapsed = now - tBuzzer;
  int beepIndex = elapsed / 400;          // which beep (0, 1, 2)
  int beepPhase = (elapsed % 400) < 200;  // 1=ON, 0=OFF

  if (beepIndex >= 3) {
    // Cycle done
    digitalWrite(BUZZ_PIN, LOW);
    buzzerActive   = false;
    tBuzzerCycle   = now;
  } else {
    digitalWrite(BUZZ_PIN, beepPhase ? HIGH : LOW);
  }
}

// ============================================================
//  IR SENSOR
// ============================================================
void handleIR() {
  // LOW = object detected (active-low sensor with pull-up)
  irDetected = (digitalRead(IR_PIN) == LOW);
}

// ============================================================
//  TOUCH SENSOR (capacitive, cycles through display modes)
// ============================================================
void handleTouch() {
  unsigned long now = millis();
  if (now - tLastTouch < 400) return; // 400ms debounce

  bool touched = (touchRead(TOUCH_PIN) < TOUCH_THRESHOLD);

  if (touched && !lastTouchActive) {
    // Rising edge → advance mode
    currentMode = (DisplayMode)((currentMode + 1) % DM_COUNT);
    tLastTouch  = now;
    Serial.printf("Touch! New display mode: %d\n", currentMode);
  }
  lastTouchActive = touched;
}

// ============================================================
//  OLED UPDATE DISPATCHER
// ============================================================
void updateOLED() {
  unsigned long now = millis();

  // Auto-blink every 3.5 s (only in eye modes)
  bool doBlink = false;
  if (currentMode != DM_SENSORS) {
    if (!isBlinking && (now - tLastBlink >= 3500)) {
      isBlinking  = true;
      tBlinkStart = now;
      tLastBlink  = now;
    }
    if (isBlinking && (now - tBlinkStart >= 150)) {
      isBlinking = false;
    }
    doBlink = isBlinking;
  }

  // Eye look-around: shift pupils every 2.5 s
  if (currentMode != DM_SENSORS && (now - tLastLook >= 2500)) {
    tLastLook = now;
    int patterns[5][2] = {{0,0},{3,2},{-3,2},{0,3},{0,-2}};
    lookStep = (lookStep + 1) % 5;
    eyeLookX = patterns[lookStep][0];
    eyeLookY = patterns[lookStep][1];
  }

  switch (currentMode) {
    case DM_SENSORS:      drawSensorScreen();               break;
    case DM_EYES_NORMAL:  drawEyes(doBlink, 35, 93, 32);   break;
    case DM_EYES_HAPPY:   drawHappyEyes(35, 93, 32);        break;
    case DM_EYES_SLEEPY:  drawSleepyEyes(35, 93, 32);       break;
    case DM_EYES_ANGRY:   drawAngryEyes(35, 93, 32);        break;
    default: break;
  }
}

// ============================================================
//  OLED: SENSOR DATA SCREEN
// ============================================================
void drawSensorScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  // Title bar
  display.fillRect(0, 0, 128, 10, WHITE);
  display.setTextColor(BLACK);
  display.setCursor(28, 1);
  display.print("PLANT MONITOR");
  display.setTextColor(WHITE);

  // Soil Moisture
  display.setCursor(0, 13);
  display.print("Soil:");
  display.print(soilPct);
  display.print("% ");
  display.print(soilRaw > SOIL_DRY_THRESHOLD ? "[DRY]" : "[WET]");

  // Temperature
  display.setCursor(0, 24);
  display.print("Temp:");
  display.print(temperature, 1);
  display.print("C");

  // Humidity
  display.setCursor(0, 35);
  display.print("Hum: ");
  display.print(humidity, 1);
  display.print("%");

  // Water Tank
  display.setCursor(0, 46);
  display.print("Tank:");
  display.print(waterPct);
  display.print("%");
  if (waterLow) {
    display.print(" [LOW!]");
  }

  // Status row: Pump + IR
  display.setCursor(0, 57);
  display.print(pumpRunning ? "Pump:ON " : "Pump:OFF");
  display.print(irDetected  ? " IR:YES" : " IR:NO ");

  display.display();
}

// ============================================================
//  OLED EYE DRAWING HELPERS
//  lx/rx = left/right eye X center
//  oy    = Y center for both eyes
// ============================================================

// Normal eyes (with optional blink)
void drawEyes(bool blink, int lx, int rx, int oy) {
  display.clearDisplay();

  if (blink) {
    // Closed eyes — thin horizontal lines
    for (int i = -1; i <= 1; i++) {
      display.drawFastHLine(lx - 13, oy + i, 26, WHITE);
      display.drawFastHLine(rx - 13, oy + i, 26, WHITE);
    }
  } else {
    int r = 13;
    // Eye whites
    display.fillCircle(lx, oy, r, WHITE);
    display.fillCircle(rx, oy, r, WHITE);
    // Pupils (shift by look offsets)
    display.fillCircle(lx + 3 + eyeLookX, oy + 2 + eyeLookY, 5, BLACK);
    display.fillCircle(rx + 3 + eyeLookX, oy + 2 + eyeLookY, 5, BLACK);
    // Corneal shine
    display.fillCircle(lx + eyeLookX,     oy - 3 + eyeLookY, 2, WHITE);
    display.fillCircle(rx + eyeLookX,     oy - 3 + eyeLookY, 2, WHITE);

    // If IR detected → react: show surprised overlay text
    if (irDetected) {
      display.setTextSize(1);
      display.setCursor(38, 54);
      display.print("!! HI THERE !!");
    }
  }

  display.display();
}

// Happy eyes — curved smile shape
void drawHappyEyes(int lx, int rx, int oy) {
  display.clearDisplay();
  int r = 13;
  // Draw full circle then erase top half → leaves curved bottom = happy arc
  display.fillCircle(lx, oy + 4, r, WHITE);
  display.fillRect(lx - r - 1, oy + 4 - r - 1, 2 * r + 2, r + 1, BLACK);
  display.fillCircle(rx, oy + 4, r, WHITE);
  display.fillRect(rx - r - 1, oy + 4 - r - 1, 2 * r + 2, r + 1, BLACK);

  // Rosy cheeks
  for (int i = 0; i < 3; i++) {
    display.drawFastHLine(lx - 18, oy + 15 + i, 10, WHITE);
    display.drawFastHLine(rx +  8, oy + 15 + i, 10, WHITE);
  }

  display.setTextSize(1);
  display.setCursor(46, 54);
  display.print(":)  HAPPY");
  display.display();
}

// Sleepy eyes — half-open droopy lids
void drawSleepyEyes(int lx, int rx, int oy) {
  display.clearDisplay();
  int r = 13;
  // Full circle then erase upper half
  display.fillCircle(lx, oy, r, WHITE);
  display.fillRect(lx - r - 1, oy - r - 1, 2 * r + 2, r + 2, BLACK);
  display.fillCircle(rx, oy, r, WHITE);
  display.fillRect(rx - r - 1, oy - r - 1, 2 * r + 2, r + 2, BLACK);
  // Droopy pupils
  display.fillCircle(lx, oy + 4, 4, BLACK);
  display.fillCircle(rx, oy + 4, 4, BLACK);
  // ZZZ
  display.setTextSize(1);
  display.setCursor(50, 10);
  display.print("z z z");
  display.setCursor(46, 54);
  display.print("... sleepy");
  display.display();
}

// Angry eyes — eyebrows slanting inward
void drawAngryEyes(int lx, int rx, int oy) {
  display.clearDisplay();
  int r = 13;
  display.fillCircle(lx, oy, r, WHITE);
  display.fillCircle(rx, oy, r, WHITE);
  // Pupils (center)
  display.fillCircle(lx + 2, oy + 2, 5, BLACK);
  display.fillCircle(rx + 2, oy + 2, 5, BLACK);
  // Shines
  display.fillCircle(lx,     oy - 3, 2, WHITE);
  display.fillCircle(rx,     oy - 3, 2, WHITE);
  // Angry eyebrows: outer end up, inner end down (classic scowl)
  display.drawLine(lx - r,     oy - r + 2, lx + r / 2, oy - r - 3, WHITE);
  display.drawLine(lx - r,     oy - r + 3, lx + r / 2, oy - r - 2, WHITE);
  display.drawLine(rx - r / 2, oy - r - 3, rx + r,     oy - r + 2, WHITE);
  display.drawLine(rx - r / 2, oy - r - 2, rx + r,     oy - r + 3, WHITE);

  display.setTextSize(1);
  display.setCursor(42, 54);
  display.print(">:( GRUMPY");
  display.display();
}

// Surprised eyes — wide open, small pupils
void drawSurprisedEyes(int lx, int rx, int oy) {
  display.clearDisplay();
  int r = 16; // extra large
  display.fillCircle(lx, oy, r, WHITE);
  display.fillCircle(rx, oy, r, WHITE);
  // Small centered pupils
  display.fillCircle(lx, oy, 5, BLACK);
  display.fillCircle(rx, oy, 5, BLACK);
  // Shine
  display.fillCircle(lx - 2, oy - 4, 2, WHITE);
  display.fillCircle(rx - 2, oy - 4, 2, WHITE);

  display.setTextSize(1);
  display.setCursor(34, 54);
  display.print("O_O  SURPRISE!");
  display.display();
}

// ============================================================
//  WEB SERVER: Root — Serve Dashboard HTML
// ============================================================
void handleRoot() {
  server.sendHeader("Content-Encoding", "identity");
  server.send_P(200, "text/html", INDEX_HTML);
}

// ============================================================
//  WEB SERVER: /data — JSON Sensor Readings
// ============================================================
void handleData() {
  // Build JSON string manually (no extra library needed)
  String json = "{";
  json += "\"soil_raw\":"   + String(soilRaw)                + ",";
  json += "\"soil_pct\":"   + String(soilPct)                + ",";
  json += "\"temperature\":" + String(temperature, 1)        + ",";
  json += "\"humidity\":"    + String(humidity, 1)           + ",";
  json += "\"water_cm\":"    + String(waterCm, 2)            + ",";
  json += "\"water_pct\":"   + String(waterPct)              + ",";
  json += "\"pump\":"        + (pumpRunning  ? "true" : "false") + ",";
  json += "\"water_low\":"   + (waterLow     ? "true" : "false") + ",";
  json += "\"ir\":"          + (irDetected   ? "true" : "false");
  json += "}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}
