#include "OpenSkyAircraftDataSource.h"

#include <ArduinoJson.h>

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

    const String latitude = configServer.GetStoredString("latitude");
    const String longitude = configServer.GetStoredString("longitude");
    const String radius = configServer.GetStoredString("radius");

    // request
    HttpResult result = http.Get(
        "https://opensky-network.org/api/states/all",
        {{"lamin", String(latitude.toDouble() - radius.toDouble())},
         {"lamax", String(latitude.toDouble() + radius.toDouble())},
         {"lomin", String(longitude.toDouble() - radius.toDouble())},
         {"lomax", String(longitude.toDouble() + radius.toDouble())}},
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
