#pragma once

#include <Arduino.h>

enum class UnitSystem
{
    Aviation,
    Imperial,
    Metric
};

inline UnitSystem ParseUnitSystem(const String &value)
{
    if (value == "imperial")
        return UnitSystem::Imperial;
    if (value == "metric")
        return UnitSystem::Metric;
    return UnitSystem::Aviation;
}

inline const char *UnitSystemValue(UnitSystem unitSystem)
{
    switch (unitSystem)
    {
    case UnitSystem::Imperial:
        return "imperial";
    case UnitSystem::Metric:
        return "metric";
    case UnitSystem::Aviation:
    default:
        return "aviation";
    }
}

inline const char *UnitSystemDescription(UnitSystem unitSystem)
{
    switch (unitSystem)
    {
    case UnitSystem::Imperial:
        return "Imperial - feet, mph, statute miles";
    case UnitSystem::Metric:
        return "Metric - meters, km/h, kilometers";
    case UnitSystem::Aviation:
    default:
        return "Aviation - feet, knots, nautical miles";
    }
}

inline const char *DistanceUnitLabel(UnitSystem unitSystem)
{
    switch (unitSystem)
    {
    case UnitSystem::Imperial:
        return "statute miles";
    case UnitSystem::Metric:
        return "kilometers";
    case UnitSystem::Aviation:
    default:
        return "nautical miles";
    }
}

inline const char *AltitudeUnitLabel(UnitSystem unitSystem)
{
    switch (unitSystem)
    {
    case UnitSystem::Metric:
        return "m";
    case UnitSystem::Imperial:
    case UnitSystem::Aviation:
    default:
        return "ft";
    }
}

inline const char *SpeedUnitLabel(UnitSystem unitSystem)
{
    switch (unitSystem)
    {
    case UnitSystem::Imperial:
        return "mph";
    case UnitSystem::Metric:
        return "km/h";
    case UnitSystem::Aviation:
    default:
        return "kt";
    }
}

inline constexpr float MIN_RANGE_MILES = 1.0f;
inline constexpr float MAX_RANGE_MILES = 500.0f;

inline float ClampMilesRange(float miles)
{
    if (miles < MIN_RANGE_MILES)
        return MIN_RANGE_MILES;
    if (miles > MAX_RANGE_MILES)
        return MAX_RANGE_MILES;
    return miles;
}

inline float MilesToDisplayDistance(UnitSystem unitSystem, float miles)
{
    switch (unitSystem)
    {
    case UnitSystem::Imperial:
        return miles;
    case UnitSystem::Metric:
        return miles * 1.609344f;
    case UnitSystem::Aviation:
    default:
        return miles / 1.150779f;
    }
}

inline float DisplayDistanceToMiles(UnitSystem unitSystem, float displayDistance)
{
    switch (unitSystem)
    {
    case UnitSystem::Imperial:
        return displayDistance;
    case UnitSystem::Metric:
        return displayDistance / 1.609344f;
    case UnitSystem::Aviation:
    default:
        return displayDistance * 1.150779f;
    }
}

inline float MetersToDisplayAltitude(UnitSystem unitSystem, float meters)
{
    switch (unitSystem)
    {
    case UnitSystem::Metric:
        return meters;
    case UnitSystem::Imperial:
    case UnitSystem::Aviation:
    default:
        return meters * 3.28084f;
    }
}

inline float MpsToDisplaySpeed(UnitSystem unitSystem, float mps)
{
    switch (unitSystem)
    {
    case UnitSystem::Imperial:
        return mps * 2.23694f;
    case UnitSystem::Metric:
        return mps * 3.6f;
    case UnitSystem::Aviation:
    default:
        return mps * 1.94384f;
    }
}
