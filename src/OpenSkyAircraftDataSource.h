#pragma once

#include <vector>

#include "AircraftDataSource.h"
#include "ConfigurationWebServer.h"
#include "HttpRequestManager.h"
#include "OpenSkyAuthTokenHandler.h"

class OpenSkyAircraftDataSource : public AircraftDataSource
{
private:
    ConfigurationWebServer& configServer;
    OpenSkyAuthTokenHandler& authHandler;
    HttpRequestManager& http;

    unsigned long fetchInterval = 0;

public:
    OpenSkyAircraftDataSource(
        ConfigurationWebServer& config,
        OpenSkyAuthTokenHandler& auth,
        HttpRequestManager& httpManager)
        : configServer(config), authHandler(auth), http(httpManager)
    {
    }

    ~OpenSkyAircraftDataSource() override = default;

    void Initialise() override;
    [[nodiscard]] unsigned long GetFetchIntervalMs() const override;
    bool Fetch(std::vector<Aircraft>& aircraft) override;
};
