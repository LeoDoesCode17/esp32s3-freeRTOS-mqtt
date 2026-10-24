#pragma once
#include <stdint.h>

typedef struct
{
    float temperature_celcius;
    float humidity_percent;
} sensor_t;

typedef struct
{
    float motor_speed_rpm;
    int servo_degree;
} actuator_t;

typedef struct
{
    float temperature_celcius;
    int timer_minutes;
    int command;
} command_t;

// ---- Outbound (device -> broker) -------------------------------------------
// One tag per kind of data. To publish to a new topic:
//   1. add a value here        2. add a payload struct + union member below
//   3. add a case in network_manager::publish()
enum class msg_type_t : uint8_t
{
    SENSOR,
    ACTUATOR,
};

// Copied into the FreeRTOS queue with memcpy, so members must be plain data
// (no String / std::string / pointers to stack memory).
typedef struct
{
    msg_type_t type;
    union
    {
        sensor_t sensor;
        actuator_t actuator;
    };
} outbound_msg_t;