#pragma once

#include <Arduino.h>

enum class AircraftDetailField
{
    Altitude,
    Speed,
    Heading,
    Distance,
    Squawk,
    Icao24,
    MessageAge
};

struct AircraftDetailFieldSpec
{
    AircraftDetailField field;
    const char *key;
    const char *label;
    bool defaultEnabled;
};

inline constexpr size_t MAX_SELECTED_DETAIL_FIELDS = 5;

inline constexpr AircraftDetailFieldSpec AIRCRAFT_DETAIL_FIELDS[] = {
    {AircraftDetailField::Altitude, "detail-altitude", "Altitude", true},
    {AircraftDetailField::Speed, "detail-speed", "Speed", true},
    {AircraftDetailField::Heading, "detail-heading", "Heading", true},
    {AircraftDetailField::Distance, "detail-distance", "Distance", true},
    {AircraftDetailField::Squawk, "detail-squawk", "Squawk", false},
    {AircraftDetailField::Icao24, "detail-icao24", "ICAO address", false},
    {AircraftDetailField::MessageAge, "detail-msgage", "Message age", false},
};

inline constexpr size_t AIRCRAFT_DETAIL_FIELD_COUNT =
    sizeof(AIRCRAFT_DETAIL_FIELDS) / sizeof(AIRCRAFT_DETAIL_FIELDS[0]);

inline const AircraftDetailFieldSpec &GetAircraftDetailFieldSpec(size_t index)
{
    return AIRCRAFT_DETAIL_FIELDS[index];
}
