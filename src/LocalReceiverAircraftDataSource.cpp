#include "LocalReceiverAircraftDataSource.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "UnitSystem.h"

namespace
{
    constexpr float FT_TO_M = 0.3048f;
    constexpr float KT_TO_MPS = 0.514444f;
    constexpr float FPM_TO_MPS = 0.00508f;
    constexpr float MAX_POSITION_AGE_SECONDS = 15.0f;
    constexpr float EARTH_RADIUS_MILES = 3958.7613f;
    constexpr float DEFAULT_RANGE_MILES = 50.0f;
    constexpr unsigned long MIN_FETCH_INTERVAL_MS = 2000;
}

String LocalReceiverAircraftDataSource::NormalizeBaseUrl(String url)
{
    url.trim();
    while (url.endsWith("/"))
        url.remove(url.length() - 1);
    return url;
}

float LocalReceiverAircraftDataSource::MilesBetween(float lat1, float lon1, float lat2, float lon2)
{
    const float lat1Rad = radians(lat1);
    const float lon1Rad = radians(lon1);
    const float lat2Rad = radians(lat2);
    const float lon2Rad = radians(lon2);

    const float dLat = lat2Rad - lat1Rad;
    const float dLon = lon2Rad - lon1Rad;

    const float sinLat = sin(dLat / 2.0f);
    const float sinLon = sin(dLon / 2.0f);
    const float a = sinLat * sinLat +
                    cos(lat1Rad) * cos(lat2Rad) *
                    sinLon * sinLon;
    const float c = 2.0f * atan2(sqrt(a), sqrt(1.0f - a));
    return EARTH_RADIUS_MILES * c;
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
    const String receiverUrl = NormalizeBaseUrl(configServer.GetStoredString("receiver-url"));
    if (!receiverUrl.isEmpty())
    {
        baseUrl = receiverUrl;
    }
    else
    {
        const String receiverIp = configServer.GetStoredString("receiver-ip");
        if (!receiverIp.isEmpty())
        {
            baseUrl = NormalizeBaseUrl(String("http://") + receiverIp + ":8080/data");
        }
    }

    if (baseUrl.isEmpty())
    {
        fetchInterval = MIN_FETCH_INTERVAL_MS;
        return;
    }

    HttpResult result = http.Get(baseUrl + "/receiver.json");
    if (!result.success)
    {
        Serial.print("[WARN] Local receiver discovery failed: ");
        Serial.println(result.errorMessage);
        fetchInterval = MIN_FETCH_INTERVAL_MS;
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, result.response))
    {
        fetchInterval = MIN_FETCH_INTERVAL_MS;
        return;
    }

    fetchInterval = doc["refresh"] | MIN_FETCH_INTERVAL_MS;
    if (fetchInterval < MIN_FETCH_INTERVAL_MS)
        fetchInterval = MIN_FETCH_INTERVAL_MS;

    if (!doc["lat"].isNull())
    {
        receiverLat = doc["lat"].as<float>();
        configServer.SetStoredString("receiver-lat", String(receiverLat, 6));
    }

    if (!doc["lon"].isNull())
    {
        receiverLon = doc["lon"].as<float>();
        configServer.SetStoredString("receiver-lon", String(receiverLon, 6));
    }

    const String radiusSetting = configServer.GetStoredString("radius");
    rangeMiles = radiusSetting.isEmpty() ? DEFAULT_RANGE_MILES : ClampMilesRange(radiusSetting.toFloat());
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

    JsonDocument filter;
    filter["aircraft"][0]["hex"] = true;
    filter["aircraft"][0]["flight"] = true;
    filter["aircraft"][0]["lat"] = true;
    filter["aircraft"][0]["lon"] = true;
    filter["aircraft"][0]["alt_baro"] = true;
    filter["aircraft"][0]["alt_geom"] = true;
    filter["aircraft"][0]["gs"] = true;
    filter["aircraft"][0]["track"] = true;
    filter["aircraft"][0]["baro_rate"] = true;
    filter["aircraft"][0]["seen_pos"] = true;
    filter["aircraft"][0]["seen"] = true;
    filter["aircraft"][0]["on_ground"] = true;
    filter["aircraft"][0]["squawk"] = true;
    filter["aircraft"][0]["spi"] = true;

    constexpr uint8_t MAX_ATTEMPTS = 2;
    for (uint8_t attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
    {
        HTTPClient client;
        if (!client.begin(baseUrl + "/aircraft.json"))
        {
            Serial.println("[WARN] Local receiver aircraft request failed: unable to begin request");
            return false;
        }

        client.setTimeout(5000);

        const int responseCode = client.GET();
        if (responseCode <= 0)
        {
            Serial.print("[WARN] Local receiver aircraft request failed: ");
            Serial.println(client.errorToString(responseCode));
            client.end();
            return false;
        }

        if (responseCode != HTTP_CODE_OK)
        {
            Serial.print("[WARN] Local receiver aircraft request returned HTTP ");
            Serial.println(responseCode);
            client.end();
            return false;
        }

        const String body = client.getString();
        client.end();
        if (body.isEmpty())
        {
            Serial.println("[WARN] Local receiver aircraft request returned an empty body");
            return false;
        }

        JsonDocument doc;
        DeserializationError error = deserializeJson(
            doc,
            body,
            DeserializationOption::Filter(filter));
        if (!error)
        {
            aircraft.clear();
            JsonArrayConst items = doc["aircraft"].as<JsonArrayConst>();
            for (JsonVariantConst item : items)
            {
                Aircraft ac;
                if (!ParseAircraftObject(item, ac))
                    continue;

                if (receiverLat != 0.0f || receiverLon != 0.0f)
                {
                    const float distanceMiles =
                        MilesBetween(receiverLat, receiverLon, ac.latitude, ac.longitude);
                    if (distanceMiles > rangeMiles)
                        continue;
                }

                aircraft.push_back(ac);
            }

            return true;
        }

        Serial.print("[WARN] Local receiver aircraft JSON parse failed: ");
        Serial.println(error.f_str());
        if (error != DeserializationError::IncompleteInput || attempt + 1 >= MAX_ATTEMPTS)
            return false;

        delay(250);
    }

    return false;
}
