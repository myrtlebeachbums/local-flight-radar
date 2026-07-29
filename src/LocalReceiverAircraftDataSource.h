#pragma once

#include <vector>

#include "AircraftDataSource.h"
#include "ConfigurationWebServer.h"
#include "HttpRequestManager.h"

class LocalReceiverAircraftDataSource : public AircraftDataSource
{
private:
    ConfigurationWebServer& configServer;
    HttpRequestManager& http;

    String baseUrl;
    unsigned long fetchInterval = 1000;

    static String NormalizeBaseUrl(String url);
    static bool ParseAircraftObject(const JsonVariantConst& item, Aircraft& aircraft);

public:
    LocalReceiverAircraftDataSource(
        ConfigurationWebServer& config,
        HttpRequestManager& httpManager)
        : configServer(config), http(httpManager)
    {
    }

    ~LocalReceiverAircraftDataSource() override = default;

    void Initialise() override;
    [[nodiscard]] unsigned long GetFetchIntervalMs() const override;
    bool Fetch(std::vector<Aircraft>& aircraft) override;
};
