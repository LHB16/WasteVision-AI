#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESP32Servo.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <DHT.h>
#include "Audio.h"
#include <WebServer.h>
#define SERVER_PORT 5000
#include <HTTPClient.h>
static bool binFullLock = false;
bool binFullLock_P = false;
bool binFullLock_PA = false;
bool binFullLock_M = false;

// === PROTOTYPE DECLARATIONS ===
void speakEN(const String &en);
void sendDetectToServer(const String &type);
void handleStatus();
static void sendCaptureToAI();
void drawWelcomePage(const char *line);
bool isObjectPresent();

String serverIP = "192.168.137.1";

// Config
const char *ssid = "Quoc Inh";
const char *password = "chauquocinh";
#define localPort 4210
WebServer server(80);

// FULL THRESHOLD (ADJUST HERE)
static const int FULL_THRESHOLD = 92; // >=92% Full

#define TFT_CS 15
#define TFT_DC 2
#define TFT_RST 4
#define TFT_BL 21

#define PIN_TRIG 1
#define ECHO_P 10
#define ECHO_PA 6
#define ECHO_M 7

#define PIN_GAS 5
#define ECHO_DETECT 9

#define I2S_LRC 35
#define I2S_BCLK 36
#define I2S_DOUT 37

// ===================== BUZZER / ALARM =====================
#define BUZZER_PIN 8
#define BUZZER_ACTIVE_HIGH 1

static const float TEMP_ALARM_C = 45;
static const int GAS_ALARM_RAW = 500;

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, 13, 12, TFT_RST);

WiFiUDP udp;
Servo sXoay, sCua;
DHT dht(47, DHT11);
Audio audio;

char packetBuffer[255];
IPAddress aiIP;
bool aiIPValid = false;
static const uint16_t AI_CAPTURE_PORT = 5005;

// color
static const uint16_t C_BG = 0x0000;
static const uint16_t C_WHITE = 0xFFFF;
static const uint16_t C_CYAN = 0x07FF;
static const uint16_t C_BLUE = 0x001F;
static const uint16_t C_ORANGE = 0xFDA0;
static const uint16_t C_MAGENTA = 0xF81F;
static const uint16_t C_GREEN = 0x07E0;
static const uint16_t C_RED = 0xF800;
static const uint16_t C_GRAY = 0x8410;
static const uint16_t C_NAVY = 0x0012;

unsigned long lastTempAlertMs = 0;
static const uint32_t TEMP_ALERT_INTERVAL = 20000;

// check out lcd
enum Page
{
  PAGE_BOOT,
  PAGE_WAIT_AI,
  PAGE_WELCOME,
  PAGE_DASH,
  PAGE_PROCESS,
  PAGE_FULL
};
Page currentPage = PAGE_BOOT;

bool wifiReady = false;
bool aiLinked = false;
bool systemReady = false;

// welcome timing
unsigned long welcomeUntil = 0;
unsigned long lastWelcomeSpeech = 0;
static const uint32_t WELCOME_SHOW_MS = 2200;
static const uint32_t WELCOME_COOLDOWN_MS = 12000;

// FULL warning page timing
unsigned long fullUntil = 0;
static const uint32_t FULL_SHOW_MS = 2200;

// ===================== DASHBOARD STATE =====================
int oldP = -1000, oldPa = -1000, oldM = -1000;
int oldGas = -1000;
float oldTemp = -999, oldHumi = -999;

float fP = 0, fPa = 0, fM = 0;
int binP = 0, binPa = 0, binM = 0;

static const int GAS_WIN = 16;
int gasBuf[GAS_WIN];
int gasIdx = 0;
long gasSum = 0;
bool gasInit = false;
int gasSmoothed = 0;

// --- Baseline calibration ---
static const uint32_t GAS_WARMUP_MS = 8000;
static const uint32_t GAS_CALIB_MS = 6000;
static const uint32_t GAS_CALIB_STEP = 50;

bool gasCalibrated = false;
unsigned long gasBootMs = 0;
unsigned long gasCalibStartMs = 0;
unsigned long gasCalibLastMs = 0;
long gasBaseSum = 0;
int gasBaseCount = 0;
int gasBaseline = 0;

// --- Threshold theo delta so với baseline ---
static const int GAS_DELTA_WARN = 110;
static const int GAS_DELTA_DANGER = 220;

// --- Hysteresis ---
static const int GAS_HYS = 25;

// --- Debounce ---
static const uint32_t GAS_LEVEL_HOLD_MS = 900;

enum GasLevel
{
  GAS_SAFE = 0,
  GAS_WARN = 1,
  GAS_DANGER = 2
};
GasLevel gasLevel = GAS_SAFE;
GasLevel gasLevelCandidate = GAS_SAFE;
unsigned long gasLevelChangeMs = 0;

int readGasSmoothedRaw()
{
  int v = analogRead(PIN_GAS);

  if (!gasInit)
  {
    gasSum = 0;
    for (int i = 0; i < GAS_WIN; i++)
    {
      gasBuf[i] = v;
      gasSum += v;
    }
    gasIdx = 0;
    gasInit = true;
  }
  else
  {
    gasSum -= gasBuf[gasIdx];
    gasBuf[gasIdx] = v;
    gasSum += v;
    gasIdx = (gasIdx + 1) % GAS_WIN;
  }
  gasSmoothed = (int)(gasSum / GAS_WIN);
  return gasSmoothed;
}

static inline const char *gasLevelText(GasLevel lv)
{
  switch (lv)
  {
  case GAS_SAFE:
    return "SAFE";
  case GAS_WARN:
    return "WARN";
  case GAS_DANGER:
    return "DANGER";
  default:
    return "SAFE";
  }
}

static inline int gasLevelCodeForUI()
{
  if (!gasCalibrated)
    return -1;
  return (int)gasLevel;
}

void gasSetup()
{
  gasBootMs = millis();
  gasCalibrated = false;
  gasBaseline = 0;
  gasBaseSum = 0;
  gasBaseCount = 0;

  gasLevel = GAS_SAFE;
  gasLevelCandidate = GAS_SAFE;
  gasLevelChangeMs = millis();

  gasInit = false;
}

unsigned long lastGasAlertMs = 0;
static const uint32_t GAS_ALERT_INTERVAL = 15000;

void gasLoopUpdate()
{
  int g = readGasSmoothedRaw();
  unsigned long now = millis();

  // 1) Warmup
  if (now - gasBootMs < GAS_WARMUP_MS)
  {
    gasCalibStartMs = now;
    return;
  }

  // 2) Calibrate baseline
  if (!gasCalibrated)
  {
    if (gasCalibStartMs == 0)
      gasCalibStartMs = now;
    if (now - gasCalibStartMs <= GAS_CALIB_MS)
    {
      if (now - gasCalibLastMs >= GAS_CALIB_STEP)
      {
        gasCalibLastMs = now;
        gasBaseSum += g;
        gasBaseCount++;
      }
      return;
    }
    else
    {
      if (gasBaseCount > 0)
        gasBaseline = (int)(gasBaseSum / gasBaseCount);
      else
        gasBaseline = g;
      gasCalibrated = true;
      gasLevel = GAS_SAFE;
      gasLevelCandidate = GAS_SAFE;
      gasLevelChangeMs = now;
      return;
    }
  }

  // 3) Delta
  int delta = g - gasBaseline;
  if (delta < 0)
    delta = 0;

  // 4) Decide target with hysteresis
  GasLevel target = gasLevel;
  if (gasLevel == GAS_SAFE)
  {
    if (delta >= GAS_DELTA_DANGER)
      target = GAS_DANGER;
    else if (delta >= GAS_DELTA_WARN)
      target = GAS_WARN;
  }
  else if (gasLevel == GAS_WARN)
  {
    if (delta >= GAS_DELTA_DANGER)
      target = GAS_DANGER;
    else if (delta <= (GAS_DELTA_WARN - GAS_HYS))
      target = GAS_SAFE;
  }
  else
  { // GAS_DANGER
    if (delta <= (GAS_DELTA_DANGER - GAS_HYS))
    {
      if (delta >= GAS_DELTA_WARN)
        target = GAS_WARN;
      else
        target = GAS_SAFE;
    }
  }

  // 5) Debounce đổi level
  if (target != gasLevelCandidate)
  {
    gasLevelCandidate = target;
    gasLevelChangeMs = now;
  }

  if (gasLevelCandidate != gasLevel && (now - gasLevelChangeMs >= GAS_LEVEL_HOLD_MS))
  {
    gasLevel = gasLevelCandidate;

    if (gasLevel == GAS_DANGER)
    {
      speakEN("Warning. Gas leak detected. Please check immediately.");
      lastGasAlertMs = now;
    }
  }

  if (gasLevel == GAS_DANGER && (now - lastGasAlertMs > GAS_ALERT_INTERVAL))
  {
    speakEN("Gas level is still dangerous.");
    lastGasAlertMs = now;
  }
}

bool gasAlarmActive()
{
  return gasCalibrated && (gasLevel == GAS_DANGER);
}

// ===================== DHT FILTER =====================
float tFilt = NAN, hFilt = NAN;

static inline void feedAudioOnce()
{
  audio.loop();
}

void waitWithAudio(uint32_t ms)
{
  unsigned long t0 = millis();
  while (millis() - t0 < ms)
  {
    feedAudioOnce();
    delay(1);
  }
}

static const int TTS_VOL = 12;
static const uint32_t TTS_MIN_GAP_MS = 120;
unsigned long lastTtsStartMs = 0;

static inline bool isUnreservedChar(uint8_t c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}
String urlEncodeUTF8(const String &s)
{
  String out;
  out.reserve(s.length() * 3);
  const uint8_t *bytes = (const uint8_t *)s.c_str();
  for (size_t i = 0; i < s.length(); i++)
  {
    uint8_t c = bytes[i];
    if (c == 0)
      break;
    if (isUnreservedChar(c))
      out += (char)c;
    else
    {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

struct TTSItem
{
  String lang;
  String text;
};
static const int TTS_Q_MAX = 8;
TTSItem ttsQ[TTS_Q_MAX];
int ttsHead = 0, ttsTail = 0;

bool ttsEmpty() { return ttsHead == ttsTail; }
bool ttsFull() { return ((ttsTail + 1) % TTS_Q_MAX) == ttsHead; }

void ttsEnqueue(const String &lang, const String &text)
{
  if (!text.length())
    return;
  if (ttsFull())
    ttsHead = (ttsHead + 1) % TTS_Q_MAX;
  ttsQ[ttsTail] = {lang, text};
  ttsTail = (ttsTail + 1) % TTS_Q_MAX;
}

void ttsStart(const String &lang, const String &text)
{
  audio.setVolume(TTS_VOL);
  String q = urlEncodeUTF8(text);
  String url = "https://translate.google.com/translate_tts?ie=UTF-8&q=" + q + "&tl=" + lang + "&client=tw-ob";
  audio.connecttohost(url.c_str());
  lastTtsStartMs = millis();
}

void ttsLoop()
{
  if (ttsEmpty())
    return;
  if (millis() - lastTtsStartMs < TTS_MIN_GAP_MS)
    return;
  if (audio.isRunning())
    return;

  TTSItem it = ttsQ[ttsHead];
  ttsHead = (ttsHead + 1) % TTS_Q_MAX;
  ttsStart(it.lang, it.text);
}

// SPEAK FUNCTION - MUST BE DECLARED BEFORE USE
void speakEN(const String &en) { 
  ttsEnqueue("en", en); 
}

// ===================== ULTRASONIC (MEDIAN 5 + EMA) =====================
long readUltrasonicRawUS(int echoPin)
{
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  long us = pulseIn(echoPin, HIGH, 8000);
  return us;
}

int durationToDistanceCM(long duration)
{
  if (duration <= 0)
    return -1;
  return (int)(duration * 0.034 / 2);
}

int median5_distanceCM(int echoPin)
{
  int a[5], n = 0;
  for (int i = 0; i < 5; i++)
  {
    long du = readUltrasonicRawUS(echoPin);
    int d = durationToDistanceCM(du);
    if (d >= 2 && d <= 80)
      a[n++] = d;

    feedAudioOnce();
    delay(1);
  }
  if (n == 0)
    return -1;
  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
    {
      if (a[j] < a[i])
      {
        int t = a[i];
        a[i] = a[j];
        a[j] = t;
      }
    }
  return a[n / 2];
}

int distanceToPercent(int cm)
{
  if (cm < 0)
    return 0;
  return constrain(map(cm, 5, 27, 100, 0), 0, 100);
}

int filteredBinPercent(int echoPin, float &emaState)
{
  int cm = median5_distanceCM(echoPin);
  int p = distanceToPercent(cm);
  const float alpha = 0.30f;
  emaState = alpha * p + (1 - alpha) * emaState;
  return constrain((int)roundf(emaState), 0, 100);
}

int quickReadBinPercent(int echoPin)
{
  int cm = median5_distanceCM(echoPin);
  return distanceToPercent(cm);
}

// ===================== DHT FILTER =====================
void readDHTFiltered(float &tOut, float &hOut)
{
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t) && !isnan(h))
  {
    const float alpha = 0.28f;
    if (isnan(tFilt))
      tFilt = t;
    else
      tFilt = alpha * t + (1 - alpha) * tFilt;
    if (isnan(hFilt))
      hFilt = h;
    else
      hFilt = alpha * h + (1 - alpha) * hFilt;
  }
  tOut = tFilt;
  hOut = hFilt;
}

// ===================== CMD NORMALIZE =====================
String normalizeCmd(String s)
{
  s.trim();
  s.toUpperCase();

  int cut = -1;
  for (int i = 0; i < (int)s.length(); i++)
  {
    char c = s[i];
    if (!((c >= 'A' && c <= 'Z') || c == '_'))
    {
      cut = i;
      break;
    }
  }
  if (cut > 0)
    s = s.substring(0, cut);

  if (s == "METAL")
    s = "METAL";
  return s;
}

static inline bool isValidWasteCmd(const String &c)
{
  return (c == "PLASTIC" || c == "PAPER" || c == "METAL");
}

// ===================== UI =====================
void setPage(Page p) { currentPage = p; }

void drawHeader(const char *title)
{
  tft.fillRect(0, 0, 320, 52, C_NAVY);
  tft.drawFastHLine(0, 52, 320, C_CYAN);
  tft.setTextSize(2);
  tft.setTextColor(C_WHITE);
  tft.setCursor(10, 10);
  tft.print(title);
}

void drawBoot()
{
  setPage(PAGE_BOOT);
  tft.fillScreen(C_BG);
  drawHeader("WASTEVISION");
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(18, 90);
  tft.print("Connecting WiFi...");
}

void drawWaitAI()
{
  setPage(PAGE_WAIT_AI);
  tft.fillScreen(C_BG);
  drawHeader("WASTEVISION");

  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(18, 78);
  tft.print("WiFi: OK");

  tft.setTextColor(C_WHITE);
  tft.setCursor(18, 108);
  tft.print("IP: ");
  tft.print(WiFi.localIP().toString());

  tft.setTextColor(C_CYAN);
  tft.setCursor(18, 138);
  tft.print("UDP Port: ");
  tft.print(localPort);

  tft.setTextColor(C_WHITE);
  tft.setCursor(18, 168);
  tft.print("AI: WAITING LINK...");

  tft.setTextSize(1);
  tft.setTextColor(C_GRAY);
  tft.setCursor(18, 210);
  tft.print("He thong chi van hanh khi AI da link");
}

void drawWelcomePage(const char *line)
{
  setPage(PAGE_WELCOME);
  tft.fillScreen(C_BG);
  drawHeader("XIN CHAO!");

  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(18, 92);
  tft.print(line);

  tft.setTextSize(3);
  tft.setTextColor(C_WHITE);
  tft.setCursor(36, 130);
  tft.print("WASTE VISION");

  tft.setTextSize(1);
  tft.setTextColor(C_GRAY);
  tft.setCursor(18, 210);
  tft.print("Hay bo rac dung loai de doi qua");
}

void drawFullBinPage(const String &binName, int pct)
{
  setPage(PAGE_FULL);
  tft.fillScreen(C_BG);
  drawHeader("XIN LOI");

  tft.setTextSize(2);
  tft.setTextColor(C_RED);
  tft.setCursor(18, 90);
  tft.print("Thung da day:");

  tft.setTextSize(3);
  tft.setTextColor(C_WHITE);
  tft.setCursor(18, 125);
  tft.print(binName);

  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(18, 170);
  tft.printf("Muc day: %d%%", pct);

  tft.setTextSize(1);
  tft.setTextColor(C_GRAY);
  tft.setCursor(18, 210);
  tft.print("Vui long dung thung khac");
}

void drawDashFrame()
{
  setPage(PAGE_DASH);
  tft.fillScreen(C_BG);

  tft.fillRect(0, 0, 320, 34, C_NAVY);
  tft.drawFastHLine(0, 34, 320, C_CYAN);
  tft.setTextSize(2);
  tft.setTextColor(C_WHITE);
  tft.setCursor(8, 8);
  tft.print("WASTEVISION MONITOR");

  tft.fillRect(0, 34, 320, 18, C_BG);
  tft.drawFastHLine(0, 52, 320, C_GRAY);

  tft.drawRoundRect(16, 62, 92, 122, 12, C_WHITE);
  tft.drawRoundRect(114, 62, 92, 122, 12, C_WHITE);
  tft.drawRoundRect(212, 62, 92, 122, 12, C_WHITE);

  tft.fillRoundRect(10, 195, 300, 40, 12, C_NAVY);
  tft.drawRoundRect(10, 195, 300, 40, 12, C_CYAN);

  oldP = oldPa = oldM = -1000;
  oldGas = -1000;
  oldTemp = oldHumi = -999;
}

void drawStatusLineReady()
{
  tft.fillRect(0, 36, 320, 14, C_BG);
  tft.setTextSize(1);

  tft.setCursor(8, 38);
  tft.setTextColor(C_GREEN);
  tft.print("AI: LINKED");

  tft.setCursor(92, 38);
  tft.setTextColor(C_CYAN);
  tft.print("IP:");

  tft.setTextColor(C_WHITE);
  tft.print(WiFi.localIP().toString());
}

void updateGaugeInner(int x, int y, int w, int h, int p, int &oldVal, const char *name, uint16_t barColor)
{
  if (abs(p - oldVal) < 2)
    return;

  int innerH = h - 28;
  int barH = map(p, 0, 100, 0, innerH);

  tft.fillRoundRect(x + 4, y + 6, w - 8, innerH, 10, C_BG);
  tft.fillRoundRect(x + 10, y + 6 + (innerH - barH), w - 20, barH, 8, barColor);

  tft.setTextSize(1);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(x + 8, y + h - 18);
  tft.print("                ");
  tft.setCursor(x + 8, y + h - 18);
  tft.printf("%s %3d%%", name, p);

  oldVal = p;
}

void updateEnv()
{
  float t, h;
  readDHTFiltered(t, h);

  int gCode = gasLevelCodeForUI();

  bool need = false;
  if (!isnan(t) && fabs(t - oldTemp) > 0.6f)
    need = true;
  if (!isnan(h) && fabs(h - oldHumi) > 1.2f)
    need = true;
  if (gCode != oldGas)
    need = true;
  if (!need)
    return;

  tft.fillRoundRect(12, 197, 296, 36, 12, C_NAVY);

  tft.setTextSize(2);
  if (!isnan(t) && !isnan(h))
  {
    tft.setTextColor(C_GREEN, C_NAVY);
    tft.setCursor(18, 205);
    tft.printf("T:%2.0fC  H:%2.0f%%", t, h);
    oldTemp = t;
    oldHumi = h;
  }
  else
  {
    tft.setTextColor(C_GRAY, C_NAVY);
    tft.setCursor(18, 205);
    tft.print("DHT: --");
  }

  const char *gl = gasCalibrated ? gasLevelText(gasLevel) : "CAL...";
  uint16_t gColor = C_GRAY;
  if (gasCalibrated)
  {
    if (gasLevel == GAS_SAFE)
      gColor = C_GREEN;
    else if (gasLevel == GAS_WARN)
      gColor = C_ORANGE;
    else
      gColor = C_RED;
  }

  tft.setTextColor(gColor, C_NAVY);
  tft.setCursor(220, 205);
  tft.print("G:");
  tft.print(gl);

  oldGas = gCode;
}

void dashboardLoopDraw()
{
  if (currentPage != PAGE_DASH)
    return;
  drawStatusLineReady();
  updateGaugeInner(16, 62, 92, 122, binP, oldP, "PLASTIC", C_BLUE);
  updateGaugeInner(114, 62, 92, 122, binPa, oldPa, "PAPER", C_ORANGE);
  updateGaugeInner(212, 62, 92, 122, binM, oldM, "METAL", C_MAGENTA);
  updateEnv();
}

// ===================== SERVO MOTION =====================
static const int SERVO_HOME = 90;
static inline float smoothstep(float x) { return x * x * (3.0f - 2.0f * x); }

struct ServoMove
{
  bool active = false;
  int startAngle = SERVO_HOME;
  int targetAngle = SERVO_HOME;
  unsigned long t0 = 0;
  unsigned long dur = 800;
} sxMove;

void startServoMoveTo(int target, unsigned long durationMs)
{
  target = constrain(target, 0, 180);
  sxMove.active = true;
  sxMove.startAngle = sXoay.read();
  sxMove.targetAngle = target;
  sxMove.t0 = millis();
  sxMove.dur = max(250UL, durationMs);
}

bool servoMoveLoop()
{
  if (!sxMove.active)
    return true;

  unsigned long now = millis();
  unsigned long dt = now - sxMove.t0;
  float u = (sxMove.dur == 0) ? 1.0f : (float)dt / (float)sxMove.dur;

  if (u >= 1.0f)
  {
    sXoay.write(sxMove.targetAngle);
    sxMove.active = false;
    return true;
  }

  float e = smoothstep(u);
  float a = sxMove.startAngle + (sxMove.targetAngle - sxMove.startAngle) * e;
  sXoay.write((int)roundf(a));
  return false;
}

// ===================== PROCESS (NON-BLOCKING) =====================
enum ProcState
{
  PROC_IDLE,
  PROC_SHOW,
  PROC_ROTATE,
  PROC_OPEN,
  PROC_WAIT,
  PROC_CLOSE,
  PROC_RETURN_HOME,
  PROC_BACK_DASH
};
ProcState procState = PROC_IDLE;

String procType = "";
int procTargetAngle = SERVO_HOME;
uint16_t procColor = C_CYAN;
unsigned long procT0 = 0;

void drawProcessPage(const String &type, uint16_t color)
{
  setPage(PAGE_PROCESS);
  tft.fillScreen(C_BG);
  tft.drawRoundRect(10, 12, 300, 216, 18, color);

  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(30, 40);
  tft.print("PROCESSING...");

  tft.setTextSize(4);
  tft.setTextColor(color);
  tft.setCursor(30, 92);
  tft.print(type);

  tft.setTextSize(1);
  tft.setTextColor(C_GRAY);
  tft.setCursor(30, 170);
  tft.print("Dang xu ly - vui long doi");
}

void startProcess(const String &type, int angle, uint16_t color)
{
  if (!systemReady)
    return;
  if (procState != PROC_IDLE)
    return;

  procType = type;
  procTargetAngle = constrain(angle, 0, 180);
  procColor = color;

  speakEN(type + " detected.");

  drawProcessPage(procType, procColor);

  procState = PROC_SHOW;
  procT0 = millis();
}

void processLoop()
{
  switch (procState)
  {
  case PROC_IDLE:
    return;

  case PROC_SHOW:
    if (millis() - procT0 >= 220)
    {
      startServoMoveTo(procTargetAngle, 900);
      procState = PROC_ROTATE;
    }
    break;

  case PROC_ROTATE:
    if (servoMoveLoop())
      procState = PROC_OPEN;
    break;

  case PROC_OPEN:
    sCua.write(90);
    procT0 = millis();
    procState = PROC_WAIT;
    break;

  case PROC_WAIT:
    if (millis() - procT0 >= 4500)
      procState = PROC_CLOSE;
    break;

  case PROC_CLOSE:
    sCua.write(180);
    startServoMoveTo(SERVO_HOME, 900);
    procState = PROC_RETURN_HOME;
    break;

  case PROC_RETURN_HOME:
    if (servoMoveLoop())
    {
      procT0 = millis();
      procState = PROC_BACK_DASH;
    }
    break;

  case PROC_BACK_DASH:
    if (millis() - procT0 >= 150)
    {
      procState = PROC_IDLE;
      drawDashFrame();
    }
    break;
  }
}

// ===================== BIN FULL LOGIC =====================
bool isFullByPercent(int pct) { return pct >= FULL_THRESHOLD; }

bool checkAndHandleFull(const String &type)
{
  int pct = 0;
  bool *lockPtr = nullptr;
  String binName;

  if (type == "PLASTIC")
  {
    pct = quickReadBinPercent(ECHO_P);
    lockPtr = &binFullLock_P;
    binName = "PLASTIC";
  }
  else if (type == "PAPER")
  {
    pct = quickReadBinPercent(ECHO_PA);
    lockPtr = &binFullLock_PA;
    binName = "PAPER";
  }
  else if (type == "METAL")
  {
    pct = quickReadBinPercent(ECHO_M);
    lockPtr = &binFullLock_M;
    binName = "METAL";
  }
  else return false;

  bool full = isFullByPercent(pct);

  // FULL lần đầu → báo
  if (full && !(*lockPtr))
  {
    *lockPtr = true;

    drawFullBinPage(binName, pct);
    speakEN("Sorry. The " + binName + " bin is full.");
    fullUntil = millis() + FULL_SHOW_MS;
    return true;
  }

  // khi rỗng lại → mở khóa
  if (!full)
  {
    *lockPtr = false;
  }

  return full;
}

bool buzzerOn = false;
unsigned long buzzerT0 = 0;

void buzzerWrite(bool on)
{
#if BUZZER_ACTIVE_HIGH
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
#else
  digitalWrite(BUZZER_PIN, on ? LOW : HIGH);
#endif
}

void buzzerLoop(bool alarm)
{
  if (!alarm)
  {
    buzzerWrite(false);
    return;
  }

  unsigned long now = millis();
  if (!buzzerOn && now - buzzerT0 > 400)
  {
    buzzerOn = true;
    buzzerT0 = now;
    buzzerWrite(true);
  }
  else if (buzzerOn && now - buzzerT0 > 150)
  {
    buzzerOn = false;
    buzzerT0 = now;
    buzzerWrite(false);
  }
}

// ===================== OPTIMIZED OBJECT DETECTION =====================
bool isObjectPresent()
{
  long duration = readUltrasonicRawUS(ECHO_DETECT);
  int distance = durationToDistanceCM(duration);
  
  // Simplified detection range: 2-6cm
  return (distance >= 2 && distance <= 6);
}

// === OPTIMIZED DETECTION ALGORITHM ===
static void sendCaptureToAI() {
  IPAddress ip;
  ip.fromString(serverIP);   // 192.168.137.1

  Serial.println(">>> SEND CAPTURE");

  udp.beginPacket(ip, 5005);
  udp.print("CAPTURE");
  udp.endPacket();
}

// ===================== SETUP =====================
void setup()
{
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(ECHO_DETECT, INPUT);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(ECHO_P, INPUT);
  pinMode(ECHO_PA, INPUT);
  pinMode(ECHO_M, INPUT);

  tft.init(240, 320);
  tft.setRotation(1);

  drawBoot();

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);

  sXoay.attach(14);
  sCua.attach(20);
  sXoay.write(SERVO_HOME);
  sCua.write(180);

  pinMode(BUZZER_PIN, OUTPUT);
  buzzerWrite(false);

  dht.begin();
  gasSetup();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(500);

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");

  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED && retryCount < 100)
  {
    delay(200);
    Serial.print(".");
    feedAudioOnce();
    retryCount++;
  }

  WiFi.setSleep(false);
  wifiReady = true;
  udp.begin(localPort);

  drawWelcomePage("He thong khoi dong");
  speakEN("System starting");
  waitWithAudio(WELCOME_SHOW_MS);

  drawWaitAI();

  server.on("/status", handleStatus);
  server.begin();
}

// ===================== HTTP STATUS HANDLER =====================
void handleStatus()
{
  String json = "{";
  json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"page\":" + String(currentPage) + ",";
  json += "\"plastic\":" + String(binP) + ",";
  json += "\"paper\":" + String(binPa) + ",";
  json += "\"metal\":" + String(binM) + ",";
  json += "\"temperature\":" + String(tFilt, 1) + ",";
  json += "\"humidity\":" + String(hFilt, 1) + ",";

  json += "\"gas_ready\":" + String(gasCalibrated ? 1 : 0) + ",";
  json += "\"gas_level\":\"" + String(gasCalibrated ? gasLevelText(gasLevel) : "CAL") + "\",";

  json += "\"gas_raw\":" + String(gasSmoothed) + ",";
  json += "\"gas_base\":" + String(gasBaseline) + ",";

  json += "\"alarm\":" + String(
                             ((!isnan(tFilt) && tFilt >= TEMP_ALARM_C) ||
                              (gasAlarmActive())));
  json += "}";

  server.send(200, "application/json", json);
}

// ===================== SEND DETECT TO SERVER =====================
void sendDetectToServer(const String &type)
{
  if (WiFi.status() != WL_CONNECTED)
    return;

  HTTPClient http;
  String url = "http://" + serverIP + ":5000/api/detect";
  http.setTimeout(1500);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  String payload = "{\"type\":\"" + type + "\"}";
  int httpCode = http.POST(payload);
  http.end();
}

// ===================== OPTIMIZED LOOP =====================
void loop()
{
  audio.loop();
  ttsLoop();
  gasLoopUpdate();
  server.handleClient();
  processLoop();

  // Alarm buzzer
  bool alarm = false;
  if (!isnan(tFilt) && tFilt >= TEMP_ALARM_C)
    alarm = true;
  if (gasAlarmActive())
    alarm = true;
  buzzerLoop(alarm);

  // FULL PAGE timeout -> về DASH
  if (systemReady && currentPage == PAGE_FULL && (long)(millis() - fullUntil) >= 0)
  {
    drawDashFrame();
  }

  // ===== 1) UDP: AI LINK + DIRECT WASTE PROCESSING =====
  int pkt = udp.parsePacket();
  if (pkt)
  {
    int len = udp.read(packetBuffer, 255);
    if (len < 0) len = 0;
    if (len > 254) len = 254;
    packetBuffer[len] = 0;

    // Save AI IP
    aiIP = udp.remoteIP();
    aiIPValid = true;
    serverIP = aiIP.toString();
    
    String cmd = normalizeCmd(String(packetBuffer));

    // Link AI (first packet only)
    if (!aiLinked)
    {
      aiLinked = true;
      systemReady = wifiReady && aiLinked;

      drawWelcomePage("AI da ket noi");
      speakEN("AI system connected.");
      waitWithAudio(WELCOME_SHOW_MS);
      drawDashFrame();
      lastWelcomeSpeech = millis();
    }

    // === OPTIMIZED: DIRECT WASTE PROCESSING ===
    // If we receive a valid waste command, go straight to processing
    if (systemReady && procState == PROC_IDLE && isValidWasteCmd(cmd))
    {
      // Check if bin is full first
      if (checkAndHandleFull(cmd))
      {
        // Bin is full, show full page
      }
      else
      {
        // Bin not full, go directly to processing
        if (cmd == "PLASTIC")
        {
          sendDetectToServer("plastic");
          startProcess("PLASTIC", 0, C_BLUE);
        }
        else if (cmd == "PAPER")
        {
          sendDetectToServer("paper");
          startProcess("PAPER", 115, C_ORANGE);
        }
        else if (cmd == "METAL")
        {
          sendDetectToServer("metal");
          startProcess("METAL", 190, C_MAGENTA);
        }
      }
    }
  }

  // ===== 2) SIMPLIFIED OBJECT DETECTION =====
  // Only check for object presence and send CAPTURE
  static bool lastObjectState = false;
  bool objectPresent = isObjectPresent();
  
  // Object just appeared - send CAPTURE immediately
  if (objectPresent && !lastObjectState && systemReady && procState == PROC_IDLE)
  {
    sendCaptureToAI();
    drawWelcomePage("Dang phan tich...");
    speakEN("Analyzing object.");
  }
  
  lastObjectState = objectPresent;

  // Welcome page timeout
  if (systemReady && currentPage == PAGE_WELCOME && (long)(millis() - welcomeUntil) >= 0)
  {
    drawDashFrame();
  }

  // Update dashboard sensors
  static unsigned long lastUp = 0;
  if (systemReady && procState == PROC_IDLE && currentPage == PAGE_DASH && millis() - lastUp > 850)
  {
    if (!audio.isRunning())
    {
      binP = filteredBinPercent(ECHO_P, fP);
      binPa = filteredBinPercent(ECHO_PA, fPa);
      binM = filteredBinPercent(ECHO_M, fM);
    }
    lastUp = millis();
  }

  // Update dashboard display
  static unsigned long lastDraw = 0;
  if (systemReady && currentPage == PAGE_DASH && millis() - lastDraw > 100)
  {
    dashboardLoopDraw();
    lastDraw = millis();
  }

  // Temperature alarm
  unsigned long now = millis();
  if (!isnan(tFilt) && tFilt >= TEMP_ALARM_C)
  {
    alarm = true;
    if (now - lastTempAlertMs > TEMP_ALERT_INTERVAL)
    {
      speakEN("Warning. System temperature is too high. Please check the power supply.");
      lastTempAlertMs = now;
    }
  }

  if (gasAlarmActive())
    alarm = true;

  buzzerLoop(alarm);
}