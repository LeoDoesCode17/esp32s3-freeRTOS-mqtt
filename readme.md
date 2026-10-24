# ESP32-S3 FreeRTOS + MQTT

An ESP32-S3 project that publishes telemetry to an MQTT broker and reacts to commands coming back from it, using FreeRTOS tasks and queues on both cores.

The design goal is simple: **exactly one task owns the MQTT client**. Every other task talks to it through queues, so `PubSubClient` (which is not thread-safe) is never touched from two tasks, and no mutex is needed.

## Features

- Dual-core FreeRTOS task layout (network on core 0, application tasks on core 1)
- Non-blocking WiFi and MQTT connect/reconnect state machine with exponential backoff
- Single outbound queue for all published data, tagged by message type
- Drop-oldest policy when the queue is full, so the freshest data survives outages
- Messages stay queued while offline and are flushed once the broker is back
- Short MQTT callback: parse, validate, enqueue, return
- RGB LED status: blue = WiFi connecting, red = MQTT connecting, green = connected
- Unique MQTT client ID derived from the chip MAC address

## Hardware

| Item | Detail |
|------|--------|
| Board | ESP32-S3 DevKitM-1 (`esp32-s3-devkitm-1`) |
| Status RGB LED | GPIO 48 (on-board) |
| Red LED | GPIO 14 |

Pins are defined in `src/core/pin.h`.

## Architecture

```
 core 1                                              core 0
┌──────────────────────┐                      ┌────────────────────────┐
│ blinkLedManagement   │                      │ networkTask            │
│ (producer, 2 s)      │── outbound_queue ───▶│  service()  WiFi/MQTT  │──▶ broker
└──────────────────────┘                      │  publish()             │     (sensor,
                                              │  mqtt_loop()           │      actuator)
┌──────────────────────┐                      │                        │
│ commandReactTask     │◀─ command_queue ─────│  MQTT callback         │◀── broker
│ (blocks on queue)    │                      └────────────────────────┘     (command)
└──────────────────────┘
```

| Task | Core | Role |
|------|------|------|
| `networkTask` | 0 | The only task that touches WiFi and `PubSubClient`: connection state machine, publishing, `client.loop()` |
| `blinkLedManagementTask` | 1 | Blinks the red LED every 2 s and produces sensor and actuator data (demo values) |
| `commandReactTask` | 1 | Blocks on `command_queue` and handles incoming commands (currently prints them) |

### Rules that keep it safe

1. `network_manager::begin()` runs once in `setup()`, before any task starts.
2. After that, `service()`, `is_connected()`, `publish()` and `mqtt_loop()` are called **only** from `networkTask`.
3. Other tasks publish by calling `queue_manager::enqueue_*()`, which never blocks.
4. Queue items are copied with `memcpy`, so message structs must be plain data (no `String`, no `std::string`).

## Project structure

```
src/
├── main.cpp                      Tasks and setup()
├── core/
│   ├── pin.h                     Pin assignments
│   ├── secrets.h                 WiFi credentials (git-ignored)
│   └── type.h                    sensor_t, actuator_t, command_t, outbound_msg_t
└── managers/
    ├── network_manager.h/.cpp    WiFi + MQTT state machine, publish(), callback
    └── queue_manager.h/.cpp      outbound and command queues
```

## Getting started

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- An ESP32-S3 DevKitM-1 (or edit `platformio.ini` for your board)

### Configure

1. Edit `src/core/secrets.h` and set your WiFi credentials:

   ```cpp
   constexpr const char *WIFI_SSID = "YOUR_WIFI_SSID";
   constexpr const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   ```

2. Add the file to `.gitignore` so credentials are never committed:

   ```
   src/core/secrets.h
   ```

3. (Optional) Change the broker and topics at the top of `src/managers/network_manager.cpp`.

### Build, upload and monitor

```bash
pio run -t upload
pio device monitor
```

The serial monitor runs at 115200 baud. Libraries (`PubSubClient`, `ArduinoJson`) are installed automatically from `platformio.ini`.

## MQTT interface

Default broker: `broker.emqx.io:1883` (a **public** broker, so anyone can read and publish to your topics; use a unique topic prefix or your own broker for anything real).

| Direction | Topic | Payload |
|-----------|-------|---------|
| Publish | `esp32/test/tx/sensor` | `{"temperature_c": 10.40, "humidity_pct": 40.40}` |
| Publish | `esp32/test/tx/actuator` | `{"motor_rpm": 120.40, "servo_deg": 30}` |
| Subscribe | `esp32/test/rx/command` | `{"command": 1, "temperature_celcius": 60.0, "timer_minutes": 15}` |

`command` must be `0` or `1`; other values are rejected. Missing fields default to `0`.

Try it from a terminal:

```bash
# watch what the device publishes
mosquitto_sub -h broker.emqx.io -t "esp32/test/tx/#" -v

# send a command
mosquitto_pub -h broker.emqx.io -t "esp32/test/rx/command" \
  -m '{"command": 1, "temperature_celcius": 60.0, "timer_minutes": 15}'
```

## Extending

### Publish to a new topic

1. **`type.h`**: add a value to `msg_type_t`, a payload struct, and a member in the `outbound_msg_t` union.
2. **`network_manager.cpp`**: add a topic constant and a `case` in `publish()` that formats the payload.
3. **Any producer task**: build an `outbound_msg_t` and call `queue_manager::enqueue_outbound()` (or add a small wrapper next to `enqueue_sensor_data()`).

No new task and no new queue are needed.

### Handle a new command

Extend `command_t` in `type.h`, parse the new fields in the `callback()` in `network_manager.cpp`, and act on them in `commandReactTask`.

### Wait for connectivity from another task

```cpp
xEventGroupWaitBits(network_manager::event_group(),
                    network_manager::BIT_MQTT_CONNECTED,
                    pdFALSE, pdTRUE, portMAX_DELAY);
```

## Behavior notes and limitations

- **Offline behavior:** while WiFi or MQTT is down, the network task stops dequeuing. The outbound queue (10 slots) fills, then drops the oldest message each time a new one arrives.
- **QoS:** `PubSubClient` publishes at QoS 0 only. If you need QoS 1, consider ESP-IDF's `esp-mqtt` or the `espMqttClient` library.
- **Lost on drop:** a message dequeued at the exact moment the connection drops is lost.
- **Packet size:** `PubSubClient` defaults to a 256-byte packet buffer; payloads here are well under that.
- **Stack sizes:** the network task has a 6144-byte stack. Check `uxTaskGetStackHighWaterMark(NULL)` if you add more work to a task.
- **Core pinning:** pinning is a choice, not a requirement. Unpinned `xTaskCreate` also works for this workload.

## License

Not added yet.