#include "network_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "core/pin.h"
#include "core/secrets.h"
#include "queue_manager.h"

namespace
{
    // ---- Configuration -------------------------------------------------------
    const char *mqtt_broker = "broker.emqx.io";
    constexpr uint16_t mqtt_port = 1883;
    const char *topic_sub_command = "esp32/test/rx/command";
    const char *topic_pub_sensor = "esp32/test/tx/sensor";
    const char *topic_pub_actuator = "esp32/test/tx/actuator";

    constexpr uint32_t WIFI_RETRY_MS = 30000; // safety net if the driver gives up
    constexpr uint32_t MQTT_BACKOFF_MIN_MS = 1000;
    constexpr uint32_t MQTT_BACKOFF_MAX_MS = 30000;

    // ---- State (touched only by the network task after begin()) --------------
    enum class link_state_t : uint8_t
    {
        WIFI_CONNECTING,
        MQTT_CONNECTING,
        CONNECTED,
    };

    WiFiClient wifi_client;
    PubSubClient client(wifi_client);
    EventGroupHandle_t net_events = nullptr;

    char client_id[32];
    link_state_t state = link_state_t::WIFI_CONNECTING;
    bool state_valid = false; // false until the first set_state()
    bool wifi_was_up = false;
    uint32_t wifi_attempt_ms = 0;
    uint32_t mqtt_next_try_ms = 0;
    uint32_t mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;

    // Only acts on a change, so the RGB LED / event bits aren't rewritten every loop.
    void set_state(link_state_t new_state)
    {
        if (state_valid && new_state == state)
            return;
        state = new_state;
        state_valid = true;

        switch (state)
        {
        case link_state_t::WIFI_CONNECTING:
            neopixelWrite(pin::BUILTIN_RGB_PIN, 0, 0, 50); // blue
            xEventGroupClearBits(net_events, network_manager::BIT_MQTT_CONNECTED);
            Serial.println("[NET] WiFi connecting...");
            break;
        case link_state_t::MQTT_CONNECTING:
            neopixelWrite(pin::BUILTIN_RGB_PIN, 50, 0, 0); // red
            xEventGroupClearBits(net_events, network_manager::BIT_MQTT_CONNECTED);
            break;
        case link_state_t::CONNECTED:
            neopixelWrite(pin::BUILTIN_RGB_PIN, 0, 50, 0); // green
            xEventGroupSetBits(net_events, network_manager::BIT_MQTT_CONNECTED);
            break;
        }
    }

    void start_wifi()
    {
        WiFi.begin(secrets::WIFI_SSID, secrets::WIFI_PASSWORD);
        wifi_attempt_ms = millis();
    }

    void try_connect_mqtt()
    {
        Serial.print("[MQTT] Connecting... ");
        if (client.connect(client_id))
        {
            Serial.println("connected");
            if (!client.subscribe(topic_sub_command))
                Serial.println("[MQTT] Subscribe failed");
            mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;
        }
        else
        {
            Serial.printf("failed, rc=%d, retry in %lu ms\n", client.state(), (unsigned long)mqtt_backoff_ms);
            mqtt_next_try_ms = millis() + mqtt_backoff_ms;
            const uint32_t doubled = mqtt_backoff_ms * 2;
            mqtt_backoff_ms = (doubled > MQTT_BACKOFF_MAX_MS) ? MQTT_BACKOFF_MAX_MS : doubled;
        }
    }

    // Runs inside client.loop(), i.e. in the network task. Keep it short:
    // parse, validate, enqueue, return.
    void callback(char *topic, byte *payload, unsigned int length)
    {
        if (strcmp(topic, topic_sub_command) != 0)
            return;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload, length);
        if (err)
        {
            Serial.print("[MQTT] Deserialize json failed : ");
            Serial.println(err.c_str());
            return;
        }

        command_t cmd;
        cmd.temperature_celcius = doc["temperature_celcius"] | 0.0f;
        cmd.timer_minutes = doc["timer_minutes"] | 0;
        cmd.command = doc["command"] | -1;

        if (cmd.command != 0 && cmd.command != 1)
        {
            Serial.println("[MQTT] Unknown command value");
            return;
        }
        if (!queue_manager::enqueue_command_data(cmd))
            Serial.println("[MQTT] Command queue full, command dropped");
    }

    bool publish_text(const char *topic, const char *payload)
    {
        const bool ok = client.publish(topic, payload);
        Serial.printf("[MQTT] %s %s -> %s\n", ok ? "published" : "FAILED", topic, payload);
        return ok;
    }
} // namespace

namespace network_manager
{
    void begin()
    {
        net_events = xEventGroupCreate();

        // Unique client ID: a fixed ID on a public broker gets kicked by anyone else using it.
        const uint64_t mac = ESP.getEfuseMac();
        snprintf(client_id, sizeof(client_id), "esp32s3-%04X%08X",
                 (unsigned)((mac >> 32) & 0xFFFF), (unsigned)(mac & 0xFFFFFFFF));

        client.setServer(mqtt_broker, mqtt_port);
        client.setCallback(callback);
        client.setSocketTimeout(3); // seconds; keeps a failed connect() short

        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true); // the driver handles ordinary drops by itself
        start_wifi();
    }

    void service()
    {
        const uint32_t now = millis();

        if (WiFi.status() != WL_CONNECTED)
        {
            wifi_was_up = false;
            set_state(link_state_t::WIFI_CONNECTING);
            if (now - wifi_attempt_ms >= WIFI_RETRY_MS)
            {
                Serial.println("[NET] WiFi still down, forcing reconnect");
                WiFi.reconnect();
                wifi_attempt_ms = now;
            }
            return;
        }

        if (!wifi_was_up)
        {
            wifi_was_up = true;
            mqtt_next_try_ms = now; // try MQTT immediately after WiFi (re)appears
            mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;
            Serial.print("[NET] WiFi connected, IP: ");
            Serial.println(WiFi.localIP());
        }

        if (client.connected())
        {
            set_state(link_state_t::CONNECTED);
            return;
        }

        set_state(link_state_t::MQTT_CONNECTING);
        if ((int32_t)(now - mqtt_next_try_ms) >= 0) // wrap-safe "is it time yet"
            try_connect_mqtt();
    }

    bool is_connected()
    {
        return WiFi.status() == WL_CONNECTED && client.connected();
    }

    bool publish(const outbound_msg_t &msg)
    {
        if (!client.connected())
            return false;

        char buf[128];
        switch (msg.type)
        {
        case msg_type_t::SENSOR:
            snprintf(buf, sizeof(buf), "{\"temperature_c\": %.2f, \"humidity_pct\": %.2f}",
                     msg.sensor.temperature_celcius, msg.sensor.humidity_percent);
            return publish_text(topic_pub_sensor, buf);

        case msg_type_t::ACTUATOR:
            snprintf(buf, sizeof(buf), "{\"motor_rpm\": %.2f, \"servo_deg\": %d}",
                     msg.actuator.motor_speed_rpm, msg.actuator.servo_degree);
            return publish_text(topic_pub_actuator, buf);
        }
        return false;
    }

    void mqtt_loop()
    {
        client.loop();
    }

    EventGroupHandle_t event_group()
    {
        return net_events;
    }
} // namespace network_manager