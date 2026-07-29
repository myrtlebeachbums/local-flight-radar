#pragma once

#include <ESPAsyncWebServer.h>
#include <Preferences.h>

class ConfigurationWebServer {
private:
    AsyncWebServer server;
    Preferences prefs;

public:
    ConfigurationWebServer() : server(8080), prefs() {}
    ConfigurationWebServer(int port) : server(port), prefs() {}

    void Initialise();
    [[nodiscard]] const String GetStoredString(const char* key);
    void SetStoredString(const char* key, const String& value);
};
