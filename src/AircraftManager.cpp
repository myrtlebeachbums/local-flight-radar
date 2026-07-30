#include "AircraftManager.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <WiFi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

constexpr int SCREEN_SIZE = 240;
constexpr int SCREEN_SIZE_DIV_2 = (SCREEN_SIZE / 2);

#include <ArduinoJson.h>

enum ScreenMode
{
    SCREEN_RADAR,
    SCREEN_DETAILS
};

ScreenMode currentScreen = SCREEN_RADAR;

int selectedAircraftIndex = 0;
std::vector<TrackedAircraft *> visibleAircraft;

static const uint32_t AircraftColours[] =
    {
        lgfx::color888(0, 255, 128),   // Radar green
        lgfx::color888(0, 220, 255),   // Cyan
        lgfx::color888(0, 128, 255),   // Electric blue
        lgfx::color888(100, 200, 255), // Sky blue
        lgfx::color888(160, 130, 255), // Soft violet
        lgfx::color888(220, 80, 255),  // Magenta
        lgfx::color888(255, 50, 150),  // Hot pink
        lgfx::color888(255, 60, 60),   // Coral red
        lgfx::color888(255, 120, 0),   // Burnt orange
        lgfx::color888(255, 190, 0),   // Amber
        lgfx::color888(255, 235, 0),   // Neon yellow
        lgfx::color888(180, 255, 0),   // Acid lime
        lgfx::color888(80, 255, 180),  // Mint
        lgfx::color888(0, 200, 180),   // Teal
        lgfx::color888(50, 180, 255),  // Cornflower
        lgfx::color888(255, 160, 100), // Peach
        lgfx::color888(255, 210, 160), // Warm cream
        lgfx::color888(140, 255, 140), // Pale green
        lgfx::color888(200, 160, 255), // Lavender
        lgfx::color888(255, 120, 200)  // Pink
};

void AircraftManager::FetchTaskEntry(void *parameter)
{
    auto *self = static_cast<AircraftManager *>(parameter);
    self->FetchLoop();
}

void AircraftManager::ServiceConnectivityWatchdog()
{
    const unsigned long now = millis();
    if (lastSuccessfulFetchMs == 0)
        return;

    if (now - lastSuccessfulFetchMs < 5UL * 60UL * 1000UL)
        return;

    if (now - lastWatchdogRecoveryAttemptMs < 5UL * 60UL * 1000UL)
        return;

    lastWatchdogRecoveryAttemptMs = now;

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[WARN] Wi-Fi appears down; attempting reconnect");
        WiFi.reconnect();
        return;
    }

    Serial.println("[WARN] Local ADSB feed appears stale; reinitialising data source");
    dataSource.Initialise();
    fetchInterval = dataSource.GetFetchIntervalMs();
}

void AircraftManager::FetchLoop()
{
    for (;;)
    {
        ServiceConnectivityWatchdog();

        const TickType_t delayTicks = pdMS_TO_TICKS(fetchInterval == 0 ? 1000 : fetchInterval);

        if (WiFi.status() != WL_CONNECTED)
        {
            vTaskDelay(delayTicks);
            continue;
        }

        std::vector<Aircraft> aircraft;
        if (dataSource.Fetch(aircraft))
        {
            lastSuccessfulFetchMs = millis();
            if (pendingAircraftMutex != nullptr &&
                xSemaphoreTake(pendingAircraftMutex, portMAX_DELAY) == pdTRUE)
            {
                pendingAircraft = std::move(aircraft);
                pendingAircraftReady = true;
                xSemaphoreGive(pendingAircraftMutex);
            }
        }

        vTaskDelay(delayTicks);
    }
}

String AircraftManager::GetConnectivityWarningMessage() const
{
    const unsigned long now = millis();

    if (lastSuccessfulFetchMs != 0 && now - lastSuccessfulFetchMs < 5UL * 60UL * 1000UL)
        return "";

    if (WiFi.status() != WL_CONNECTED)
        return "Unable to connect to Wi-Fi";

    return "Unable to connect to local ADSB feed";
}

bool AircraftManager::TryConsumePendingAircraft(std::vector<Aircraft> &aircraft)
{
    if (pendingAircraftMutex == nullptr)
        return false;

    if (xSemaphoreTake(pendingAircraftMutex, 0) != pdTRUE)
        return false;

    const bool hasUpdate = pendingAircraftReady;
    if (hasUpdate)
    {
        aircraft = std::move(pendingAircraft);
        pendingAircraft.clear();
        pendingAircraftReady = false;
    }

    xSemaphoreGive(pendingAircraftMutex);
    return hasUpdate;
}

void AircraftManager::LoadDetailFields()
{
    detailFields.clear();

    for (size_t i = 0; i < AIRCRAFT_DETAIL_FIELD_COUNT; ++i)
    {
        const auto &spec = GetAircraftDetailFieldSpec(i);
        const String storedValue = configServer.GetStoredString(spec.key);
        const bool enabled = storedValue.isEmpty() ? spec.defaultEnabled : storedValue == "true";

        if (!enabled)
            continue;

        if (detailFields.size() >= MAX_SELECTED_DETAIL_FIELDS)
            continue;

        detailFields.push_back(spec.field);
    }

    if (!detailFields.empty())
        return;

    for (size_t i = 0; i < AIRCRAFT_DETAIL_FIELD_COUNT && detailFields.size() < MAX_SELECTED_DETAIL_FIELDS; ++i)
    {
        const auto &spec = GetAircraftDetailFieldSpec(i);
        if (spec.defaultEnabled)
            detailFields.push_back(spec.field);
    }
}

String AircraftManager::FormatDetailValue(const TrackedAircraft &tracked, AircraftDetailField field) const
{
    switch (field)
    {
    case AircraftDetailField::Altitude:
        return String((int)MetersToDisplayAltitude(unitSystem, tracked.state.baroAltitude)) + " " + AltitudeUnitLabel(unitSystem);
    case AircraftDetailField::Speed:
        return String((int)MpsToDisplaySpeed(unitSystem, tracked.state.velocity)) + " " + SpeedUnitLabel(unitSystem);
    case AircraftDetailField::Heading:
        return String((int)tracked.state.trueTrack) + " deg";
    case AircraftDetailField::Distance:
    {
        if (lat == 0.0 && lon == 0.0)
            return "--";

        constexpr float MILES_PER_DEG_LAT = 69.172f;
        const float milesPerDegLon = MILES_PER_DEG_LAT * std::cos(radians(lat));
        const float dLon = (tracked.state.longitude - lon) * milesPerDegLon;
        const float dLat = (tracked.state.latitude - lat) * MILES_PER_DEG_LAT;
        const float distanceMiles = std::sqrt(dLon * dLon + dLat * dLat);
        return String(MilesToDisplayDistance(unitSystem, distanceMiles), 1) + " " + DistanceUnitLabel(unitSystem);
    }
    case AircraftDetailField::Squawk:
        return tracked.state.squawk.isEmpty() ? String("--") : tracked.state.squawk;
    case AircraftDetailField::Icao24:
        return tracked.state.icao24.isEmpty() ? String("--") : tracked.state.icao24;
    case AircraftDetailField::MessageAge:
    {
        const unsigned long ageMs = millis() - tracked.lastSeen;
        return String(ageMs / 1000.0f, 1) + " s";
    }
    }

    return "--";
}

void AircraftManager::Initialise()
{
    dataSource.Initialise();

    // get receiver point + display distance
    lat = configServer.GetStoredString("receiver-lat").toDouble();
    lon = configServer.GetStoredString("receiver-lon").toDouble();
    if (lat == 0.0 && lon == 0.0)
    {
        lat = configServer.GetStoredString("latitude").toDouble();
        lon = configServer.GetStoredString("longitude").toDouble();
    }

    rad = configServer.GetStoredString("radius").toDouble();
    if (rad <= 0.0)
        rad = 50.0;

    // configuration
    const String renderText = configServer.GetStoredString("infotext");
    const String renderTris = configServer.GetStoredString("triangle");
    const String renderIncomplete = configServer.GetStoredString("show-incomplete-data");
    if (!renderText.isEmpty())
        displayInfoText = renderText == "true" ? true : false;
    if (!renderTris.isEmpty())
        displayTriangles = renderTris == "true" ? true : false;
    if (!renderIncomplete.isEmpty())
        displayIncompleteAircraft = renderIncomplete == "true" ? true : false;

    unitSystem = ParseUnitSystem(configServer.GetStoredString("units"));
    LoadDetailFields();

    fetchInterval = dataSource.GetFetchIntervalMs();
    lastSuccessfulFetchMs = millis();
    lastWatchdogRecoveryAttemptMs = lastSuccessfulFetchMs;

    if (pendingAircraftMutex == nullptr)
        pendingAircraftMutex = xSemaphoreCreateMutex();

    if (pendingAircraftMutex != nullptr && fetchTaskHandle == nullptr)
    {
        BaseType_t taskStarted = xTaskCreatePinnedToCore(
            AircraftManager::FetchTaskEntry,
            "aircraftFetch",
            8192,
            this,
            1,
            &fetchTaskHandle,
            0);

        if (taskStarted != pdPASS)
        {
            fetchTaskHandle = nullptr;
            Serial.println("[WARN] Unable to start aircraft fetch task");
        }
    }
    else if (pendingAircraftMutex == nullptr)
    {
        Serial.println("[WARN] Unable to create aircraft fetch mutex");
    }
}

void AircraftManager::Update()
{
    if (fetchTaskHandle == nullptr)
        ServiceConnectivityWatchdog();

    std::vector<Aircraft> aircraft;

    if (fetchTaskHandle != nullptr)
    {
        if (!TryConsumePendingAircraft(aircraft))
            return;
    }
    else
    {
        unsigned long now = millis();

        // fetch cycle fallback if the background task could not start
        if (now - lastFetch < fetchInterval)
            return;

        lastFetch = now;

        if (!dataSource.Fetch(aircraft))
            return;
        lastSuccessfulFetchMs = millis();
    }

    if (aircraft.empty())
        return;

    unsigned long now = millis();

    for (auto &ac : aircraft)
    {
        auto it = trackedAircraft.find(ac.icao24);
        if (it == trackedAircraft.end())
            trackedAircraft.emplace(ac.icao24, TrackedAircraft{ac, now});
        else
            it->second.Update(ac, now);
    }

    // remove any planes that disappeared from the feed
    for (auto it = trackedAircraft.begin(); it != trackedAircraft.end();)
    {
        bool aircraftPresent = std::any_of(aircraft.begin(), aircraft.end(), [&](const Aircraft &ac)
                                           { return ac.icao24 == it->first; });
        if (!aircraftPresent)
            it = trackedAircraft.erase(it);
        else
            ++it;
    }
}

void AircraftManager::DrawDetails(LGFX_Sprite &backbuffer)
{
    backbuffer.fillScreen(TFT_BLACK);

    constexpr int CENTRE = SCREEN_SIZE_DIV_2 - 1;
    constexpr int OUTER = SCREEN_SIZE_DIV_2 - 5;

    // Outer circular frame
    backbuffer.drawCircle(
        CENTRE,
        CENTRE,
        OUTER,
        lgfx::color888(0, 200, 0));

    if (visibleAircraft.empty())
    {
        backbuffer.setTextColor(lgfx::color888(0, 255, 0));
        backbuffer.setTextDatum(textdatum_t::middle_center);
        backbuffer.drawString("NO AIRCRAFT", CENTRE, CENTRE);
        return;
    }

    TrackedAircraft &tracked =
        *visibleAircraft[selectedAircraftIndex];

    // Aircraft icon
    DrawAircraftTriangle(
        backbuffer,
        CENTRE,
        45,
        tracked,
        false);

    backbuffer.setTextColor(lgfx::color888(0, 255, 0));
    backbuffer.setTextDatum(textdatum_t::middle_center);

    // Callsign
    String callsign = tracked.state.callsign;
    callsign.trim();
    if (callsign.isEmpty())
        callsign = "UNKNOWN";

    backbuffer.setTextSize(2);
    backbuffer.drawString(
        callsign,
        CENTRE,
        75);

    backbuffer.setTextSize(1);

    int y = 105;
    constexpr int LINE = 18;

    for (const auto field : detailFields)
    {
        const auto &spec = GetAircraftDetailFieldSpec(static_cast<size_t>(field));
        backbuffer.drawString(
            String(spec.label) + " " + FormatDetailValue(tracked, field),
            CENTRE,
            y);
        y += LINE;
    }

    y += 22;

    backbuffer.setTextColor(lgfx::color888(0, 100, 0));

    backbuffer.drawString(
        "< Rotate >",
        CENTRE,
        y);

    y += 18;

    backbuffer.drawString(
        "Click to return",
        CENTRE,
        y);
}

void AircraftManager::EncoderClick()
{
    currentScreen =
        (currentScreen == SCREEN_RADAR)
            ? SCREEN_DETAILS
            : SCREEN_RADAR;
}

void AircraftManager::Draw(LGFX_Sprite &backbuffer)
{
    visibleAircraft.clear();

    for (auto &[icao, tracked] : trackedAircraft)
    {
        if (tracked.state.onGround)
            continue;

        String callsign = tracked.state.callsign;
        callsign.trim();
        if (!displayIncompleteAircraft && callsign.isEmpty())
            continue;

        visibleAircraft.push_back(&tracked);
    }

    if (currentScreen == SCREEN_DETAILS)
    {
        DrawDetails(backbuffer);
        return;
    }

    DrawRadarCircles(backbuffer);

    visibleAircraft.clear();

    for (auto &[icao, tracked] : trackedAircraft)
    {
        if (tracked.state.onGround)
            continue;

        String callsign = tracked.state.callsign;
        callsign.trim();
        if (!displayIncompleteAircraft && callsign.isEmpty())
            continue;

        visibleAircraft.push_back(&tracked);
    }

    if (!visibleAircraft.empty())
    {
        selectedAircraftIndex =
            constrain(
                selectedAircraftIndex,
                0,
                (int)visibleAircraft.size() - 1);
    }
    else
    {
        selectedAircraftIndex = 0;
    }

    for (size_t i = 0; i < visibleAircraft.size(); i++)
    {
        TrackedAircraft &tracked = *visibleAircraft[i];

        tracked.Tick();

        auto [predLat, predLon] = tracked.GetDisplayPosition();
        auto [x, y] = ProjectCoordinateToScreen(predLat, predLon);

        bool selected =
            ((int)i == selectedAircraftIndex);

        if (displayInfoText)
            DrawAircraftInfo(backbuffer, x, y, tracked);

        if (displayTriangles)
            DrawAircraftTriangle(
                backbuffer,
                x,
                y,
                tracked,
                selected);
        else
        {
            backbuffer.fillCircle(
                x,
                y,
                selected ? 5 : 3,
                selected
                    ? lgfx::color888(0, 0, 255)
                    : lgfx::color888(0, 255, 0));
        }
    }
}

uint32_t AircraftManager::GetAircraftColour(
    const TrackedAircraft &tracked) const
{
    uint32_t hash = 0;

    for (char c : tracked.state.icao24)
        hash = hash * 31 + c;

    return AircraftColours[hash % (sizeof(AircraftColours) /
                                   sizeof(AircraftColours[0]))];
}

void AircraftManager::SelectNextAircraft()
{
    if (visibleAircraft.empty())
        return;

    selectedAircraftIndex++;

    if (selectedAircraftIndex >= visibleAircraft.size())
        selectedAircraftIndex = 0;

    Serial.print("Aircraft count: ");
    Serial.println(visibleAircraft.size());

    Serial.print("Selected index: ");
    Serial.println(selectedAircraftIndex);
}

void AircraftManager::SelectPreviousAircraft()
{
    if (visibleAircraft.empty())
        return;

    selectedAircraftIndex--;

    if (selectedAircraftIndex < 0)
        selectedAircraftIndex =
            visibleAircraft.size() - 1;

    Serial.print("Selected previous aircraft, index: ");
    Serial.println(selectedAircraftIndex);
}

void AircraftManager::DrawRadarCircles(LGFX_Sprite &backbuffer) const
{
    constexpr int CENTRE = SCREEN_SIZE_DIV_2 - 1;
    constexpr int OUTER = SCREEN_SIZE_DIV_2 - 5;

    backbuffer.drawCircle(CENTRE, CENTRE, OUTER, lgfx::color888(0, 200, 0));
    backbuffer.drawCircle(CENTRE, CENTRE, (OUTER / 3) * 2, lgfx::color888(0, 64, 0));
    backbuffer.drawCircle(CENTRE, CENTRE, OUTER / 3, lgfx::color888(0, 32, 0));
}

std::pair<int, int> AircraftManager::ProjectCoordinateToScreen(float predLat, float predLon) const
{
    constexpr float MILES_PER_DEG_LAT = 69.172f;
    const float milesPerDegLon = MILES_PER_DEG_LAT * std::cos(radians(lat));

    const float dLon = (predLon - lon) * milesPerDegLon;
    const float dLat = (predLat - lat) * MILES_PER_DEG_LAT;

    const float normLon = (dLon + rad) / (2.0f * rad);
    const float normLat = (dLat + rad) / (2.0f * rad);

    const int x = static_cast<int>(normLon * SCREEN_SIZE);
    const int y = static_cast<int>(SCREEN_SIZE - (normLat * SCREEN_SIZE));

    return {x, y};
}

void AircraftManager::DrawAircraftInfo(LGFX_Sprite &backbuffer, int x, int y, const TrackedAircraft &tracked) const
{
    // const int lineHeight = tft.fontHeight() + 1;

    backbuffer.setTextSize(1);
    backbuffer.setTextColor(lgfx::color888(0, 128, 0));
    backbuffer.drawString(tracked.state.callsign, x + 10, y + 15);
    // backbuffer.drawString(String(tracked.state.velocity) + "m/s", x + 5, y + 5 + lineHeight);
    // backbuffer.drawString(String(tracked.state.baroAltitude) + "m", x + 5, y + 5 + lineHeight * 2);
}

void AircraftManager::DrawAircraftTriangle(LGFX_Sprite &backbuffer, int x, int y, const TrackedAircraft &tracked) const
{
    const float dx = std::sin(radians(tracked.state.trueTrack));
    const float dy = -std::cos(radians(tracked.state.trueTrack));
    const float px = -dy;
    const float py = dx;

    constexpr float TRIANGLE_LENGTH = 6.0f;
    constexpr float TRIANGLE_WIDTH = 3.0f;

    const float tipX = x + dx * TRIANGLE_LENGTH;
    const float tipY = y + dy * TRIANGLE_LENGTH;
    const float leftX = x - dx * TRIANGLE_LENGTH * 0.5f + px * TRIANGLE_WIDTH * 0.5f;
    const float leftY = y - dy * TRIANGLE_LENGTH * 0.5f + py * TRIANGLE_WIDTH * 0.5f;
    const float rightX = x - dx * TRIANGLE_LENGTH * 0.5f - px * TRIANGLE_WIDTH * 0.5f;
    const float rightY = y - dy * TRIANGLE_LENGTH * 0.5f - py * TRIANGLE_WIDTH * 0.5f;

    backbuffer.fillTriangle(tipX, tipY, leftX, leftY, rightX, rightY, lgfx::color888(0, 255, 0));
}

void AircraftManager::DrawAircraftTriangle(
    LGFX_Sprite &backbuffer,
    int x,
    int y,
    const TrackedAircraft &tracked,
    bool selected) const
{
    const float dx = std::sin(radians(tracked.state.trueTrack));
    const float dy = -std::cos(radians(tracked.state.trueTrack));

    const float px = -dy;
    const float py = dx;

    const uint16_t colour =
        selected
            ? lgfx::color888(255, 255, 255) // White
            : GetAircraftColour(tracked);

    constexpr float BODY_FRONT = 8.0f;
    constexpr float BODY_REAR = 6.0f;

    constexpr float WING_SPAN = 8.0f;
    constexpr float WING_SWEEP = 4.0f;

    // Fuselage endpoints
    const float noseX = x + dx * BODY_FRONT;
    const float noseY = y + dy * BODY_FRONT;

    const float tailX = x - dx * BODY_REAR;
    const float tailY = y - dy * BODY_REAR;

    // Rounded fuselage (implemented as 3 parallel lines)
    for (int i = -1; i <= 1; i++)
    {
        backbuffer.drawLine(
            noseX + px * i,
            noseY + py * i,
            tailX + px * i,
            tailY + py * i,
            colour);
    }

    // Rounded ends
    backbuffer.fillCircle(noseX, noseY, 1, colour);
    backbuffer.fillCircle(tailX, tailY, 1, colour);

    // Wing triangle
    const float wingCenterX = x + dx * 2.5f;
    const float wingCenterY = y + dy * 2.5f;

    const float leftWingX =
        wingCenterX + px * WING_SPAN - dx * WING_SWEEP;

    const float leftWingY =
        wingCenterY + py * WING_SPAN - dy * WING_SWEEP;

    const float rightWingX =
        wingCenterX - px * WING_SPAN - dx * WING_SWEEP;

    const float rightWingY =
        wingCenterY - py * WING_SPAN - dy * WING_SWEEP;

    backbuffer.fillTriangle(
        wingCenterX,
        wingCenterY,

        leftWingX,
        leftWingY,

        rightWingX,
        rightWingY,

        colour);
}
