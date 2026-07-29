#pragma once

#include <vector>

#include "models/Aircraft.h"

class AircraftDataSource
{
public:
    virtual ~AircraftDataSource() = default;

    virtual void Initialise() = 0;
    virtual unsigned long GetFetchIntervalMs() const = 0;
    virtual bool Fetch(std::vector<Aircraft>& aircraft) = 0;
};
