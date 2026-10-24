#pragma once
#include <Arduino.h>
#include "core/type.h"

namespace queue_manager
{
    void initialize();

    // ---- Outbound: any producer task -> network task -------------------------
    // Never blocks. If the queue is full the OLDEST item is dropped, so the
    // queue always holds the freshest data (good for telemetry).
    bool enqueue_outbound(const outbound_msg_t &msg);
    // Used by the network task only.
    bool dequeue_outbound(outbound_msg_t &msg, TickType_t timeout);

    // Convenience wrappers so producers don't build outbound_msg_t by hand.
    bool enqueue_sensor_data(const sensor_t &data);
    bool enqueue_actuator_data(const actuator_t &data);

    // ---- Inbound: network task (MQTT callback) -> command task ---------------
    bool enqueue_command_data(const command_t &cmd); // never blocks
    bool dequeue_command_data(command_t &cmd, TickType_t timeout);
} // namespace queue_manager