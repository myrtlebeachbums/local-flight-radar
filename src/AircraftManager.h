#pragma once

#include <map>
#include <vector>

#include "models/TrackedAircraft.h"
#include "ConfigurationWebServer.h"
#include "AircraftDataSource.h"
#include "LGFX.h"

class AircraftManager
{
private:
    double lat = 0.0;
    double lon = 0.0;
    double rad = 0.2;
    std::map<String, TrackedAircraft> trackedAircraft;

    bool displayInfoText = true;
    bool displayTriangles = true;

    unsigned long fetchInterval = 0;
    unsigned long lastFetch = 999999;

    ConfigurationWebServer &configServer;
    AircraftDataSource &dataSource;
    LGFX &tft;

    void DrawRadarCircles(LGFX_Sprite &backbuffer) const;
    std::pair<int, int> ProjectCoordinateToScreen(float predLat, float predLon) const;
    void DrawAircraftInfo(LGFX_Sprite &backbuffer, int x, int y, const TrackedAircraft &tracked) const;
    void DrawAircraftTriangle(LGFX_Sprite &backbuffer, int x, int y, const TrackedAircraft &tracked, bool selected) const;
    void DrawAircraftTriangle(LGFX_Sprite &backbuffer, int x, int y, const TrackedAircraft &tracked) const;

public:
    AircraftManager(ConfigurationWebServer &config, AircraftDataSource &source, LGFX &tftGfx)
        : configServer(config), dataSource(source), tft(tftGfx)
    {
    }
    ~AircraftManager() = default;

    void Initialise();
    void Update();
    void Draw(LGFX_Sprite &backbuffer);
    uint32_t GetAircraftColour(const TrackedAircraft &tracked) const;
    void SelectNextAircraft();
    void SelectPreviousAircraft();
    void DrawDetails(LGFX_Sprite &backbuffer);
    void EncoderClick();
};
