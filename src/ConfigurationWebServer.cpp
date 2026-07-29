#include "ConfigurationWebServer.h"
#include "DetailFields.h"
#include "UnitSystem.h"
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <time.h>

namespace
{
    static String NormalizeBaseUrl(String url)
    {
        url.trim();
        while (url.endsWith("/"))
            url.remove(url.length() - 1);
        return url;
    }

    static String FormatUtcOffsetLabel(int offsetMinutes)
    {
        const char sign = offsetMinutes <= 0 ? '+' : '-';
        const int absoluteMinutes = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
        const int hours = absoluteMinutes / 60;
        const int minutes = absoluteMinutes % 60;

        char buffer[16];
        snprintf(buffer, sizeof(buffer), "UTC%c%02d:%02d", sign, hours, minutes);
        return String(buffer);
    }

    static bool ProbeReceiverJson(const String &baseUrl, String &discoveryMessage)
    {
        HTTPClient client;
        client.setTimeout(1500);

        if (!client.begin(baseUrl + "/receiver.json"))
        {
            discoveryMessage = "Unable to begin request for receiver.json";
            return false;
        }

        const int responseCode = client.GET();
        if (responseCode != HTTP_CODE_OK)
        {
            discoveryMessage = "HTTP " + String(responseCode) + " while reading receiver.json";
            client.end();
            return false;
        }

        JsonDocument doc;
        const DeserializationError error = deserializeJson(doc, *client.getStreamPtr());
        client.end();
        if (error)
        {
            discoveryMessage = String("Invalid receiver.json: ") + error.f_str();
            return false;
        }

        discoveryMessage = "Receiver.json found";
        if (!doc["lat"].isNull() && !doc["lon"].isNull())
            discoveryMessage += " with receiver location";
        return true;
    }

    static String AutoDiscoverReceiverFromMdns(String &discoveryMessage, String &selectedIdentity)
    {
        const int serviceCount = MDNS.queryService("http", "tcp");
        if (serviceCount <= 0)
        {
            discoveryMessage = "No mDNS HTTP services were found on this network.";
            return "";
        }

        discoveryMessage = "Checking mDNS HTTP services...";
        selectedIdentity = "";

        for (int i = 0; i < serviceCount; ++i)
        {
            const IPAddress ip = MDNS.IP(i);
            if (ip == IPAddress(0, 0, 0, 0))
                continue;

            const uint16_t port = MDNS.port(i);
            const String host = MDNS.hostname(i);
            const String serviceName = host.length() ? host : String("service-") + String(i);

            const String candidates[] = {
                NormalizeBaseUrl(String("http://") + ip.toString() + ":" + String(port) + "/data"),
                NormalizeBaseUrl(String("http://") + ip.toString() + ":" + String(port)),
                NormalizeBaseUrl(String("http://") + ip.toString() + "/data"),
                NormalizeBaseUrl(String("http://") + ip.toString() + "/skyaware/data"),
                NormalizeBaseUrl(String("http://") + ip.toString() + "/tar1090/data"),
            };

            for (const auto &candidate : candidates)
            {
                String probeMessage;
                if (ProbeReceiverJson(candidate, probeMessage))
                {
                    const String resolvedIp = ip.toString();
                    discoveryMessage = String("Receiver found via mDNS: ") + serviceName + String(" (") + resolvedIp + ")";
                    selectedIdentity = resolvedIp;
                    return candidate;
                }
            }
        }

        discoveryMessage = "mDNS found HTTP services, but none exposed a compatible receiver.json endpoint.";
        return "";
    }

    static String DiscoverReceiverBaseUrl(const String &hostOrUrl, String &discoveryMessage, String &selectedIdentity);

    static String ResolveConfiguredReceiverBaseUrl(const String &receiverIp, const String &receiverUrl)
    {
        String configuredUrl = receiverUrl;
        configuredUrl.trim();
        if (!configuredUrl.isEmpty())
            return NormalizeBaseUrl(configuredUrl);

        String configuredIp = receiverIp;
        configuredIp.trim();
        if (configuredIp.isEmpty())
            return "";

        String discoveryMessage;
        String selectedIdentity;
        return DiscoverReceiverBaseUrl(configuredIp, discoveryMessage, selectedIdentity);
    }

    static float MilesBetween(float lat1, float lon1, float lat2, float lon2)
    {
        constexpr float EARTH_RADIUS_MILES = 3958.7613f;

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

    static bool LoadReceiverLocation(const String &baseUrl, float &receiverLat, float &receiverLon, String &errorMessage)
    {
        HTTPClient client;
        client.setTimeout(1500);

        if (!client.begin(baseUrl + "/receiver.json"))
        {
            errorMessage = "Unable to begin request for receiver.json";
            return false;
        }

        const int responseCode = client.GET();
        if (responseCode != HTTP_CODE_OK)
        {
            errorMessage = "HTTP " + String(responseCode) + " while reading receiver.json";
            client.end();
            return false;
        }

        JsonDocument doc;
        const DeserializationError error = deserializeJson(doc, *client.getStreamPtr());
        client.end();
        if (error)
        {
            errorMessage = String("Invalid receiver.json: ") + error.f_str();
            return false;
        }

        if (doc["lat"].isNull() || doc["lon"].isNull())
        {
            errorMessage = "Receiver.json did not include a receiver location";
            return false;
        }

        receiverLat = doc["lat"].as<float>();
        receiverLon = doc["lon"].as<float>();
        return true;
    }

    static bool LoadDistancePresetCounts(const String &receiverIp, const String &receiverUrl, UnitSystem unitSystem, String &countsJson, String &errorMessage)
    {
        const String baseUrl = ResolveConfiguredReceiverBaseUrl(receiverIp, receiverUrl);
        if (baseUrl.isEmpty())
        {
            errorMessage = "No receiver endpoint is configured yet.";
            return false;
        }

        float receiverLat = 0.0f;
        float receiverLon = 0.0f;
        if (!LoadReceiverLocation(baseUrl, receiverLat, receiverLon, errorMessage))
            return false;

        HTTPClient client;
        client.setTimeout(5000);
        if (!client.begin(baseUrl + "/aircraft.json"))
        {
            errorMessage = "Unable to begin request for aircraft.json";
            return false;
        }

        const int responseCode = client.GET();
        if (responseCode != HTTP_CODE_OK)
        {
            errorMessage = "HTTP " + String(responseCode) + " while reading aircraft.json";
            client.end();
            return false;
        }

        JsonDocument filter;
        filter["aircraft"][0]["lat"] = true;
        filter["aircraft"][0]["lon"] = true;
        filter["aircraft"][0]["alt_baro"] = true;
        filter["aircraft"][0]["seen_pos"] = true;
        filter["aircraft"][0]["seen"] = true;
        filter["aircraft"][0]["on_ground"] = true;

        JsonDocument doc;
        const DeserializationError error = deserializeJson(doc, *client.getStreamPtr(), DeserializationOption::Filter(filter));
        client.end();
        if (error)
        {
            errorMessage = String("Invalid aircraft.json: ") + error.f_str();
            return false;
        }

        constexpr float presetDistances[] = { 10.0f, 25.0f, 50.0f, 100.0f, 200.0f };
        constexpr size_t presetCount = sizeof(presetDistances) / sizeof(presetDistances[0]);
        int counts[presetCount] = { 0 };

        const JsonArrayConst aircraftArray = doc["aircraft"].as<JsonArrayConst>();
        for (const JsonVariantConst item : aircraftArray)
        {
            const JsonVariantConst latVariant = item["lat"];
            const JsonVariantConst lonVariant = item["lon"];
            if (latVariant.isNull() || lonVariant.isNull())
                continue;

            const JsonVariantConst altBaro = item["alt_baro"];
            const bool isGround =
                (altBaro.is<const char *>() && strcmp(altBaro.as<const char *>(), "ground") == 0) ||
                (item["on_ground"] | false);
            if (isGround)
                continue;

            const float seenPosSeconds = item["seen_pos"] | item["seen"] | 0.0f;
            if (seenPosSeconds > 15.0f)
                continue;

            const float aircraftLat = latVariant.as<float>();
            const float aircraftLon = lonVariant.as<float>();
            const float distanceMiles = MilesBetween(receiverLat, receiverLon, aircraftLat, aircraftLon);

            for (size_t i = 0; i < presetCount; ++i)
            {
                const float presetMiles = DisplayDistanceToMiles(unitSystem, presetDistances[i]);
                if (distanceMiles <= presetMiles)
                    ++counts[i];
            }
        }

        JsonDocument countsDoc;
        for (size_t i = 0; i < presetCount; ++i)
        {
            countsDoc[String((int)presetDistances[i])] = counts[i];
        }

        countsJson = "";
        serializeJson(countsDoc, countsJson);
        return true;
    }

    static String DiscoverReceiverBaseUrl(const String &hostOrUrl, String &discoveryMessage, String &selectedIdentity)
    {
        String input = hostOrUrl;
        input.trim();
        if (input.isEmpty())
        {
            return AutoDiscoverReceiverFromMdns(discoveryMessage, selectedIdentity);
        }

        if (input.indexOf("://") >= 0)
        {
            const String baseUrl = NormalizeBaseUrl(input);
            if (ProbeReceiverJson(baseUrl, discoveryMessage))
            {
                selectedIdentity = input;
                return baseUrl;
            }
            return "";
        }

        const String candidates[] = {
            NormalizeBaseUrl(String("http://") + input + ":8080/data"),
            NormalizeBaseUrl(String("http://") + input + "/skyaware/data"),
            NormalizeBaseUrl(String("http://") + input + "/tar1090/data"),
            NormalizeBaseUrl(String("http://") + input + ":8504/data"),
            NormalizeBaseUrl(String("http://") + input + "/data"),
        };

        for (const auto &candidate : candidates)
        {
            if (ProbeReceiverJson(candidate, discoveryMessage))
            {
                selectedIdentity = input;
                return candidate;
            }
        }

        discoveryMessage = "No compatible dump1090-style receiver.json endpoint was found.";
        return "";
    }

    static String BuildDetailFieldsHtml(Preferences &prefs)
    {
        String html;
        html.reserve(1200);

        for (size_t i = 0; i < AIRCRAFT_DETAIL_FIELD_COUNT; ++i)
        {
            const auto &spec = GetAircraftDetailFieldSpec(i);
            const String checked = prefs.getString(spec.key, spec.defaultEnabled ? "true" : "false") == "true" ? "checked" : "";

            html += "<label class='flex flex-col sm:flex-row items-start sm:items-center gap-2'>";
            html += "<span>";
            html += spec.label;
            html += ":</span>";
            html += "<input";
            html += " name='" + String(spec.key) + "'";
            html += " type='checkbox'";
            html += " data-detail-field";
            if (!checked.isEmpty())
                html += " checked";
            html += " class='px-3 sm:px-1 accent-green-500'>";
            html += "</label>";
        }

        return html;
    }

    static String BuildStatusJson(Preferences &prefs)
    {
        JsonDocument doc;

        const String receiverIp = prefs.getString("receiver-ip", "");
        const String receiverUrl = prefs.getString("receiver-url", "");
        const String units = prefs.getString("units", "aviation");
        const String radiusMiles = prefs.getString("radius", "");
        const String scanlineEnabled = prefs.getString("scanline", "true");
        const String infoTextEnabled = prefs.getString("infotext", "true");
        const String incompleteDataEnabled = prefs.getString("show-incomplete-data", "true");
        const String triangleEnabled = prefs.getString("triangle", "true");
        const String sleepEnabled = prefs.getString("sleep-enabled", "false");
        const String sleepStart = prefs.getString("sleep-start", "22:00");
        const String sleepEnd = prefs.getString("sleep-end", "07:00");
        const int timezoneOffsetMinutes = prefs.getString("sleep-timezone-offset", "").toInt();

        bool timeSynced = false;
        String localDateTime;
        struct tm timeInfo;
        if (getLocalTime(&timeInfo, 25))
        {
            char buffer[48];
            if (strftime(buffer, sizeof(buffer), "%a %b %d %Y %I:%M:%S %p", &timeInfo) > 0)
            {
                localDateTime = buffer;
                timeSynced = true;
            }
        }

        doc["wifiConnected"] = WiFi.status() == WL_CONNECTED;
        doc["wifiSsid"] = WiFi.SSID();
        doc["ipAddress"] = WiFi.localIP().toString();
        doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
        doc["receiverConfigured"] = !receiverIp.isEmpty() || !receiverUrl.isEmpty();
        doc["receiverIp"] = receiverIp;
        doc["receiverUrl"] = receiverUrl;
        doc["units"] = units;
        doc["radiusMiles"] = radiusMiles;
        doc["scanline"] = scanlineEnabled == "true";
        doc["infoText"] = infoTextEnabled == "true";
        doc["showIncompleteData"] = incompleteDataEnabled == "true";
        doc["triangle"] = triangleEnabled == "true";
        doc["timeSynced"] = timeSynced;
        doc["localDateTime"] = localDateTime;
        doc["sleepEnabled"] = sleepEnabled == "true";
        doc["timezoneOffsetMinutes"] = timezoneOffsetMinutes;
        doc["timezoneLabel"] = FormatUtcOffsetLabel(timezoneOffsetMinutes);
        doc["sleepStart"] = sleepStart;
        doc["sleepEnd"] = sleepEnd;
        doc["mode"] = (receiverIp.isEmpty() && receiverUrl.isEmpty()) ? "setup" : "running";

        String body;
        serializeJson(doc, body);
        return body;
    }
}

// HTML stored in flash
// %PLACEHOLDER% tokens are substituted at serve time by the template processor
static const char CONFIG_HTML[] PROGMEM = R"(
<html>
    <head>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <title>Configure Local Flight Radar</title>
        <script src="https://cdn.jsdelivr.net/npm/@tailwindcss/browser@4.3.0"></script>
        <script src="https://cdn.jsdelivr.net/npm/qrcodejs@1.0.0/qrcode.min.js"></script>
    </head>
    <body class="font-mono bg-gray-900 text-green-500 min-h-screen p-4 sm:p-0 text-md sm:text-sm">
        <fieldset class="border border-green-500 p-5 w-full max-w-2xl mx-auto sm:m-10">
            <legend class="px-2">Configure Local Flight Radar</legend>

            <form id="cfg" action="/save" method="POST" class="flex flex-col gap-4 sm:gap-2">

                <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                    <span>Flight tracker IP or hostname:</span>
                    <input
                        name="receiver-ip"
                        id="receiver-ip"
                        type="text"
                        value='%RECEIVER_IP%'
                        placeholder="172.16.0.100"
                        class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                </label>

                <input type="hidden" name="receiver-url" id="receiver-url" value='%RECEIVER_URL%'>

                <div class="flex flex-col gap-2 text-xs text-green-400 leading-relaxed">
                    <div id="receiver-url-display">%RECEIVER_URL_TEXT%</div>
                    <div>Find receiver checks common dump1090, SkyAware, and tar1090 data paths on the address you enter.</div>
                </div>

                <div class="flex flex-col sm:flex-row gap-4 sm:items-center">
                    <button
                        id="discover-receiver"
                        type="button"
                        class="bg-green-500 text-black px-4 py-2 text-lg sm:text-base sm:px-2 sm:py-0 cursor-pointer self-start">
                        Find receiver
                    </button>
                    <div id="discover-status" class="text-xs text-green-400"></div>
                </div>

                <div class="border border-green-500/60 p-4 rounded-sm bg-black/20 flex flex-col gap-2">
                    <div class="text-sm text-green-400">Device status</div>
                    <div id="status-summary" class="text-xs text-green-400 leading-relaxed">Loading status...</div>
                    <div id="status-details" class="text-xs text-green-400 leading-relaxed"></div>
                    <button
                        id="sync-time"
                        type="button"
                        class="bg-green-500 text-black px-3 py-2 text-sm sm:text-xs sm:px-2 sm:py-1 cursor-pointer self-start">
                        Sync time now
                    </button>
                    <div id="sync-status" class="text-xs text-green-400 leading-relaxed"></div>
                </div>

                <div class="border border-green-500/60 p-4 rounded-sm bg-black/20 flex flex-col gap-3">
                    <div class="text-sm text-green-400">Setup QR code</div>
                    <div class="text-xs text-green-400 leading-relaxed">Scan this to reopen the setup page on this device or another browser.</div>
                    <div id="setup-qr" class="bg-white p-3 inline-flex self-start"></div>
                    <div id="setup-url-text" class="text-xs text-green-400 break-all"></div>
                </div>

                <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                    <span>Measurement units:</span>
                    <select
                        name="units"
                        id="units"
                        class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                        <option value="aviation" %UNIT_AVIATION%>Aviation - feet, knots, nautical miles</option>
                        <option value="imperial" %UNIT_IMPERIAL%>Imperial - feet, mph, statute miles</option>
                        <option value="metric" %UNIT_METRIC%>Metric - meters, km/h, kilometers</option>
                    </select>
                </label>

                <div class="border border-green-500/60 p-4 rounded-sm bg-black/20 flex flex-col gap-3">
                    <div class="text-sm text-green-400">Screen density presets</div>
                    <div class="text-xs text-green-400 leading-relaxed">Choose the display density that best fits your receiver. Each preset shows how many aircraft are visible right now at that setting. The advanced section below lets you enter a custom whole-number distance if none of the presets fit your receiver.</div>
                    <div id="distance-matrix" class="grid grid-cols-1 sm:grid-cols-2 gap-3"></div>
                    <div id="distance-summary" class="text-xs text-green-400 leading-relaxed"></div>
                </div>

                <details class="border border-green-500/60 p-4 rounded-sm bg-black/20">
                    <summary class="cursor-pointer text-sm text-green-400">Advanced distance settings</summary>
                    <div class="mt-4 flex flex-col gap-4">
                        <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                            <span id="distance-label">Custom distance (nautical miles):</span>
                            <input
                                name="radius"
                                id="radius"
                                type="number"
                                min="1"
                                step="1"
                                max='%RADIUS_MAX%'
                                value='%RADIUS%'
                                class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                        </label>

                        <div id="distance-range-hint" class="text-xs text-green-400 leading-relaxed"></div>

                        <div id="unit-help" class="text-xs text-green-400 leading-relaxed">
                            Aviation: feet, knots, nautical miles. Imperial: feet, mph, statute miles. Metric: meters, km/h, kilometers. Presets appear above; whole numbers only.
                        </div>
                    </div>
                </details>

                <div class="border border-green-500/60 p-4 rounded-sm bg-black/20 flex flex-col gap-4">
                    <div class="text-sm text-green-400">Display schedule</div>
                    <div class="text-xs text-green-400 leading-relaxed">Keep the screen and onboard LED quiet during sleeping hours. Times are saved in your browser's local time zone. Wake-on-knob lets a press temporarily light the display.</div>
                    <div class="flex flex-col gap-3">
                        <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                            <span>Enable schedule:</span>
                            <input
                                name="sleep-enabled"
                                type="checkbox"
                                %SLEEP_ENABLED%
                                class="px-3 sm:px-1 accent-green-500">
                        </label>
                        <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                            <span>Sleep starts:</span>
                            <input
                                name="sleep-start"
                                id="sleep-start"
                                type="time"
                                value="%SLEEP_START%"
                                class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                        </label>
                        <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                            <span>Sleep ends:</span>
                            <input
                                name="sleep-end"
                                id="sleep-end"
                                type="time"
                                value="%SLEEP_END%"
                                class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                        </label>
                        <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                            <span>Wake on knob press:</span>
                            <input
                                name="sleep-wake-knob"
                                type="checkbox"
                                %SLEEP_WAKE_KNOB%
                                class="px-3 sm:px-1 accent-green-500">
                        </label>
                    </div>
                    <input type="hidden" name="sleep-timezone-offset" id="sleep-timezone-offset" value="%SLEEP_TZ_OFFSET%">
                </div>

                <div class="flex flex-col sm:flex-row gap-4 sm:justify-between">
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>Radar sweep:</span>
                        <input
                            name="scanline"
                            type="checkbox"
                            %SCANLINE%
                            class="px-3 sm:px-1 accent-green-500">
                    </label>
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>Aircraft Info:</span>
                        <input
                            name="infotext"
                            type="checkbox"
                            %INFOTEXT%
                            class="px-3 sm:px-1 accent-green-500">
                    </label>
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>Incomplete data:</span>
                        <input
                            name="show-incomplete-data"
                            type="checkbox"
                            %INCOMPLETE_DATA%
                            class="px-3 sm:px-1 accent-green-500">
                    </label>
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>Directional Aircraft:</span>
                        <input
                            name="triangle"
                            type="checkbox"
                            %TRIANGLE%
                            class="px-3 sm:px-1 accent-green-500">
                    </label>
                </div>

                <details class="border border-green-500/60 p-4 rounded-sm bg-black/20">
                    <summary class="cursor-pointer text-sm text-green-400">Advanced aircraft detail fields</summary>
                    <div class="mt-4 flex flex-col gap-4">
                        <div class="text-xs text-green-400 leading-relaxed">Select up to 5 fields to show below the flight code on the detail screen.</div>
                        <div id="detail-limit" class="text-xs text-green-400"></div>
                        <div class="flex flex-col gap-3">
                            %DETAIL_FIELDS%
                        </div>
                    </div>
                </details>

                <div class="flex flex-col gap-4">
                    <div class="flex flex-col sm:flex-row gap-4 sm:gap-5">
                        <input
                            type="submit"
                            value="Save"
                            class="bg-green-500 text-black mt-4 px-4 py-3 text-lg sm:text-base sm:px-2 sm:py-0 self-start cursor-pointer">

                        <div id="result" class="mt-4 px-1 sm:px-10"></div>
                    </div>

                    <details class="border border-red-500/60 p-4 rounded-sm bg-black/20">
                        <summary class="cursor-pointer text-sm text-red-300">Wi-Fi and factory reset</summary>
                        <div class="mt-4 flex flex-col gap-4 text-xs text-red-200 leading-relaxed">
                            <div>Wi-Fi reset clears only the saved wireless credentials and reboots into setup mode again. Factory reset clears all saved configuration and Wi-Fi credentials.</div>
                            <div class="flex flex-col sm:flex-row gap-3">
                                <button type="button" id="wifi-reset" class="bg-red-500 text-black px-4 py-2 text-lg sm:text-base sm:px-2 sm:py-0 cursor-pointer self-start">Wi-Fi reset</button>
                                <button type="button" id="factory-reset" class="border border-red-400 text-red-200 px-4 py-2 text-lg sm:text-base sm:px-2 sm:py-0 cursor-pointer self-start">Factory reset</button>
                            </div>
                        </div>
                    </details>
                </div>
            </form>
        </fieldset>

        <script>
            const distanceLabels = {
                aviation: 'Custom distance (nautical miles):',
                imperial: 'Custom distance (statute miles):',
                metric: 'Custom distance (kilometers):'
            };
            const distanceUnits = {
                aviation: 'nautical miles',
                imperial: 'statute miles',
                metric: 'kilometers'
            };
            const distanceMaxes = {
                aviation: %RADIUS_MAX_AVIATION%,
                imperial: %RADIUS_MAX_IMPERIAL%,
                metric: %RADIUS_MAX_METRIC%
            };
            const distancePresets = [
                { label: 'Sparse', value: 10, description: 'A cleaner screen with plenty of room.' },
                { label: 'Comfortable', value: 25, description: 'A good default for a readable display.' },
                { label: 'Busy', value: 50, description: 'More aircraft, less empty space.' },
                { label: 'Crowded', value: 100, description: 'A lot of traffic on screen.' },
                { label: 'Hard To Read', value: 200, description: 'Only if you really want a dense display.' }
            ];
            const distancePresetCounts = %DISTANCE_PRESET_COUNTS%;
            const maxDetailFields = 5;

            function presetCountText(value) {
                if (!distancePresetCounts || typeof distancePresetCounts !== 'object') {
                    return 'Counts unavailable';
                }

                const rawCount = distancePresetCounts[String(value)];
                const count = Number(rawCount);
                if (!Number.isFinite(count)) {
                    return 'Counts unavailable';
                }

                return `${count} aircraft visible right now`;
            }

            function renderDistanceMatrix() {
                const units = document.getElementById('units').value;
                const unitLabel = distanceUnits[units] || distanceUnits.aviation;
                const radius = document.getElementById('radius');
                const matrix = document.getElementById('distance-matrix');
                const summary = document.getElementById('distance-summary');
                const currentValue = radius.value;
                const selectedPreset = distancePresets.find(preset => String(preset.value) === currentValue) || null;

                matrix.innerHTML = '';
                distancePresets.forEach(preset => {
                    const button = document.createElement('button');
                    const active = selectedPreset && selectedPreset.value === preset.value;
                    button.type = 'button';
                    button.className = active
                        ? 'text-left border border-green-400 bg-green-500/10 px-4 py-3 rounded-sm transition-colors'
                        : 'text-left border border-green-500/40 bg-black/40 px-4 py-3 rounded-sm transition-colors hover:border-green-300';
                    button.setAttribute('aria-pressed', active ? 'true' : 'false');
                    button.innerHTML = `
                        <div class="flex items-center justify-between gap-3">
                            <span class="text-base sm:text-sm font-semibold">${preset.label}</span>
                            <span class="text-xs text-green-300">${preset.value} ${unitLabel}</span>
                        </div>
                        <div class="mt-1 text-xs text-green-400/90">${preset.description}</div>
                        <div class="mt-1 text-xs text-green-300">${presetCountText(preset.value)}</div>
                    `;
                    button.addEventListener('click', () => {
                        radius.value = String(preset.value);
                        updateUnitHints();
                    });
                    matrix.appendChild(button);
                });

                if (selectedPreset) {
                    const countText = presetCountText(selectedPreset.value);
                    summary.textContent = countText === 'Counts unavailable'
                        ? `${selectedPreset.label} is selected. Counts are unavailable right now, but you can still enter a custom whole-number distance in Advanced.`
                        : `${selectedPreset.label} is selected. ${countText} at this setting. You can still enter a custom whole-number distance in Advanced.`;
                } else
                    summary.textContent = `Custom distance selected: ${currentValue || '1'} ${unitLabel}. The presets above show how many aircraft each setting would display right now.`;
            }

            function updateUnitHints() {
                const units = document.getElementById('units').value;
                const maxDistance = distanceMaxes[units] || distanceMaxes.aviation;
                document.getElementById('distance-label').textContent = distanceLabels[units] || distanceLabels.aviation;
                const radius = document.getElementById('radius');
                radius.max = maxDistance;
                document.getElementById('distance-range-hint').textContent = `Allowed range: 1 to ${maxDistance} ${distanceUnits[units] || distanceUnits.aviation}. Whole numbers only.`;
                renderDistanceMatrix();
            }

            function updateDetailLimit() {
                const boxes = Array.from(document.querySelectorAll('[data-detail-field]'));
                const checked = boxes.filter(box => box.checked);
                const count = checked.length;
                document.getElementById('detail-limit').textContent = `${count} of ${maxDetailFields} fields selected`;
                boxes.forEach(box => {
                    box.disabled = !box.checked && count >= maxDetailFields;
                });
            }

            function syncSleepTimezoneOffset() {
                const field = document.getElementById('sleep-timezone-offset');
                if (field) {
                    field.value = String(new Date().getTimezoneOffset());
                }
            }

            function renderSetupQr() {
                const target = document.getElementById('setup-qr');
                target.innerHTML = '';
                const setupUrl = window.location.origin + '/';
                if (window.QRCode) {
                    new QRCode(target, {
                        text: setupUrl,
                        width: 160,
                        height: 160,
                        colorDark: '#22c55e',
                        colorLight: '#ffffff',
                        correctLevel: QRCode.CorrectLevel.M
                    });
                } else {
                    target.textContent = setupUrl;
                }
            }

            function clearDiscoveredReceiver() {
                document.getElementById('receiver-url').value = '';
                document.getElementById('receiver-url-display').textContent = 'No receiver endpoint discovered yet.';
            }

            function renderStatus(data) {
                const summary = document.getElementById('status-summary');
                const details = document.getElementById('status-details');
                const lines = [];

                if (data.wifiConnected) {
                    lines.push(`Wi-Fi connected to ${data.wifiSsid || 'saved network'} at ${data.ipAddress || 'unknown IP'}.`);
                } else {
                    lines.push('Wi-Fi is not connected right now.');
                }

                if (data.timeSynced) {
                    const timezoneLabel = data.timezoneLabel || 'UTC+00:00';
                    lines.push(`Local time: ${data.localDateTime || 'available but unreadable'} (${timezoneLabel}).`);
                } else {
                    const timezoneLabel = data.timezoneLabel || 'UTC+00:00';
                    lines.push(`Local time is not synced yet (${timezoneLabel}).`);
                }

                if (data.sleepEnabled) {
                    lines.push(`Sleep window: ${data.sleepStart || 'unset'} to ${data.sleepEnd || 'unset'}.`);
                } else {
                    lines.push('Sleep schedule is disabled.');
                }

                if (data.receiverConfigured) {
                    lines.push(`Receiver configured: ${data.receiverUrl || data.receiverIp || 'saved endpoint'}.`);
                } else {
                    lines.push('Receiver not configured yet.');
                }

                lines.push(`Mode: ${data.mode === 'setup' ? 'setup' : 'running'}.`);
                summary.textContent = lines[0];
                details.textContent = lines.slice(1).join(' ');
                const syncStatus = document.getElementById('sync-status');
                if (syncStatus) {
                    syncStatus.textContent = data.timeSynced ? 'Clock sync looks good.' : 'Clock has not synced yet.';
                }
            }

            function loadStatus() {
                fetch('/status')
                    .then(response => response.json())
                    .then(renderStatus)
                    .catch(() => {
                        document.getElementById('status-summary').textContent = 'Status unavailable right now.';
                        document.getElementById('status-details').textContent = '';
                        const syncStatus = document.getElementById('sync-status');
                        if (syncStatus) {
                            syncStatus.textContent = '';
                        }
                    });
            }

            function syncTimeNow() {
                const syncStatus = document.getElementById('sync-status');
                if (syncStatus) {
                    syncStatus.textContent = 'Syncing time now...';
                }

                fetch('/sync-time', { method: 'POST' })
                    .then(response => response.text().then(text => ({ ok: response.ok, text })))
                    .then(result => {
                        if (syncStatus) {
                            syncStatus.textContent = result.text;
                        }
                        loadStatus();
                    })
                    .catch(error => {
                        if (syncStatus) {
                            syncStatus.textContent = error.message;
                        }
                    });
            }

            function discoverReceiver() {
                const host = document.getElementById('receiver-ip').value.trim();
                const status = document.getElementById('discover-status');
                const url = host ? ('/discover?receiver-ip=' + encodeURIComponent(host)) : '/discover';
                status.textContent = host
                    ? 'Searching common receiver endpoints...'
                    : 'Searching the local network for a compatible receiver...';
                fetch(url)
                    .then(response => response.json())
                    .then(data => {
                        if (!data.success) {
                            throw new Error(data.error || 'Unable to find a compatible receiver.');
                        }

                        if (!host && data.receiverIdentity) {
                            document.getElementById('receiver-ip').value = data.receiverIdentity;
                        }
                        document.getElementById('receiver-url').value = data.baseUrl || '';
                        document.getElementById('receiver-url-display').textContent = data.message || ('Detected endpoint: ' + data.baseUrl);
                        status.textContent = data.message || (host ? 'Receiver found.' : 'Receiver found on the local network.');
                    })
                    .catch(error => {
                        clearDiscoveredReceiver();
                        status.textContent = error.message;
                    });
            }

            function triggerReset(endpoint, confirmMessage, busyMessage) {
                const result = document.getElementById('result');
                if (!window.confirm(confirmMessage)) {
                    return;
                }

                result.textContent = busyMessage;
                fetch(endpoint, { method: 'POST' })
                    .then(response => response.text())
                    .then(message => {
                        result.textContent = message;
                    })
                    .catch(error => {
                        result.textContent = error.message;
                    });
            }

            document.getElementById('units').addEventListener('change', updateUnitHints);
            document.getElementById('radius').addEventListener('input', updateUnitHints);
            document.getElementById('receiver-ip').addEventListener('input', clearDiscoveredReceiver);
            document.getElementById('discover-receiver').addEventListener('click', discoverReceiver);
            document.getElementById('sync-time').addEventListener('click', syncTimeNow);
            document.getElementById('wifi-reset').addEventListener('click', () => {
                triggerReset('/reset-wifi', 'Reset Wi-Fi? This will erase saved wireless credentials and reboot the device.', 'Resetting Wi-Fi and rebooting...');
            });
            document.getElementById('factory-reset').addEventListener('click', () => {
                triggerReset('/factory-reset', 'Factory reset? This will erase all saved configuration and Wi-Fi credentials.', 'Erasing all saved configuration and rebooting...');
            });
            document.querySelectorAll('[data-detail-field]').forEach(box => box.addEventListener('change', updateDetailLimit));
            updateUnitHints();
            updateDetailLimit();
            syncSleepTimezoneOffset();
            document.getElementById('setup-url-text').textContent = window.location.origin + '/';
            renderSetupQr();
            loadStatus();
            setInterval(loadStatus, 30000);

            document.getElementById('cfg').addEventListener('submit', function(e) {
                e.preventDefault();
                syncSleepTimezoneOffset();
                fetch(this.action, { method: 'POST', body: new FormData(this) })
                    .then(r => r.text())
                    .then(html => document.getElementById('result').innerHTML = html);
            });
        </script>
    </body>
</html>
)";

void ConfigurationWebServer::Initialise()
{
    // start mDNS and check result
    if (!MDNS.begin("microradar"))
    {
        Serial.println("[WARN] Failed to start mDNS. Continuing without mDNS...");
    }

    server.on("/discover", HTTP_GET, [&](AsyncWebServerRequest *request)
              {
        Serial.println("[GET] Handling receiver discovery request...");

        const String receiverHint = request->hasParam("receiver-ip")
            ? request->getParam("receiver-ip")->value()
            : this->GetStoredString("receiver-ip");

        String discoveryMessage;
        String receiverIdentity;
        const String baseUrl = DiscoverReceiverBaseUrl(receiverHint, discoveryMessage, receiverIdentity);

        JsonDocument doc;
        doc["success"] = !baseUrl.isEmpty();
        if (!baseUrl.isEmpty())
        {
            doc["baseUrl"] = baseUrl;
            doc["receiverIdentity"] = receiverIdentity;
            doc["message"] = discoveryMessage;
        }
        else
        {
            doc["error"] = discoveryMessage;
        }

        String body;
        serializeJson(doc, body);
        request->send(doc["success"].as<bool>() ? 200 : 404, "application/json", body); });

    // Handle visit to config web server
    server.on("/", HTTP_GET, [&](AsyncWebServerRequest *request)
              {
        Serial.println("[GET] Handling request to config web server...");

        // read all values up front so the processor lambda can capture by value
        prefs.begin("config", true);
        const String receiverIp = prefs.getString("receiver-ip", "");
        const String receiverUrl = prefs.getString("receiver-url", "");
        const String units = prefs.getString("units", "aviation");
        const String radiusMiles = prefs.getString("radius", "");
        const String scanlineEnabled = prefs.getString("scanline", "true");
        const String infoTextEnabled = prefs.getString("infotext", "true");
        const String incompleteDataEnabled = prefs.getString("show-incomplete-data", "true");
        const String triangleEnabled = prefs.getString("triangle", "true");
        const String sleepEnabled = prefs.getString("sleep-enabled", "false");
        const String sleepStart = prefs.getString("sleep-start", "22:00");
        const String sleepEnd = prefs.getString("sleep-end", "07:00");
        const String sleepWakeKnob = prefs.getString("sleep-wake-knob", "true");
        const String sleepTimezoneOffset = prefs.getString("sleep-timezone-offset", "");
        const String detailFieldsHtml = BuildDetailFieldsHtml(prefs);
        prefs.end();

        // template processor called once per %PLACEHOLDER% token found in CONFIG_HTML.
        const UnitSystem unitSystem = ParseUnitSystem(units);
        const float storedRadiusMiles = radiusMiles.isEmpty() ? DisplayDistanceToMiles(unitSystem, 25.0f) : ClampMilesRange(radiusMiles.toFloat());
        const int radiusMaxAviation = (int)MilesToDisplayDistance(UnitSystem::Aviation, MAX_RANGE_MILES);
        const int radiusMaxImperial = (int)MilesToDisplayDistance(UnitSystem::Imperial, MAX_RANGE_MILES);
        const int radiusMaxMetric = (int)MilesToDisplayDistance(UnitSystem::Metric, MAX_RANGE_MILES);
        const int radiusMaxCurrent = unitSystem == UnitSystem::Aviation ? radiusMaxAviation : (unitSystem == UnitSystem::Imperial ? radiusMaxImperial : radiusMaxMetric);
        int radiusDisplay = (int)(MilesToDisplayDistance(unitSystem, storedRadiusMiles) + 0.5f);
        if (radiusDisplay < 1) radiusDisplay = 1;
        if (radiusDisplay > radiusMaxCurrent) radiusDisplay = radiusMaxCurrent;
        const String radius = String(radiusDisplay);
        const String receiverUrlText = receiverUrl.isEmpty() ? "No receiver endpoint discovered yet." : String("Detected endpoint: ") + receiverUrl;
        String distancePresetCounts = "null";
        String countsErrorMessage;
        if (!receiverIp.isEmpty() || !receiverUrl.isEmpty())
        {
            if (!LoadDistancePresetCounts(receiverIp, receiverUrl, unitSystem, distancePresetCounts, countsErrorMessage))
            {
                Serial.print("[INFO] Distance preset counts unavailable: ");
                Serial.println(countsErrorMessage);
            }
        }

        AsyncWebServerResponse* response = request->beginResponse(
            200, "text/html",
            (const uint8_t*)CONFIG_HTML, sizeof(CONFIG_HTML) - 1,
            [receiverIp, receiverUrl, receiverUrlText, radius, units, scanlineEnabled, infoTextEnabled, incompleteDataEnabled, triangleEnabled, sleepEnabled, sleepStart, sleepEnd, sleepWakeKnob, sleepTimezoneOffset, detailFieldsHtml, radiusMaxAviation, radiusMaxImperial, radiusMaxMetric, radiusMaxCurrent, distancePresetCounts]
            (const String& var) -> String {
                if (var == "RECEIVER_IP")    return receiverIp;
                if (var == "RECEIVER_URL")   return receiverUrl;
                if (var == "RECEIVER_URL_TEXT") return receiverUrlText;
                if (var == "RADIUS")         return radius;
                if (var == "UNIT_AVIATION")  return units == "aviation" ? "selected" : "";
                if (var == "UNIT_IMPERIAL")  return units == "imperial" ? "selected" : "";
                if (var == "UNIT_METRIC")    return units == "metric" ? "selected" : "";
                if (var == "SCANLINE")       return scanlineEnabled == "true" ? "checked" : "";
                if (var == "INFOTEXT")       return infoTextEnabled == "true" ? "checked" : "";
                if (var == "INCOMPLETE_DATA") return incompleteDataEnabled == "true" ? "checked" : "";
                if (var == "TRIANGLE")       return triangleEnabled == "true" ? "checked" : "";
                if (var == "SLEEP_ENABLED")  return sleepEnabled == "true" ? "checked" : "";
                if (var == "SLEEP_START")    return sleepStart;
                if (var == "SLEEP_END")      return sleepEnd;
                if (var == "SLEEP_WAKE_KNOB") return sleepWakeKnob == "true" ? "checked" : "";
                if (var == "SLEEP_TZ_OFFSET") return sleepTimezoneOffset;
                if (var == "DETAIL_FIELDS")  return detailFieldsHtml;
                if (var == "RADIUS_MAX_AVIATION") return String(radiusMaxAviation);
                if (var == "RADIUS_MAX_IMPERIAL") return String(radiusMaxImperial);
                if (var == "RADIUS_MAX_METRIC") return String(radiusMaxMetric);
                if (var == "RADIUS_MAX") return String(radiusMaxCurrent);
                if (var == "DISTANCE_PRESET_COUNTS") return distancePresetCounts;
                return "";
            }
        );
        request->send(response); });

    server.on("/status", HTTP_GET, [&](AsyncWebServerRequest *request)
              {
        prefs.begin("config", true);
        const String statusJson = BuildStatusJson(prefs);
        prefs.end();
        request->send(200, "application/json", statusJson); });

    server.on("/sync-time", HTTP_POST, [&](AsyncWebServerRequest *request)
              {
        prefs.begin("config", true);
        const int timezoneOffsetMinutes = prefs.getString("sleep-timezone-offset", "0").toInt();
        prefs.end();

        configTime(-(timezoneOffsetMinutes * 60), 0, "pool.ntp.org", "time.nist.gov");

        struct tm timeInfo;
        if (getLocalTime(&timeInfo, 5000))
        {
            char buffer[48];
            if (strftime(buffer, sizeof(buffer), "%a %b %d %Y %I:%M:%S %p", &timeInfo) > 0)
            {
                request->send(200, "text/plain", String("Time synced: ") + buffer + " (" + FormatUtcOffsetLabel(timezoneOffsetMinutes) + ")");
                return;
            }
        }

        request->send(500, "text/plain", "Unable to sync time right now."); });

    // Handle save submission to web server
    server.on("/save", HTTP_POST, [&](AsyncWebServerRequest *request)
              {
        Serial.println("[POST] Handling form submission to config web server...");

        const auto* unitsParam = request->getParam("units", true);
        const String units = unitsParam == nullptr ? "aviation" : unitsParam->value();
        const UnitSystem unitSystem = ParseUnitSystem(units);
        const int radiusMaxCurrent = (int)MilesToDisplayDistance(unitSystem, MAX_RANGE_MILES);
        const auto* sleepStartParam = request->getParam("sleep-start", true);
        const auto* sleepEndParam = request->getParam("sleep-end", true);
        const auto* sleepTimezoneOffsetParam = request->getParam("sleep-timezone-offset", true);
        const bool sleepEnabled = request->hasParam("sleep-enabled", true);
        const bool sleepWakeKnob = request->hasParam("sleep-wake-knob", true);

        if (sleepEnabled)
        {
            const auto parseClock = [](const String &value, int &minutes) -> bool
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
            };

            int ignoredMinutes = 0;
            if (sleepStartParam == nullptr || !parseClock(sleepStartParam->value(), ignoredMinutes) ||
                sleepEndParam == nullptr || !parseClock(sleepEndParam->value(), ignoredMinutes))
            {
                request->send(400, "text/html", "Sleep hours must use HH:MM format.");
                return;
            }
        }

        const auto* radiusValidationParam = request->getParam("radius", true);
        if (radiusValidationParam != nullptr)
        {
            const float displayRadius = radiusValidationParam->value().toFloat();
            const float radiusMiles = DisplayDistanceToMiles(unitSystem, displayRadius);
            const bool wholeNumber = displayRadius == (float)((int)displayRadius);
            if (!wholeNumber || displayRadius < 1.0f || displayRadius > radiusMaxCurrent || radiusMiles < MIN_RANGE_MILES || radiusMiles > MAX_RANGE_MILES)
            {
                request->send(400, "text/html", "Distance must be a whole number between 1 and 500 miles.");
                return;
            }
        }

        prefs.begin("config", false);

        const auto* receiverIpParam = request->getParam("receiver-ip", true);
        if (receiverIpParam != nullptr)
            prefs.putString("receiver-ip", receiverIpParam->value());

        const auto* receiverUrlParam = request->getParam("receiver-url", true);
        if (receiverUrlParam != nullptr)
            prefs.putString("receiver-url", receiverUrlParam->value());

        prefs.putString("units", units);

        const auto* radiusParam = request->getParam("radius", true);
        if (radiusParam != nullptr)
        {
            const float radiusMiles = DisplayDistanceToMiles(unitSystem, radiusParam->value().toFloat());
            prefs.putString("radius", String(radiusMiles, 4));
        }

        prefs.putString("scanline", request->hasParam("scanline", true) ? "true" : "false");
        prefs.putString("triangle", request->hasParam("triangle", true) ? "true" : "false");
        prefs.putString("infotext", request->hasParam("infotext", true) ? "true" : "false");
        prefs.putString("show-incomplete-data", request->hasParam("show-incomplete-data", true) ? "true" : "false");
        prefs.putString("sleep-enabled", sleepEnabled ? "true" : "false");
        prefs.putString("sleep-wake-knob", sleepWakeKnob ? "true" : "false");
        if (sleepStartParam != nullptr)
            prefs.putString("sleep-start", sleepStartParam->value());
        if (sleepEndParam != nullptr)
            prefs.putString("sleep-end", sleepEndParam->value());
        if (sleepTimezoneOffsetParam != nullptr)
            prefs.putString("sleep-timezone-offset", sleepTimezoneOffsetParam->value());

        size_t selectedDetailFields = 0;
        for (size_t i = 0; i < AIRCRAFT_DETAIL_FIELD_COUNT; ++i)
        {
            const auto &spec = GetAircraftDetailFieldSpec(i);
            const bool selected = request->hasParam(spec.key, true);
            if (selected && selectedDetailFields < MAX_SELECTED_DETAIL_FIELDS)
            {
                prefs.putString(spec.key, "true");
                ++selectedDetailFields;
            }
            else
            {
                prefs.putString(spec.key, "false");
            }
        }

        prefs.end();

        request->send(200, "text/html", "Saved - restarting device...");
        delay(500);
        ESP.restart(); });

    server.on("/reset-wifi", HTTP_POST, [&](AsyncWebServerRequest *request)
              {
        Serial.println("[POST] Handling Wi-Fi reset request...");
        WiFi.disconnect(true, true);
        request->send(200, "text/plain", "Wi-Fi credentials cleared. Rebooting into setup mode...");
        delay(500);
        ESP.restart(); });

    server.on("/factory-reset", HTTP_POST, [&](AsyncWebServerRequest *request)
              {
        Serial.println("[POST] Handling factory reset request...");
        prefs.begin("config", false);
        prefs.clear();
        prefs.end();
        WiFi.disconnect(true, true);
        request->send(200, "text/plain", "All configuration cleared. Rebooting into setup mode...");
        delay(500);
        ESP.restart(); });

    server.begin();
}

const String ConfigurationWebServer::GetStoredString(const char *key)
{
    if (!prefs.begin("config", false))
    {
        return "";
    }

    const String value = prefs.getString(key, "");
    prefs.end();
    return value;
}

void ConfigurationWebServer::SetStoredString(const char* key, const String& value)
{
    if (!prefs.begin("config", false))
    {
        return;
    }

    prefs.putString(key, value);
    prefs.end();
}
