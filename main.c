#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include <esp_http_server.h>
#include "driver/gpio.h"

#include "ssd1306.h"
#include "ultrasonic.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/api.h>
#include <lwip/netdb.h>

#define SSID "brisa-2401466"
#define PASSWORD "fkmsj15d"
#define ESP_MAXIMUM_RETRY 5

#define TRIGGER_PIN GPIO_NUM_12
#define ECHO_PIN GPIO_NUM_17
#define MAX_DISTANCE_CM 500

#define LED_GREEN_PIN GPIO_NUM_2
#define LED_YELLOW_PIN GPIO_NUM_4
#define LED_RED_PIN GPIO_NUM_5

#define I2C_MASTER_SCL_IO 22      /*!< gpio number for I2C master clock */
#define I2C_MASTER_SDA_IO 21      /*!< gpio number for I2C master data  */
#define I2C_MASTER_NUM I2C_NUM_0  /*!< I2C port number for master dev */
#define I2C_MASTER_FREQ_HZ 100000 /*!< I2C master clock frequency */

#define TAG "Trabalho Final - freeRTOS"
#define RETRY_NUM 5

int wifi_connect_status = 0;
int retry_num = 0;

uint32_t distance;

static QueueHandle_t distance_queue;
static SemaphoreHandle_t my_semaphore;
static ssd1306_handle_t dev = NULL;

volatile uint32_t ultrasonic_stack_highwater = 0;
volatile uint32_t led_stack_highwater = 0;

bool isGreenLEDOn = false;
bool isYellowLEDOn = false;
bool isRedLEDOn = false;

ultrasonic_sensor_t sensor = {
        .trigger_pin = TRIGGER_PIN,
        .echo_pin = ECHO_PIN
    };


char html_page[] = "<!DOCTYPE HTML><html>\n"
                  "<head>\n"
                  "<title>Projeto Final - Curso de Extensão</title>\n"
                  "<meta charset='utf-8'>\n"
                  "<meta name='viewport' content='width=device-width, initial-scale=1'>\n"
                  "</head>\n"
                  "<body>\n"
                  "<h1>Projeto Final - Curso de Extensão</h1>"
                  "<div class='traffic-light-container'>"
                  "<div class='traffic-light'>"
                  "<div class='light red' id='red-light'></div>"
                  "<div class='light yellow' id='yellow-light'></div>"
                  "<div class='light green' id='green-light'></div>"
                  "</div>"
                  "</div>"
                  "<p>Distância: <span id='distance'></span> cm</p>"
                  "<style>"
                  ".traffic-light-container { display: inline-block; vertical-align: middle; }"
                  ".traffic-light { width: 60px; height: 180px; background: #333; margin: 10px auto; }"
                  ".light { width: 50px; height: 50px; border-radius: 50%; margin: 10px auto; }"
                  ".red { background-color: ";
                  isRedLEDOn ? "red" : "white"
                  "; }"
                  ".yellow { background-color: ";
                  isYellowLEDOn ? "yellow" : "white"
                  "; }"
                  ".green { background-color: ";
                  isGreenLEDOn ? "green" : "white"
                  "; }"
                  "</style>"
                  "<script>"
                  "setInterval(function() {"
                  "  fetch('/distance')"
                  "    .then(response => response.text())"
                  "    .then(data => {"
                  "      document.getElementById('distance').textContent = data;"
                  "    });"
                  "  fetch('/leds')"
                  "    .then(response => response.json())"
                  "    .then(data => {"
                  "      document.getElementById('red-light').style.backgroundColor = data.red ? 'red' : 'white';"
                  "      document.getElementById('yellow-light').style.backgroundColor = data.yellow ? 'yellow' : 'white';"
                  "      document.getElementById('green-light').style.backgroundColor = data.green ? 'green' : 'white';"
                  "    });"
                  "}, 1000);"
                  "</script>"
                  "</body></html>"

static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{

    if (event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "Conectando ...");
        esp_wifi_connect();
    }

    else if (event_id == WIFI_EVENT_STA_CONNECTED)
    {
        ESP_LOGI(TAG, "Conectado a rede local");
        wifi_connect_status = 1;
    }

    else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG, "Desconectado a rede local");
        wifi_connect_status = 0;

        if (retry_num < RETRY_NUM)
        {
            esp_wifi_connect();
            retry_num++;
            ESP_LOGI(TAG, "Reconectando ...");
        }
    }
    
    else if (event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "IP: " IPSTR,IP2STR(&event->ip_info.ip));
        retry_num = 0;
    }
}

void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    wifi_config_t wifi_config = 
    {
        .sta = 
        {
            .ssid = SSID,
            .password = PASSWORD,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	  esp_wifi_set_storage(WIFI_STORAGE_RAM);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SSID:%s  password:%s",SSID,PASSWORD);
}

esp_err_t web_page(httpd_req_t *req)
{
    distance();
    char response_data[sizeof(html_page) + 50];
    memset(response_data, 0x00, sizeof(response_data));
    
    sprintf(response_data, html_page, temp, hum);
    return httpd_resp_send(req, response_data, HTTPD_RESP_USE_STRLEN);
}

esp_err_t req_handler(httpd_req_t *req)
{
    return web_page(req);
}

httpd_uri_t uri_get = 
{
    .uri = "/",
    .method = HTTP_GET, // Método de requisição
    .handler = req_handler, // Handler 
    .user_ctx = NULL // Pointeiro usado para receber o contexto (dados) oferecido pelo handler  
};

httpd_handle_t setup_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) // Inicializa o server
    {
        httpd_register_uri_handler(server, &uri_get); // Regitra o handler e a URI
    }

    return server;
}

void ultrasonic_task(void *pvParameters) {
  ultrasonic_init(&sensor);

  while (true) {
    esp_err_t res = ultrasonic_measure_cm(&sensor, MAX_DISTANCE_CM, &distance);
    if (res != ESP_OK)
        {
            printf("Error %d: ", res);
            switch (res)
            {
                case ESP_ERR_ULTRASONIC_PING:
                    printf("Cannot ping (device is in invalid state)\n");
                    break;
                case ESP_ERR_ULTRASONIC_PING_TIMEOUT:
                    printf("Ping timeout (no device found)\n");
                    break;
                case ESP_ERR_ULTRASONIC_ECHO_TIMEOUT:
                    printf("Echo timeout (i.e. distance too big)\n");
                    break;
                default:
                    printf("%s\n", esp_err_to_name(res));
            }
        }
        else
            printf("Distance: %f cm\n", distance);

        vTaskDelay(pdMS_TO_TICKS(100));

    xQueueSend(distance_queue, &distance, portMAX_DELAY);
    xSemaphoreGive(my_semaphore);

    // Atualize o estado dos LEDs
    if (distance > 30) {
      isGreenLEDOn = true;
      isYellowLEDOn = false;
      isRedLEDOn = false;
    } else if (distance <= 30 && distance >= 15) {
      isGreenLEDOn = false;
      isYellowLEDOn = true;
      isRedLEDOn = false;
    } else {
      isGreenLEDOn = false;
      isYellowLEDOn = false;
      isRedLEDOn = true;
    }

    uint32_t stack_used = uxTaskGetStackHighWaterMark(NULL);
    if (stack_used > ultrasonic_stack_highwater) {
      ultrasonic_stack_highwater = stack_used;
    }

    vTaskDelay(pdMS_TO_TICKS(900));
  }
}

void led_task(void *pvParameters) {
  while (1) {
    float distance;

    if (xSemaphoreTake(my_semaphore, portMAX_DELAY) == pdTRUE) {
      if (xQueueReceive(distance_queue, &distance, 0) == pdTRUE) {
        if (isGreenLEDOn) {
          gpio_set_level(LED_GREEN_PIN, 1);
          gpio_set_level(LED_YELLOW_PIN, 0);
          gpio_set_level(LED_RED_PIN, 0);
        } else if (isYellowLEDOn) {
          gpio_set_level(LED_GREEN_PIN, 0);
          gpio_set_level(LED_YELLOW_PIN, 1);
          gpio_set_level(LED_RED_PIN, 0);
        } else {
          gpio_set_level(LED_GREEN_PIN, 0);
          gpio_set_level(LED_YELLOW_PIN, 0);
          gpio_set_level(LED_RED_PIN, 1);
        }
      }
    }

    uint32_t stack_used = uxTaskGetStackHighWaterMark(NULL);
    if (stack_used > led_stack_highwater) {
      led_stack_highwater = stack_used;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void monitor_task(void *pvParameters) {
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(5000));

    size_t free_heap = esp_get_free_heap_size();
    uint32_t ultrasonic_stack = ultrasonic_stack_highwater;
    uint32_t led_stack = led_stack_highwater;

    printf("\nHeap Livre: ");
    printf(free_heap);
    printf(" bytes\n");
    printf("Stack Ultrasonic: ");
    printf(ultrasonic_stack);
    printf(" bytes\n");
    printf("Stack LED: ");
    printf(led_stack);
    printf(" bytes\n");

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void init_hw() {
  gpio_pad_selection(LED_GREEN_PIN);
  gpio_set_direction(LED_GREEN_PIN, GPIO_MODE_OUTPUT);
  gpio_pad_selection(LED_YELLOW_PIN);
  gpio_set_direction(LED_YELLOW_PIN, GPIO_MODE_OUTPUT);
  gpio_pad_selection(LED_RED_PIN);
  gpio_set_direction(LED_RED_PIN, GPIO_MODE_OUTPUT);

}

void app_main() {
  init_hw();

  distance_queue = xQueueCreate(1, sizeof(float));
  my_semaphore = xSemaphoreCreateBinary();

  esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init ();
    
    vTaskDelay(pdMS_TO_TICKS(10000));

    if (wifi_connect_status)
    {
        setup_server();
        ESP_LOGI(TAG, "Web Server inicializado");
    }
    else
    {
        ESP_LOGI(TAG, "Falha ao conectar à rede local");
    }
  
  xTaskCreate(ultrasonic_task, "ultrasonic_task", 4096, NULL, 5, NULL);
  xTaskCreate(led_task, "led_task", 4096, NULL, 4, NULL);
  xTaskCreate(monitor_task, "monitor_task", 4096, NULL, 3, NULL);
}
