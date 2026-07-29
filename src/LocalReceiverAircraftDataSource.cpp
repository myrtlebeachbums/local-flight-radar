#include "LocalReceiverAircraftDataSource.h"

#include <ArduinoJson.h>

namespace
{
    constexpr float FT_TO_M = 0.3048f;
    constexpr float KT_TO_MPS = 0.514444f;
    constexpr float FPM_TO_MPS = 0.00508f;
    constexpr float MAX_POSITION_AGE_SECONDS = 15.0f;
}

String LocalReceiverAircraftDataSource::NormalizeBaseUrl(String url)
{
    url.trim();
    while (url.endsWith("/"))
        url.remove(url.length() - 1);
    return url;
}

bool LocalReceiverAircraftDataSource::ParseAircraftObject(const JsonVariantConst& item, Aircraft& aircraft)
{
    const JsonObjectConst obj = item.as<JsonObjectConst>();

    const String hex = obj["hex"] | "";
    if (hex.isEmpty())
        return false;

    const JsonVariantConst latVariant = obj["lat"];
    const JsonVariantConst lonVariant = obj["lon"];
    if (latVariant.isNull() || lonVariant.isNull())
        return false;

    const JsonVariantConst altBaro = obj["alt_baro"];
    const bool isGround =
        (altBaro.is<const char*>() && strcmp(altBaro.as<const char*>(), "ground") == 0) ||
        (obj["on_ground"] | false);
    if (isGround)
        return false;

    const float seenPosSeconds = obj["seen_pos"] | obj["seen"] | 0.0f;
    if (seenPosSeconds > MAX_POSITION_AGE_SECONDS)
        return false;

    const float altitudeFeet = altBaro.is<float>() || altBaro.is<int>()
                                   ? altBaro.as<float>()
                                   : 0.0f;
    const float altitudeMeters = altitudeFeet * FT_TO_M;
    const float speedKnots = obj["gs"] | 0.0f;
    const float speedMetersPerSecond = speedKnots * KT_TO_MPS;
    const float verticalRateFeetPerMinute = obj["baro_rate"] | 0.0f;
    const float verticalRateMetersPerSecond = verticalRateFeetPerMinute * FPM_TO_MPS;

    aircraft.icao24 = hex;
    aircraft.callsign = (obj["flight"] | "");
    aircraft.callsign.trim();
    aircraft.originCountry = "";
    aircraft.timePosition = millis() / 1000;
    aircraft.lastContact = aircraft.timePosition;
    aircraft.longitude = lonVariant.as<float>();
    aircraft.latitude = latVariant.as<float>();
    aircraft.baroAltitude = altitudeMeters;
    aircraft.onGround = false;
    aircraft.velocity = speedMetersPerSecond;
    aircraft.trueTrack = obj["track"] | 0.0f;
    aircraft.verticalRate = verticalRateMetersPerSecond;
    aircraft.geoAltitude = (obj["alt_geom"] | altitudeFeet) * FT_TO_M;
    aircraft.squawk = (obj["squawk"] | "");
    aircraft.spi = obj["spi"] | false;
    aircraft.positionSource = 0;
    aircraft.category = 0;

    return true;
}

void LocalReceiverAircraftDataSource::Initialise()
{
    baseUrl = NormalizeBaseUrl(configServer.GetStoredString("receiver-url"));

    if (baseUrl.isEmpty())
    {
        fetchInterval = 1000;
        return;
    }

    HttpResult result = http.Get(baseUrl + "/receiver.json");
    if (!result.success)
    {
        Serial.print("[WARN] Local receiver discovery failed: ");
        Serial.println(result.errorMessage);
        fetchInterval = 1000;
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, result.response))
    {
        fetchInterval = 1000;
        return;
    }

    fetchInterval = doc["refresh"] | 1000;
    if (fetchInterval < 1000)
        fetchInterval = 1000;
}

unsigned long LocalReceiverAircraftDataSource::GetFetchIntervalMs() const
{
    return fetchInterval;
}

bool LocalReceiverAircraftDataSource::Fetch(std::vector<Aircraft>& aircraft)
{
    if (baseUrl.isEmpty())
    {
        Serial.println("[WARN] Local receiver base URL is not configured");
        return false;
    }

    HttpResult result = http.Get(baseUrl + "/aircraft.json");
    if (!result.success)
    {
        Serial.print("[WARN] Local receiver aircraft request failed: ");
        Serial.println(result.errorMessage);
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, result.response);
    if (error)
    {
        Serial.print("[WARN] Local receiver aircraft JSON parse failed: ");
        Serial.println(error.f_str());
        return false;
    }

    aircraft.clear();
    JsonArrayConst items = doc["aircraft"].as<JsonArrayConst>();
    for (JsonVariantConst item : items)
    {
        Aircraft ac;
        if (ParseAircraftObject(item, ac))
            aircraft.push_back(ac);
    }

    return true;
}
