#include <Arduino.h>
#include "core/pin.h"
#include "managers/network_manager.h"
#include "managers/queue_manager.h"

// The ONLY task that touches WiFi / PubSubClient.
void networkTask(void *pv)
{
  Serial.printf("[RTOS] Core %d - Network Task Started.\n", xPortGetCoreID());
  outbound_msg_t msg;
  for (;;)
  {
    network_manager::service(); // non-blocking connect / reconnect state machine

    if (network_manager::is_connected())
    {
      // Sleep until a message arrives, but wake every 20 ms so mqtt_loop() keeps running.
      if (queue_manager::dequeue_outbound(msg, pdMS_TO_TICKS(20)))
        network_manager::publish(msg);
    }
    else
    {
      // Offline: leave messages in the queue (it keeps the newest ones).
      vTaskDelay(pdMS_TO_TICKS(20));
    }

    network_manager::mqtt_loop();
  }
}

void commandReactTask(void *pv)
{
  command_t command_data;
  for (;;)
  {
    // Blocks until a command arrives, no polling or extra delay needed.
    if (queue_manager::dequeue_command_data(command_data, portMAX_DELAY))
    {
      Serial.printf("[CORE-%d] (command, temp_c, timer_min) = (%d, %.2f, %d)\n",
                    xPortGetCoreID(), command_data.command,
                    command_data.temperature_celcius, command_data.timer_minutes);
    }
  }
}

void blinkLedManagementTask(void *pv)
{
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xPeriod = pdMS_TO_TICKS(2000);
  int last_led_state = LOW;
  const float temps[5] = {10.4, 20.5, 30.6, 40.5, 50.6};
  const float humids[5] = {40.4, 50.5, 60.6, 70.5, 80.6};
  const float motor_rpms[5] = {120.4, 110.5, 123.6, 120.5, 121.6};
  const int servo_degs[5] = {30, 45, 60, 75, 90};
  int idx = 0;
  sensor_t sensor_data;
  actuator_t actuator_data;
  pinMode(pin::RED_LED_PIN, OUTPUT);
  Serial.printf("[RTOS] Core %d - Blink/Producer Task Started.\n", xPortGetCoreID());
  for (;;)
  {
    last_led_state = !last_led_state;
    digitalWrite(pin::RED_LED_PIN, last_led_state);

    sensor_data.humidity_percent = humids[idx];
    sensor_data.temperature_celcius = temps[idx];
    actuator_data.motor_speed_rpm = motor_rpms[idx];
    actuator_data.servo_degree = servo_degs[idx];
    idx = (idx + 1) % 5;

    queue_manager::enqueue_sensor_data(sensor_data);
    queue_manager::enqueue_actuator_data(actuator_data);
    vTaskDelayUntil(&xLastWakeTime, xPeriod);
  }
}

void setup()
{
  Serial.begin(115200);
  queue_manager::initialize();
  network_manager::begin(); // before the tasks start, so the event group exists

  xTaskCreatePinnedToCore(networkTask, "NetworkTask", 6144, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(blinkLedManagementTask, "BlinkLedTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(commandReactTask, "CommandReactTask", 4096, NULL, 1, NULL, 1);
}

void loop()
{
  vTaskDelete(NULL); // everything runs in the tasks
}