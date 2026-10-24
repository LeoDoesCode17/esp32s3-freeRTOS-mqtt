#pragma once
#include "core/type.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// OWNERSHIP RULE: begin() runs once from setup(), before any task starts.
// After that, service() / is_connected() / publish() / mqtt_loop() must be
// called ONLY from the network task. That is what makes PubSubClient safe
// without a mutex. Other tasks talk to it through queue_manager.
namespace network_manager
{
    constexpr EventBits_t BIT_MQTT_CONNECTED = (1 << 0);

    void begin();                       // config + start WiFi (non-blocking)
    void service();                     // non-blocking WiFi/MQTT state machine
    bool is_connected();                // WiFi up AND MQTT connected
    bool publish(const outbound_msg_t &msg);
    void mqtt_loop();                   // PubSubClient keepalive + incoming messages

    // Safe to use from any task: wait on / test BIT_MQTT_CONNECTED.
    EventGroupHandle_t event_group();
} // namespace network_manager