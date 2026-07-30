#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <time.h>

#include "LGFX.h"
#include "WiFiManagerHelpers.h"
#include "ConfigurationWebServer.h"
#include "HttpRequestManager.h"
#include "OpenSkyAuthTokenHandler.h"
#include "OpenSkyAircraftDataSource.h"
#include "LocalReceiverAircraftDataSource.h"
#include "AircraftManager.h"
#include "DrawHelpers.h"
#include "models/Aircraft.h"
#include "models/TrackedAircraft.h"
#include <ESP32Encoder.h>
#include <Adafruit_NeoPixel.h>
#include <memory>

#define ENCODER_A 9
#define ENCODER_B 8
#define ENCODER_SW 7

#define RGB_LED_PIN 48

Adafruit_NeoPixel statusLed(
    1,
    RGB_LED_PIN,
    NEO_GRB + NEO_KHZ800);

constexpr int SCREEN_SIZE = 240;
constexpr int SCREEN_SIZE_DIV_2 = (SCREEN_SIZE / 2);

LGFX tft;
LGFX_Sprite backbuffer(&tft);

WiFiManager wm;
ConfigurationWebServer configServer;
HttpRequestManager http;
OpenSkyAuthTokenHandler authHandler(http);
OpenSkyAircraftDataSource dataSource(configServer, authHandler, http);
LocalReceiverAircraftDataSource localDataSource(configServer, http);

ESP32Encoder encoder;
int64_t lastEncoderPos = 0;

std::unique_ptr<AircraftManager> aircraftManager;
static bool setupComplete = false;
static bool sleepScheduleEnabled = false;
static bool wakeOnKnobPress = true;
static int sleepStartMinutes = 22 * 60;
static int sleepEndMinutes = 7 * 60;
static int timezoneOffsetMinutes = 0;
static unsigned long wakeUntilMs = 0;
static constexpr unsigned long WAKE_DURATION_MS = 30000;

static bool ParseClockMinutes(const String &value, int &minutes)
{
  const int colon = value.indexOf(':');
  if (colon <= 0 || colon >= (int)value.length() - 1)
    return false;

  const int hour = value.substring(0, colon).toInt();
  const int minute = value.substring(colon + 1).toInt();
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
    return false;

  minutes = hour * 60 + minute;
  return true;
}

static String FormatClockMinutes(int minutes)
{
  minutes %= (24 * 60);
  if (minutes < 0)
    minutes += 24 * 60;

  const int hour = minutes / 60;
  const int minute = minutes % 60;
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);
  return String(buffer);
}

static bool IsClockWithinSleepWindow(int currentMinutes)
{
  if (!sleepScheduleEnabled)
    return false;

  if (currentMinutes < 0)
    return false;

  if (sleepStartMinutes == sleepEndMinutes)
    return false;

  if (sleepStartMinutes < sleepEndMinutes)
    return currentMinutes >= sleepStartMinutes && currentMinutes < sleepEndMinutes;

  return currentMinutes >= sleepStartMinutes || currentMinutes < sleepEndMinutes;
}

static bool IsDisplaySleeping()
{
  if (wakeUntilMs != 0 && millis() < wakeUntilMs)
    return false;

  struct tm now;
  if (!getLocalTime(&now, 25))
    return false;

  const int currentMinutes = now.tm_hour * 60 + now.tm_min;
  return IsClockWithinSleepWindow(currentMinutes);
}

static void WakeDisplay()
{
  wakeUntilMs = millis() + WAKE_DURATION_MS;
}

static bool IsProvisioned()
{
  return !configServer.GetStoredString("receiver-ip").isEmpty() ||
         !configServer.GetStoredString("receiver-url").isEmpty();
}

static void LoadSleepScheduleFromPrefs()
{
  sleepScheduleEnabled = configServer.GetStoredString("sleep-enabled") == "true";
  wakeOnKnobPress = configServer.GetStoredString("sleep-wake-knob").isEmpty()
                        ? true
                        : configServer.GetStoredString("sleep-wake-knob") == "true";

  const String startValue = configServer.GetStoredString("sleep-start");
  const String endValue = configServer.GetStoredString("sleep-end");
  int parsedMinutes = 22 * 60;
  sleepStartMinutes = ParseClockMinutes(startValue, parsedMinutes) ? parsedMinutes : (22 * 60);
  parsedMinutes = 7 * 60;
  sleepEndMinutes = ParseClockMinutes(endValue, parsedMinutes) ? parsedMinutes : (7 * 60);

  timezoneOffsetMinutes = configServer.GetStoredString("sleep-timezone-offset").toInt();
  configTime(-(timezoneOffsetMinutes * 60), 0, "pool.ntp.org", "time.nist.gov");
}

static String BuildSetupUrl()
{
  IPAddress ip = WiFi.localIP();
  return "http://" + ip.toString() + ":8080/";
}

static void DrawSetupScreen(LGFX_Sprite &buf)
{
  const String setupUrl = BuildSetupUrl();

  buf.fillScreen(lgfx::color888(0, 0, 0));
  buf.qrcode(setupUrl, 24, 24, 192, 6);
}

static void DrawConnectivityWarning(LGFX_Sprite &buf, const String &message)
{
  buf.fillScreen(lgfx::color888(0, 0, 0));
  buf.setTextDatum(textdatum_t::middle_center);
  buf.setTextSize(1);

  buf.setTextColor(lgfx::color888(255, 120, 0));
  buf.drawCentreString("CONNECTION WARNING", SCREEN_SIZE / 2, SCREEN_SIZE / 2 - 28);

  buf.setTextColor(lgfx::color888(255, 0, 0));
  buf.drawCentreString(message, SCREEN_SIZE / 2, SCREEN_SIZE / 2);

  buf.setTextColor(lgfx::color888(255, 120, 0));
  buf.drawCentreString("Retrying automatically", SCREEN_SIZE / 2, SCREEN_SIZE / 2 + 28);
}

static bool ClearWifiIfBootButtonHeld()
{
  constexpr unsigned long HOLD_DURATION_MS = 10000;

  if (digitalRead(ENCODER_SW) != LOW)
    return false;

  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.setTextColor(lgfx::color888(0, 255, 0));
  tft.drawCentreString("- SETUP -", SCREEN_SIZE / 2, SCREEN_SIZE / 2 - 20);
  tft.drawCentreString("Hold button 10s to clear Wi-Fi", SCREEN_SIZE / 2, SCREEN_SIZE / 2);
  tft.drawCentreString("Release to cancel", SCREEN_SIZE / 2, SCREEN_SIZE / 2 + 20);

  const unsigned long holdStart = millis();
  while (millis() - holdStart < HOLD_DURATION_MS)
  {
    if (digitalRead(ENCODER_SW) != LOW)
      return false;

    delay(10);
  }

  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.setTextColor(lgfx::color888(0, 255, 0));
  tft.drawCentreString("Clearing Wi-Fi...", SCREEN_SIZE / 2, SCREEN_SIZE / 2);

  wm.resetSettings();
  WiFi.disconnect(true, true);
  delay(500);
  ESP.restart();
  return true;
}

static bool ProcessEncoderInput()
{
  int64_t pos = encoder.getCount();
  static bool lastButtonState = HIGH;
  bool currentButtonState = digitalRead(ENCODER_SW);

  if (IsDisplaySleeping())
  {
    if (wakeOnKnobPress &&
        lastButtonState == HIGH &&
        currentButtonState == LOW)
    {
      WakeDisplay();
      lastButtonState = currentButtonState;
      lastEncoderPos = pos;
      return true;
    }

    lastButtonState = currentButtonState;
    lastEncoderPos = pos;
    return false;
  }

  if (pos != lastEncoderPos)
  {
    if (pos > lastEncoderPos)
    {
      aircraftManager->SelectNextAircraft();
    }
    else
    {
      aircraftManager->SelectPreviousAircraft();
    }

    WakeDisplay();
    lastEncoderPos = pos;
  }

  if (lastButtonState == HIGH &&
      currentButtonState == LOW)
  {
    aircraftManager->EncoderClick();
    WakeDisplay();
  }

  lastButtonState = currentButtonState;
  return false;
}

void SetLed(uint8_t r, uint8_t g, uint8_t b)
{
  statusLed.setPixelColor(
      0,
      statusLed.Color(r, g, b));

  statusLed.show();
}

void setup()
{
  Serial.begin(115200);
  // delay(1000); // avoids immediate serial output being cut off - uncomment if needed

  statusLed.begin();
  statusLed.setBrightness(200);
  statusLed.clear();
  statusLed.show();

  SetLed(0, 0, 255); // Booting

  // initialise LGFX + screen
  tft.init();
  tft.invertDisplay(true);
  tft.setRotation(2);
  // pinMode(3, OUTPUT);
  // digitalWrite(3, HIGH);

  backbuffer.setColorDepth(8);
  backbuffer.createSprite(SCREEN_SIZE, SCREEN_SIZE);

  pinMode(ENCODER_SW, INPUT_PULLUP);
  if (ClearWifiIfBootButtonHeld())
    return;

  // establish WiFi connection
  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.setTextColor(lgfx::color888(0, 255, 0));
  tft.drawCentreString("Connecting to WiFi...", SCREEN_SIZE / 2, SCREEN_SIZE / 2);

  SetLed(255, 255, 0); // WiFi connecting

  WiFiManagerHelpers::ConfigureWiFiManager(wm, tft);
  wm.setConfigPortalBlocking(false);

  const bool wifiConnected = wm.autoConnect(WiFiManagerHelpers::WiFiManagerName);

  // WiFiManager starts the provisioning AP asynchronously in this mode.
  // Give the portal a moment to actually come up before we move on so the
  // setup SSID has time to appear on client devices.
  if (!wifiConnected)
  {
    const unsigned long portalReadyStart = millis();
    while (!wm.getConfigPortalActive() && (millis() - portalReadyStart) < 3000)
      delay(10);
  }

  if (!wifiConnected && wm.getConfigPortalActive())
  {
    const unsigned long portalStart = millis();
    while (wm.getConfigPortalActive() && WiFi.status() != WL_CONNECTED && (millis() - portalStart) < 120000)
    {
      wm.process();
      delay(10);
    }
  }

  // WiFiManager can briefly leave AP+STA mode after provisioning.
  // Force station-only mode and wait for the DHCP address to settle before
  // starting the normal web server.
  WiFi.mode(WIFI_STA);
  const unsigned long wifiReadyStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiReadyStart) < 10000)
  {
    delay(100);
  }
  delay(250);

  // Keep the settle time short so setup feels responsive.
  // The config server now runs on 8080, so we do not need a long port-release wait.
  delay(250);

  // begin background server for configuration
  configServer.Initialise();
  setupComplete = IsProvisioned();
  if (setupComplete)
    LoadSleepScheduleFromPrefs();

  SetLed(0, 255, 0); // Running

  if (!setupComplete)
  {
    Serial.println("[SETUP] Receiver not configured yet; showing setup QR screen.");
    DrawSetupScreen(backbuffer);
    backbuffer.pushSprite(0, 0);
    return;
  }

  const String receiverIp = configServer.GetStoredString("receiver-ip");
  const String receiverUrl = configServer.GetStoredString("receiver-url");
  AircraftDataSource* activeDataSource =
      (receiverIp.isEmpty() && receiverUrl.isEmpty())
          ? static_cast<AircraftDataSource*>(&dataSource)
          : static_cast<AircraftDataSource*>(&localDataSource);

  aircraftManager = std::make_unique<AircraftManager>(configServer, *activeDataSource, tft);

  // initialise aircraft manager
  aircraftManager->Initialise();

  ESP32Encoder::useInternalWeakPullResistors = puType::up;

  encoder.attachSingleEdge(ENCODER_A, ENCODER_B);
  encoder.setCount(0);

  pinMode(ENCODER_SW, INPUT_PULLUP);
}

void loop()
{
  if (!setupComplete)
  {
    static bool setupScreenDrawn = false;
    if (!setupScreenDrawn)
    {
      DrawSetupScreen(backbuffer);
      backbuffer.pushSprite(0, 0);
      setupScreenDrawn = true;
    }

    delay(100);
    return;
  }

  ProcessEncoderInput();
  aircraftManager->Update();

  if (IsDisplaySleeping())
  {
    backbuffer.fillScreen(lgfx::color888(0, 0, 0));
    backbuffer.pushSprite(0, 0);
    SetLed(0, 0, 0);
    aircraftManager->Update();
    delay(100);
    return;
  }

  const String connectivityWarning = aircraftManager->GetConnectivityWarningMessage();

  // draw cycle
  backbuffer.fillScreen(lgfx::color888(0, 0, 0));

  if (!connectivityWarning.isEmpty())
  {
    DrawConnectivityWarning(backbuffer, connectivityWarning);
    backbuffer.pushSprite(0, 0);
    SetLed(255, 80, 0);
    return;
  }

  String renderScanlines = configServer.GetStoredString("scanline");
  if (renderScanlines.isEmpty() || renderScanlines == "true")
  {
    DrawScanLines(backbuffer,
                  SCREEN_SIZE_DIV_2 - 1,
                  SCREEN_SIZE_DIV_2 - 1,
                  SCREEN_SIZE_DIV_2 - 1 + (std::cos(millis() / 3000.0f) * SCREEN_SIZE_DIV_2),
                  SCREEN_SIZE_DIV_2 - 1 + (std::sin(millis() / 3000.0f) * SCREEN_SIZE_DIV_2),
                  20, 128, 5);
  }

  aircraftManager->Draw(backbuffer);
  backbuffer.pushSprite(0, 0);

  SetLed(0, 255, 0); // Running

  ProcessEncoderInput();
}
