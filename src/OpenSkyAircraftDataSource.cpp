#include "OpenSkyAircraftDataSource.h"

#include <ArduinoJson.h>
#include "UnitSystem.h"

namespace
{
    constexpr float MILES_PER_DEG_LAT = 69.172f;

    float MilesToLatitudeDegrees(float miles)
    {
        return miles / MILES_PER_DEG_LAT;
    }

    float MilesToLongitudeDegrees(float miles, float latitudeDegrees)
    {
        const float milesPerDegree = MILES_PER_DEG_LAT * cos(radians(latitudeDegrees));
        if (milesPerDegree <= 0.0f)
            return 0.0f;
        return miles / milesPerDegree;
    }
}

void OpenSkyAircraftDataSource::Initialise()
{
    // calculate how often we can call OpenSky API before being rate limited
    constexpr int MS_PER_DAY = 24 * 60 * 60 * 1000;
    constexpr int ANONYMOUS_TOKENS_PER_DAY = 400;
    constexpr int AUTHED_TOKENS_PER_DAY = 4000;
    constexpr int TOKEN_BUFFER = 3;
    int dailyRequestBudget = ANONYMOUS_TOKENS_PER_DAY - TOKEN_BUFFER; // non-authed tokens minus buffer

    const String token = authHandler.GetValidToken(
        configServer.GetStoredString("opensky-id"),
        configServer.GetStoredString("opensky-secret"));
    if (!token.isEmpty())
        dailyRequestBudget = AUTHED_TOKENS_PER_DAY - TOKEN_BUFFER; // authed tokens minus buffer

    fetchInterval = MS_PER_DAY / dailyRequestBudget;
}

unsigned long OpenSkyAircraftDataSource::GetFetchIntervalMs() const
{
    return fetchInterval;
}

bool OpenSkyAircraftDataSource::Fetch(std::vector<Aircraft>& aircraft)
{
    // auth
    const String token = authHandler.GetValidToken(
        configServer.GetStoredString("opensky-id"),
        configServer.GetStoredString("opensky-secret"));

    std::vector<std::pair<String, String>> headers = {};
    if (!token.isEmpty())
        headers.push_back({"Authorization", "Bearer " + token});

    const String receiverLat = configServer.GetStoredString("receiver-lat");
    const String receiverLon = configServer.GetStoredString("receiver-lon");
    const String latitude = receiverLat.isEmpty() ? configServer.GetStoredString("latitude") : receiverLat;
    const String longitude = receiverLon.isEmpty() ? configServer.GetStoredString("longitude") : receiverLon;
    const String radiusSetting = configServer.GetStoredString("radius");
    float radiusMiles = radiusSetting.isEmpty() ? 50.0f : ClampMilesRange(radiusSetting.toFloat());
    const float latitudeDegrees = MilesToLatitudeDegrees(radiusMiles);
    const float longitudeDegrees = MilesToLongitudeDegrees(radiusMiles, latitude.toDouble());

    // request
    HttpResult result = http.Get(
        "https://opensky-network.org/api/states/all",
        {{"lamin", String(latitude.toDouble() - latitudeDegrees)},
         {"lamax", String(latitude.toDouble() + latitudeDegrees)},
         {"lomin", String(longitude.toDouble() - longitudeDegrees)},
         {"lomax", String(longitude.toDouble() + longitudeDegrees)}},
        headers);

    // If request failed, skip this update
    if (!result.success)
    {
        Serial.print("[WARN] OpenSky API request failed: ");
        Serial.println(result.errorMessage);
        return false;
    }

    // track
    JsonDocument doc;
    deserializeJson(doc, result.response);
    aircraft = JsonParser::ParseArray<Aircraft>(doc["states"]);

    return true;
}
