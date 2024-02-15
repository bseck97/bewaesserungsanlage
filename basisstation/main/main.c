#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
// WIFI
#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
// NTP Time
#include "esp_sntp.h"
#include "time.h"
// timer
#include "esp_timer.h"
// MQTT
#include "cJSON.h"
#include "mqtt_client.h"
// steinhart-hart
#include <math.h>

#define TAG "ESP32_BASIS"

/* MQTT */
#define MQTT_URI "mqtt://MQTT_HOST_IP"
// topics
#define SENSOR_DATA "/topic/sensor"
#define DEBUG_MSG "/topic/debug"
#define THRESHOLD_TOPIC "/topic/threshold"
#define SENSOR_COUNT_TOPIC "/topic/sensor_count"
#define SENSOR_THRESHOLD_TOPIC "/topic/sensor_threshold"
#define PUMP_ON_TIME_TOPIC "/topic/pump_on_time"
#define PUMP_ON_TOPIC "/topic/pump_on"
#define SEND_ID_DELAY_TOPIC "/topic/send_id_delay"
#define TX_TASK_DELAY_TOPIC "/topic/tx_task_delay"
#define SENSOR_DATA_TOPIC "/topic/sensor_data"

esp_mqtt_client_handle_t client;
bool mqtt_connected = false; // flag to check if mqtt is connected

/* WIFI */
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

static EventGroupHandle_t
    s_wifi_event_group; // FreeRTOS event group to signal when we are connected
static int s_retry_num = 0;     // number of retries to connect to wifi
#define WIFI_CONNECTED_BIT BIT0 // connected to the AP with an IP
#define WIFI_FAIL_BIT                                                          \
  BIT1 // failed to connect after the maximum amount of retries

/* SD CARD */
#define MOUNT_POINT "/sdcard"
#define FILE_NAME MOUNT_POINT "/data.txt"

#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 5

/* UART */
#define TXD 17
#define RXD 16
#define RTS 4
#define CTS -1 // CTS is not used in RS485 Half-Duplex Mode

#define DATA_LENGTH 20
#define RX_BUF_SIZE (127)
#define BAUD_RATE 115200
#define UART_PORT 2
// Read packet timeout
#define PACKET_READ_TICS (1000 / portTICK_PERIOD_MS)
// Timeout threshold for UART = number of symbols (~10 tics) with unchanged
// state on receive pin
#define READ_TOUT (3) // 3.5T * 8 = 28 ticks, TOUT=3 -> ~24..33 ticks

/* CONSTANTS AND OTHER CONFIG DATA */
#define LOG_TO_SD_CARD 1 // 1 to log data to SD card, 0 to disable
#define INIT_WIFI 1      // 1 to initialize wifi, 0 to disable
#define STACK_SIZE (2048)
#define REFERENCE_VOLTAGE 3.3
#define MAX_SENSORS 5
#define GPIO_PUMP 21           // GPIO to control relais
#define SECOND_US 1000000      // in microseconds
#define MINUTE_US 60 * 1000000 // in microseconds
#define MINUTE_MS 60 * 1000    // in milliseconds

// config vars, can be modified over MQTT
typedef struct {
  double threshold;
  int sensor_threshold;
  int sensor_count;
  int pump_on_time;
  int send_id_delay;
  int tx_task_delay;
} Config;

Config config = {
    .threshold = 2.0, // threshold after which the pump is turned on
    .sensor_threshold =
        2, // number of sensors that need to be above threshold to turn on pump
    .sensor_count = 3,             // number of sensors connected to the ESP32
    .pump_on_time = 5 * SECOND_US, // in microseconds, 5 seconds
    .send_id_delay = 3000,         // delay between sending IDs in ms (3s)
    .tx_task_delay =
        10 *
        MINUTE_MS // delay after another measurement set is taken in ms (10min)
};

// sensor data
typedef struct {
  uint32_t ID;
  double resistance_measurement_voltage;
  double V_HIGH_voltage;
  double V_LOW_voltage;
  double temperature;
} SensorData;

// history of sensor readings
typedef struct {
  SensorData current;
  SensorData previous1;
  SensorData previous2;
} SensorHistory;

SensorHistory
    sensorHistories[MAX_SENSORS]; // global var, all fields are init as 0

// timer to control pump on time
esp_timer_handle_t on_timer;

// task handles
TaskHandle_t rx_task_handle;
TaskHandle_t tx_task_handle;
TaskHandle_t sensordata_task_handle;

/* Function prototypes */
void uart_init(void);
void sd_card_init(void);
void initialize_sntp(void);
void wifi_init_sta(void);
void mqtt_init(void);

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data);
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data);
static void on_timer_callback(void *arg);

esp_err_t send_ID(const int port, uint32_t id);
SensorData Convert_Bytes_to_Decimal(uint8_t *data);
double v_to_temp(double v_ntc);
char *sensor_data_to_json(const SensorData *data, bool include_time);
void write_to_sd_card(SensorData data);

static void tx_task(void *arg);
static void rx_task(void *arg);
static void sensordata_task(void *arg);

void app_main(void) {
  // create timer to turn off pump after pump_on_time
  const esp_timer_create_args_t on_timer_args = {.callback = &on_timer_callback,
                                                 .name = "on-timer"};
  ESP_ERROR_CHECK(esp_timer_create(&on_timer_args, &on_timer));

  // set PUMP GPIO to output
  gpio_set_direction(GPIO_PUMP, GPIO_MODE_OUTPUT);
  // set GPIO to LOW and turn off pump during init
  // in case the pump was on before restart
  gpio_set_level(GPIO_PUMP, 0);

  // check if INIT_WIFI is true, if not skip init
  if (INIT_WIFI) {
    // Initialize NVS, needed for WiFi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();

    // if successfully connected, initialize SNTP and MQTT
    if (s_wifi_event_group != NULL) {
      if (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) {
        initialize_sntp();
        mqtt_init();
      }
    }
  }

  // check if LOG_TO_SD_CARD is true, if not skip init
  if (LOG_TO_SD_CARD) {
    sd_card_init();
  }

  uart_init();

  // wait a bit before starting the tasks
  vTaskDelay(2000 / portTICK_PERIOD_MS);

  // rx task gets double the default stack size, since fprintf uses a lot of
  // stack space
  xTaskCreate(rx_task, "uart_rx_task", STACK_SIZE * 2, NULL,
              configMAX_PRIORITIES, &rx_task_handle);
  xTaskCreate(tx_task, "uart_tx_task", STACK_SIZE, NULL,
              configMAX_PRIORITIES - 1, &tx_task_handle);
  xTaskCreate(sensordata_task, "sensordata_task", STACK_SIZE, NULL,
              configMAX_PRIORITIES - 2, &sensordata_task_handle);

  // this part is commented out by default, but can be used to check the stack
  // size of the tasks
  /*
  while (1)
  {
      UBaseType_t uxHighWaterMark = uxTaskGetStackHighWaterMark(rx_task_handle);
      ESP_LOGI(TAG, "Minimum free stack size for rx_task: %u", uxHighWaterMark);

      uxHighWaterMark = uxTaskGetStackHighWaterMark(tx_task_handle);
      ESP_LOGI(TAG, "Minimum free stack size for tx_task: %u", uxHighWaterMark);

      uxHighWaterMark = uxTaskGetStackHighWaterMark(sensordata_task_handle);
      ESP_LOGI(TAG, "Minimum free stack size for sensor readings task: %u",
  uxHighWaterMark);

      vTaskDelay(3000 / portTICK_PERIOD_MS);
  }
  */
}

static void on_timer_callback(void *arg) {
  // this is called after the timer expires
  // turn off pump
  gpio_set_level(GPIO_PUMP, 0); // Set GPIO to LOW
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  // wifi event handler
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_num < 5) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "Retry to connect to the AP");
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    ESP_LOGI(TAG, "Connect to the AP fail");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  ESP_LOGD(TAG,
           "Event dispatched from event loop base=%s, event_id=%" PRIi32 "",
           base, event_id);
  esp_mqtt_event_handle_t event = event_data;
  client = event->client;

  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    mqtt_connected = true;

    // subscribe to topics
    esp_mqtt_client_subscribe(client, THRESHOLD_TOPIC, 0);
    esp_mqtt_client_subscribe(client, SENSOR_COUNT_TOPIC, 0);
    esp_mqtt_client_subscribe(client, PUMP_ON_TIME_TOPIC, 0);
    esp_mqtt_client_subscribe(client, PUMP_ON_TOPIC, 0);
    esp_mqtt_client_subscribe(client, SEND_ID_DELAY_TOPIC, 0);
    esp_mqtt_client_subscribe(client, TX_TASK_DELAY_TOPIC, 0);
    esp_mqtt_client_subscribe(client, SENSOR_THRESHOLD_TOPIC, 0);
    esp_mqtt_client_subscribe(client, SENSOR_DATA_TOPIC, 0);

    break;
  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
    mqtt_connected = false;
    break;
  case MQTT_EVENT_SUBSCRIBED:
    ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
    break;
  case MQTT_EVENT_UNSUBSCRIBED:
    ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
    break;
  case MQTT_EVENT_PUBLISHED:
    ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
    break;
  case MQTT_EVENT_DATA:
    ESP_LOGI(TAG, "MQTT_EVENT_DATA");
    printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
    printf("DATA=%.*s\r\n", event->data_len, event->data);

    // Null-terminate the topic and data strings for easy handling
    char *topic = malloc(event->topic_len + 1);
    memcpy(topic, event->topic, event->topic_len);
    topic[event->topic_len] = '\0';

    char *data = malloc(event->data_len + 1);
    memcpy(data, event->data, event->data_len);
    data[event->data_len] = '\0';

    // Update configuration variables based on the topic
    if (strcmp(topic, THRESHOLD_TOPIC) == 0) {
      config.threshold = atof(data);
      ESP_LOGI(TAG, "New threshold: %f", config.threshold);
      esp_mqtt_client_publish(client, DEBUG_MSG, "threshold_updated", 0, 0, 0);
    } else if (strcmp(topic, PUMP_ON_TIME_TOPIC) == 0) {
      config.pump_on_time = atoi(data);
      ESP_LOGI(TAG, "New pump on time: %d", config.pump_on_time);
      esp_mqtt_client_publish(client, DEBUG_MSG, "pump_on_time_updated", 0, 0,
                              0);
    } else if (strcmp(topic, PUMP_ON_TOPIC) == 0) {
      gpio_set_level(GPIO_PUMP, 1);
      // Try to start the on_timer
      esp_err_t ret = esp_timer_start_once(on_timer, config.pump_on_time);
      if (ret != ESP_OK) {
        gpio_set_level(GPIO_PUMP, 0); // Turn off the pump immediately
        char error_message[100];      // Buffer for the error message
        snprintf(error_message, sizeof(error_message),
                 "Failed to start on_timer with error %d", ret);
        ESP_LOGE(TAG, "%s", error_message);

        // Publish the error via MQTT
        esp_mqtt_client_publish(client, DEBUG_MSG, error_message, 0, 0, 0);
      } else {
        esp_mqtt_client_publish(client, DEBUG_MSG, "pump_on", 0, 0, 0);
      }
    } else if (strcmp(topic, SENSOR_COUNT_TOPIC) == 0) {
      config.sensor_count = atoi(data);
      ESP_LOGI(TAG, "New sensor count: %d", config.sensor_count);
      esp_mqtt_client_publish(client, DEBUG_MSG, "sensor_count_updated", 0, 0,
                              0);
    } else if (strcmp(topic, SEND_ID_DELAY_TOPIC) == 0) {
      config.send_id_delay = atoi(data);
      ESP_LOGI(TAG, "New send id delay: %d", config.send_id_delay);
      esp_mqtt_client_publish(client, DEBUG_MSG, "send_id_delay_updated", 0, 0,
                              0);
    } else if (strcmp(topic, TX_TASK_DELAY_TOPIC) == 0) {
      config.tx_task_delay = atoi(data);
      ESP_LOGI(TAG, "New tx task delay: %d", config.tx_task_delay);
      esp_mqtt_client_publish(client, DEBUG_MSG, "tx_task_delay_updated", 0, 0,
                              0);
    } else if (strcmp(topic, SENSOR_THRESHOLD_TOPIC) == 0) {
      config.sensor_threshold = atoi(data);
      ESP_LOGI(TAG, "New sensor threshold: %d", config.sensor_threshold);
      esp_mqtt_client_publish(client, DEBUG_MSG, "sensor_threshold_updated", 0,
                              0, 0);
    } else if (strcmp(topic, SENSOR_DATA_TOPIC) == 0) {
      uint32_t sensor_id = atoi(data); // Parse the sensor ID from the data

      // Find the corresponding sensor data
      for (int i = 0; i < MAX_SENSORS; i++) {
        if (sensorHistories[i].current.ID == sensor_id) {
          // Convert the sensor data to JSON and publish it
          char *json = sensor_data_to_json(&sensorHistories[i].current, false);
          esp_mqtt_client_publish(client, SENSOR_DATA, json, 0, 0, 0);
          free(json);
          break;
        }
      }
    }

    free(topic);
    free(data);
    break;
  case MQTT_EVENT_ERROR:
    ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
    break;
  default:
    ESP_LOGI(TAG, "Other event id:%d", event->event_id);
    break;
  }
}

// Function to convert sensor data to JSON
char *sensor_data_to_json(const SensorData *data, bool include_time) {
  cJSON *json = cJSON_CreateObject();

  // Add time to JSON if requested
  if (include_time) {
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
      // Get current time
      time_t now;
      struct tm timeinfo;
      time(&now);
      localtime_r(&now, &timeinfo);

      char time_str[30];
      strftime(time_str, sizeof(time_str), "%FT%T",
               &timeinfo); // Convert timeinfo to ISO 8601 format
      cJSON_AddStringToObject(json, "Date and Time", time_str);
    } else {
      // Get time since boot
      int64_t time_since_boot = esp_timer_get_time();
      cJSON_AddNumberToObject(json, "Time since boot", time_since_boot);
    }
  }

  cJSON_AddNumberToObject(json, "ID", data->ID);
  cJSON_AddNumberToObject(json, "Resistance Measurement (V)",
                          data->resistance_measurement_voltage);
  cJSON_AddNumberToObject(json, "V_HIGH (V)", data->V_HIGH_voltage);
  cJSON_AddNumberToObject(json, "V_LOW (V)", data->V_LOW_voltage);
  cJSON_AddNumberToObject(json, "Temperature (C)", data->temperature);

  char *string = cJSON_Print(json);
  cJSON_Delete(json);
  return string;
}

// Function to write sensor data to SD card
void write_to_sd_card(SensorData data) {
  FILE *f = fopen(FILE_NAME, "a");
  if (f == NULL) {
    char error_message[100]; // Buffer for the error message
    snprintf(error_message, sizeof(error_message),
             "Failed to open file for writing");
    ESP_LOGE(TAG, "%s", error_message);

    // Publish the error via MQTT
    if (mqtt_connected) {
      esp_mqtt_client_publish(client, DEBUG_MSG, error_message, 0, 0, 0);
    }

    return;
  }

  // Convert sensor data to JSON
  char *json_data = sensor_data_to_json(&data, true);

  // Print sensor data to file
  fprintf(f, "%s\n", json_data); // insert JSON data
  free(json_data);               // Don't forget to free the JSON string

  if (fclose(f) != 0) {
    char error_message[100]; // Buffer for the error message
    snprintf(error_message, sizeof(error_message), "Failed to close file");
    ESP_LOGE(TAG, "%s", error_message);

    // Publish the error via MQTT
    if (mqtt_connected) {
      esp_mqtt_client_publish(client, DEBUG_MSG, error_message, 0, 0, 0);
    }

    return;
  }

  ESP_LOGI(TAG, "Data written to SD card\n");
  // send mqtt msg that write to sd card was successful
  if (mqtt_connected) {
    esp_mqtt_client_publish(client, DEBUG_MSG, "sd_card_write_successful", 0, 0,
                            0);
  }
}

// Function to send the sensor ID to request data
esp_err_t send_ID(const int port, uint32_t id) {
  uint8_t data[4];
  data[0] = id & 0xFF; // LSB
  data[1] = (id >> 8) & 0xFF;
  data[2] = (id >> 16) & 0xFF;
  data[3] = (id >> 24) & 0xFF; // MSB

  int write_bytes = uart_write_bytes(port, (const char *)data, sizeof(data));

  if (write_bytes != sizeof(data)) {
    ESP_LOGE(TAG, "Send data critical failure.");
    return ESP_FAIL; // Return an error code
  }
  return ESP_OK; // Return success code
}

// Function to convert measured voltage to temperature using the Steinhart-Hart
// equation
double v_to_temp(double v_ntc) {
  // Steinhart-Hart Coefficients
  // source: https://www.vishay.com/en/thermistors/ntc-rt-calculator/
  // and:
  // https://www.thinksrs.com/downloads/programs/therm%20calc/ntccalibrator/ntccalculator.html
  const double A = 1.081730263e-3;
  const double B = 2.156147549e-4;
  const double C = 3.665619003e-7;

  // Fixed resistor value used in voltage divider
  const double R = 10000.0;
  // Calculate the resistance of the NTC
  double R_ntc = v_ntc / ((REFERENCE_VOLTAGE - v_ntc) / R);

  // Calculate the temperature using the Steinhart-Hart equation
  double lnR = log(R_ntc);
  double T = 1.0 / (A + B * lnR + C * pow(lnR, 3));

  // Convert from Kelvin to Celsius
  T = T - 273.15;

  return T;
}

SensorData Convert_Bytes_to_Decimal(uint8_t *data) {
  SensorData sensorData;

  uint32_t ID;

  // Discard erroneous bytes at the beginning, if any
  // this loop increments the pointer at most 12 times until it finds a valid ID
  for (int i = 0; i < 12; i++) {
    ID = (data[3] << 24) | (data[2] << 16) | (data[1] << 8) | data[0];

    // if ID is a valid value, convert bytes to decimal
    if (ID >= 10000 && ID <= (10000 + config.sensor_count)) {
      uint32_t resistance_measurement =
          (data[7] << 24) | (data[6] << 16) | (data[5] << 8) | data[4];
      uint32_t V_HIGH =
          (data[11] << 24) | (data[10] << 16) | (data[9] << 8) | data[8];
      uint32_t V_LOW =
          (data[15] << 24) | (data[14] << 16) | (data[13] << 8) | data[12];
      uint32_t temperature_adc =
          (data[19] << 24) | (data[18] << 16) | (data[17] << 8) | data[16];

      // calc voltage values

      double const ADC_MAX_VALUE = 4095;

      double resistance_measurement_voltage =
          (resistance_measurement / ADC_MAX_VALUE) * REFERENCE_VOLTAGE;
      double V_HIGH_voltage = (V_HIGH / ADC_MAX_VALUE) * REFERENCE_VOLTAGE;
      double V_LOW_voltage = (V_LOW / ADC_MAX_VALUE) * REFERENCE_VOLTAGE;
      double temperature_voltage =
          (temperature_adc / ADC_MAX_VALUE) * REFERENCE_VOLTAGE;

      // convert voltage to temperature
      double temperature_c = v_to_temp(temperature_voltage);

      // save data to sensor history
      SensorHistory *history = &sensorHistories[ID - 10000];
      history->previous2 = history->previous1;
      history->previous1 = history->current;
      history->current.ID = ID;
      history->current.resistance_measurement_voltage =
          resistance_measurement_voltage;
      history->current.V_HIGH_voltage = V_HIGH_voltage;
      history->current.V_LOW_voltage = V_LOW_voltage;
      history->current.temperature = temperature_c;

      // save data to struct and return
      sensorData.ID = ID;
      sensorData.resistance_measurement_voltage =
          resistance_measurement_voltage;
      sensorData.V_HIGH_voltage = V_HIGH_voltage;
      sensorData.V_LOW_voltage = V_LOW_voltage;
      sensorData.temperature = temperature_c;

      return sensorData;
    }

    data++;
  }

  // if the function didnt return yet, the ID is not valid
  ESP_LOGE(TAG, "ID is not valid");
  sensorData.ID = 0;
  return sensorData;
}

// Init functions
void uart_init(void) {
  // Configure parameters of UART
  // 8 Databits, keine Parität, 1 Stopbit
  uart_config_t uart_config = {
      .baud_rate = BAUD_RATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 122,
      .source_clk = UART_SCLK_DEFAULT,
  };

  ESP_LOGI(TAG, "Start application and configure UART.");

  // Install UART driver (we don't need an event queue here)
  ESP_ERROR_CHECK(
      uart_driver_install(UART_PORT, RX_BUF_SIZE * 2, 0, 0, NULL, 0));

  // Configure UART parameters
  ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));

  // Set UART pins
  ESP_ERROR_CHECK(uart_set_pin(UART_PORT, TXD, RXD, RTS, CTS));

  // Set RS485 half duplex mode
  ESP_ERROR_CHECK(uart_set_mode(UART_PORT, UART_MODE_RS485_HALF_DUPLEX));

  // Set read timeout of UART TOUT feature
  ESP_ERROR_CHECK(uart_set_rx_timeout(UART_PORT, READ_TOUT));
}

void sd_card_init(void) {
  esp_err_t ret;

  // Options for mounting the filesystem.
  // If format_if_mount_failed is set to true, SD card will be partitioned and
  // formatted in case when mounting fails.
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024};
  sdmmc_card_t *card;
  const char mount_point[] = MOUNT_POINT;
  ESP_LOGI(TAG, "Initializing SD card");

  // Use settings defined above to initialize SD card and mount FAT filesystem.
  // Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
  // Please check its source code and implement error recovery when developing
  // production applications.
  ESP_LOGI(TAG, "Using SPI peripheral");

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.max_freq_khz = 1000;
  host.set_card_clk(host.slot, 1000);

  spi_bus_config_t bus_cfg = {
      .mosi_io_num = PIN_NUM_MOSI,
      .miso_io_num = PIN_NUM_MISO,
      .sclk_io_num = PIN_NUM_CLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 4000,
  };

  ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize bus.");
    return;
  }

  // This initializes the slot without card detect (CD) and write protect (WP)
  // signals. Modify slot_config.gpio_cd and slot_config.gpio_wp if your board
  // has these signals.
  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = PIN_NUM_CS;
  slot_config.host_id = host.slot;

  ESP_LOGI(TAG, "Mounting filesystem");
  ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config,
                                &card);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount filesystem.");
    } else {
      ESP_LOGE(TAG,
               "Failed to initialize the card (%s). "
               "Make sure SD card lines have pull-up resistors in place.",
               esp_err_to_name(ret));
    }
    return;
  }
  ESP_LOGI(TAG, "Filesystem mounted");

  // Card has been initialized, print its properties
  sdmmc_card_print_info(stdout, card);

  // All done, unmount partition and disable SPI peripheral
  // esp_vfs_fat_sdcard_unmount(mount_point, card);
  // ESP_LOGI(TAG, "Card unmounted");

  // deinitialize the bus after all devices are removed
  // spi_bus_free(host.slot);
}

void initialize_sntp(void) {
  ESP_LOGI(TAG, "Initializing SNTP");
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, "pool.ntp.org");
  sntp_init();

  // Set timezone to Central European Time (CET)
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
}

void wifi_init_sta(void) {
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

  wifi_config_t wifi_config = {
      .sta = {.ssid = WIFI_SSID, .password = WIFI_PASSWORD},
  };
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_sta finished.");

  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, portMAX_DELAY);

  /* xEventGroupWaitBits() returns the bits before the call returned, hence we
   * can test which event actually happened. */
  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Connected to AP SSID:%s", WIFI_SSID);
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGI(TAG, "Failed to connect to AP SSID:%s", WIFI_SSID);
  } else {
    ESP_LOGE(TAG, "UNEXPECTED EVENT");
  }

  // not sure if needed
  // ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
  // &event_handler)); ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT,
  // ESP_EVENT_ANY_ID, &event_handler)); vEventGroupDelete(s_wifi_event_group);
}

void mqtt_init() {
  ESP_LOGI(TAG, "STARTING MQTT");

  const esp_mqtt_client_config_t mqtt_cfg = {
      .broker.address.uri = MQTT_URI,
  };

  client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler,
                                 client);
  esp_mqtt_client_start(client);
}

// Tasks
static void tx_task(void *arg) {
  static const char *TX_TASK_TAG = "TX_TASK";
  esp_log_level_set(TX_TASK_TAG, ESP_LOG_INFO);

  while (1) {
    for (uint32_t i = 10000; i < (10000 + config.sensor_count); i++) {
      // Send ID to request data
      esp_err_t ret = send_ID(UART_PORT, i);
      if (ret != ESP_OK) {
        char error_message[100]; // Buffer for the error message
        snprintf(error_message, sizeof(error_message),
                 "send_ID failed with error %d", ret);
        ESP_LOGE(TX_TASK_TAG, "%s", error_message);

        // Publish the error via MQTT
        if (mqtt_connected) {
          esp_mqtt_client_publish(client, DEBUG_MSG, error_message, 0, 0, 0);
        }

        continue; // Skip to the next iteration
      }

      ret = uart_wait_tx_done(UART_PORT, 10);
      if (ret != ESP_OK) {
        char error_message[100]; // Buffer for the error message
        snprintf(error_message, sizeof(error_message),
                 "uart_wait_tx_done failed with error %d", ret);
        ESP_LOGE(TX_TASK_TAG, "%s", error_message);

        // Send error message to MQTT broker if connected
        if (mqtt_connected) {
          esp_mqtt_client_publish(client, DEBUG_MSG, error_message, 0, 0, 0);
        }

        continue; // Skip to the next iteration
      }

      ESP_LOGI(TAG, "Sent ID: %lu", i);

      // wait send_id_delay
      vTaskDelay(config.send_id_delay / portTICK_PERIOD_MS);
    }
    // wait tx_task_delay
    vTaskDelay(config.tx_task_delay / portTICK_PERIOD_MS);
  }
}

static void rx_task(void *arg) {
  static const char *RX_TASK_TAG = "RX_TASK";
  esp_log_level_set(RX_TASK_TAG, ESP_LOG_INFO);
  uint8_t *data = (uint8_t *)malloc(RX_BUF_SIZE);
  while (1) {
    // Read data from UART
    const int rxBytes =
        uart_read_bytes(UART_PORT, data, RX_BUF_SIZE, PACKET_READ_TICS);
    if (rxBytes >= DATA_LENGTH) {
      ESP_LOGI(TAG, "Received %u bytes:", rxBytes);
      printf("[ ");
      for (int i = 0; i < rxBytes; i++) {
        printf("0x%.2X ", (uint8_t)data[i]);
      }
      printf("] \n");

      // convert bytes to decimal
      // if ID is not valid, returned ID is zero
      SensorData sensorData = Convert_Bytes_to_Decimal(data);

      if (sensorData.ID != 0) {
        // log data to console
        ESP_LOGI(TAG, "ID: %lu", sensorData.ID);
        ESP_LOGI(TAG, "Resistance Measurement (V): %f",
                 sensorData.resistance_measurement_voltage);
        ESP_LOGI(TAG, "V_HIGH (V): %f", sensorData.V_HIGH_voltage);
        ESP_LOGI(TAG, "V_LOW (V): %f", sensorData.V_LOW_voltage);
        ESP_LOGI(TAG, "Temperature (°C): %f", sensorData.temperature);

        // Publish data to MQTT broker if connected
        if (mqtt_connected) {
          char *json = sensor_data_to_json(&sensorData, true);
          esp_mqtt_client_publish(client, SENSOR_DATA, json, 0, 0, 0);
          free(json);
        }

        if (LOG_TO_SD_CARD) {
          // write data to SD card
          write_to_sd_card(sensorData);
        }
      }
    } else if (rxBytes > 0) {
      ESP_LOGI(TAG, "Data is too short! Received %u bytes:", rxBytes);
      printf("[ ");
      for (int i = 0; i < rxBytes; i++) {
        printf("0x%.2X ", (uint8_t)data[i]);
      }
      printf("] \n");
    }
  }
  free(data);
}

static void sensordata_task(void *arg) {
  while (1) {
    int count = 0;

    for (int i = 0; i < config.sensor_count; i++) {
      SensorHistory *history = &sensorHistories[i];

      // check if enough data is available
      if (history->current.ID == 0 || history->previous1.ID == 0 ||
          history->previous2.ID == 0) {
        continue;
      }

      if ((history->current.V_HIGH_voltage - history->current.V_LOW_voltage >
           config.threshold) &&
          (history->previous1.V_HIGH_voltage -
               history->previous1.V_LOW_voltage >
           config.threshold) &&
          (history->previous2.V_HIGH_voltage -
               history->previous2.V_LOW_voltage >
           config.threshold)) {
        // Increase the count
        count++;
      }
    }

    // if sensor threshold is set above sensor count, set it to sensor count
    if (config.sensor_threshold > config.sensor_count) {
      config.sensor_threshold = config.sensor_count;
    }

    // Check if enough sensors meet the condition
    if (count >= config.sensor_threshold) {
      // Turn on pump for pump_on_time
      gpio_set_level(GPIO_PUMP, 1);

      // Try to start the on_timer
      esp_err_t ret = esp_timer_start_once(on_timer, config.pump_on_time);
      if (ret != ESP_OK) {
        gpio_set_level(GPIO_PUMP, 0); // Turn off the pump immediately
        char error_message[100];      // Buffer for the error message
        snprintf(error_message, sizeof(error_message),
                 "Failed to start on_timer with error %d", ret);
        ESP_LOGE(TAG, "%s", error_message);

        // Publish the error via MQTT
        if (mqtt_connected) {
          esp_mqtt_client_publish(client, DEBUG_MSG, error_message, 0, 0, 0);
        }
        return;
      }
    }

    // Delay a bit before checking again
    vTaskDelay(config.tx_task_delay / portTICK_PERIOD_MS);
  }
}
