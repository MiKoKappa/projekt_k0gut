/* 
    Project K0_GUT [R00_STER]

    RTOS course final project on SGGW.

    Description: 
    An ESP32-based device with ESP-IDF which is FreeRTOS implementation on ESP.
    The device connects to Wi-Fi network, sets current time on DS3231 RTC and performs a HTTP GET request,
    in order to get the next sunrise time. At this time an alarm is triggered, powering a buzzer, which needs to be
    stopped via a push button on board.
    The cycle continues for the next day.

    Creator:
    Mikołaj Tkaczyk
    S196129

    20.05.2023
*/

#include <string.h>
#include <ds3231.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/ledc.h"

#include "protocol_examples_common.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "sdkconfig.h"

/* Constants of API request */
#define WEB_SERVER "api.sunrise-sunset.org"
#define WEB_PORT "80"
// Latitude and longitude of SGGW :)
#define WEB_PATH "/json?lat=52.1613377&lng=21.045379&date=tomorrow"

#define GPIO_INPUT 15
#define GPIO_INPUT_PIN_SEL (1ULL<<GPIO_INPUT)
#define ESP_INTR_FLAG_DEFAULT 0

static const char *TAG = "Project K0_GUT";

static QueueHandle_t gpio_evt_queue = NULL;

struct tm next_alarm_time = {0};
TaskHandle_t TaskHandle_HTTP_Request;
bool is_armed = false;

static const char *REQUEST = "GET " WEB_PATH " HTTP/1.0\r\n"
    "Host: "WEB_SERVER":"WEB_PORT"\r\n"
    "User-Agent: esp-idf/1.0 esp32\r\n"
    "\r\n";

SemaphoreHandle_t xMutex = NULL;

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_task(void* arg)
{
    uint32_t io_num;
    while(1) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            ESP_ERROR_CHECK(ledc_timer_pause(LEDC_HIGH_SPEED_MODE, LEDC_TIMER_0));
            vTaskResume(TaskHandle_HTTP_Request);
        }
    }
}

// HTTP GET task using plain POSIX sockets
static void http_get_task(void *pvParameters)
{
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    struct in_addr *addr;
    int s, r;
    char recv_buf[64];

    while(1) {
        int err = getaddrinfo(WEB_SERVER, WEB_PORT, &hints, &res);

        if(err != 0 || res == NULL) {
            ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        /* Code to print the resolved IP.

           Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
        addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
        ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

        s = socket(res->ai_family, res->ai_socktype, 0);
        if(s < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket.");
            freeaddrinfo(res);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... allocated socket");

        if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
            ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
            close(s);
            freeaddrinfo(res);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "... connected");
        freeaddrinfo(res);

        if (write(s, REQUEST, strlen(REQUEST)) < 0) {
            ESP_LOGE(TAG, "... socket send failed");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... socket send success");

        struct timeval receiving_timeout;
        receiving_timeout.tv_sec = 5;
        receiving_timeout.tv_usec = 0;
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
                sizeof(receiving_timeout)) < 0) {
            ESP_LOGE(TAG, "... failed to set socket receiving timeout");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... set socket receiving timeout success");

        /* Read HTTP response */
        int hour, minute, second;
        char *sunrise;

        // Loop through binary buffer packets
        do {
            bzero(recv_buf, sizeof(recv_buf));
            r = read(s, recv_buf, sizeof(recv_buf)-1);
            
            // Get time of sunrise from buffer
            sunrise = strstr(recv_buf, "sunrise");
            if(sunrise != NULL){
                sscanf(sunrise+10, "%d:%d:%d", &hour, &minute, &second);
            }

            // for(int i = 0; i < r; i++) {
            //     putchar(recv_buf[i]);
            // }

        } while(r > 0);

        // Create a struct to store in global variable
        struct tm time = {
        .tm_hour = hour+2,
        .tm_min  = minute,
        .tm_sec  = second
        };

        // Log time of sunrise to serial
        ESP_LOGI(TAG, "Sunrise time: %02d:%02d:%02d", hour+2, minute, second);

        // Acquiring a mutex for global alarm time variable
        if( xMutex != NULL ){
            if( xSemaphoreTake( xMutex, ( TickType_t ) 10 ) == pdTRUE )
            {
                /* We were able to obtain the semaphore and can now access the
                shared resource. */

                next_alarm_time = time;

                /* We have finished accessing the shared resource.  Release the
                semaphore. */
                xSemaphoreGive( xMutex );
            }
            else
            {
                ESP_LOGI(TAG, "Could not acquire a mutex for alarm time global variable!");
            }
        }

        ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d.", r, errno);

        // Close socket connection
        close(s);

        // Delay next task execution by 10 seconds
        // vTaskDelay(10000 / portTICK_PERIOD_MS);
        vTaskSuspend(NULL);
    }
}

static void ds3231_task(void *pvParameters)
{
    // Setting new I2C device address
    i2c_dev_t dev;
    memset(&dev, 0, sizeof(i2c_dev_t));

    // Initialization of DS3231 with definition of SDA and SCL pins of I2C
    ESP_ERROR_CHECK(ds3231_init_desc(&dev, 0, 21, 22));

    // Getting date and time from compilation
    char *date = __DATE__;
    char *time = __TIME__;

    // Converting string to int values
    char s_month[5];
    int month, day, year;
    int hour, minute, second;
    struct tm t = {0};
    static const char month_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

    sscanf(date, "%s %d %d", s_month, &day, &year);
    sscanf(time, "%d:%d:%d", &hour, &minute, &second);

    month = (strstr(month_names, s_month)-month_names)/3;

    // Compensation of time elapsed since compilation in DS3231.
    int time_elapsed = esp_timer_get_time();

    ESP_LOGI(TAG, "Time elapsed [us]: %d", time_elapsed);
    ESP_LOGI(TAG, "Time elapsed [s]: %d", time_elapsed/1000000);

    second = second + (time_elapsed/1000000);
    minute = minute + second/60;
    second = second % 60;

    // Setting time in time struct
    t.tm_mon = month;
    t.tm_mday = day;
    t.tm_year = year - 1900;
    t.tm_isdst = -1;
    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_sec  = second;

    // Setting time on DS3231 via I2C
    ESP_ERROR_CHECK(ds3231_set_time(&dev, &t));

    // Main task loop
    while (1)
    {
        // Getting time from DS3231 via I2C and store in t variable
        ds3231_get_time(&dev, &t);

        uint32_t timestamp_now = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;

        if( xMutex != NULL ){
            if( xSemaphoreTake( xMutex, ( TickType_t ) 10 ) == pdTRUE )
            {
                /* We were able to obtain the semaphore and can now access the
                shared resource. */

                ESP_LOGI(TAG, "Next alarm: %02d:%02d:%02d\n", next_alarm_time.tm_hour, next_alarm_time.tm_min, next_alarm_time.tm_sec);
                if(is_armed){
                    if(next_alarm_time.tm_hour * 3600 + next_alarm_time.tm_min * 60 + next_alarm_time.tm_sec < timestamp_now){
                        ESP_ERROR_CHECK(ledc_timer_resume(LEDC_HIGH_SPEED_MODE, LEDC_TIMER_0));
                        is_armed = false;
                    }
                }
                if(next_alarm_time.tm_hour * 3600 + next_alarm_time.tm_min * 60 + next_alarm_time.tm_sec > timestamp_now){
                        ESP_ERROR_CHECK(ledc_timer_pause(LEDC_HIGH_SPEED_MODE, LEDC_TIMER_0));
                        is_armed = true;
                    }

                /* We have finished accessing the shared resource.  Release the
                semaphore. */
                xSemaphoreGive( xMutex );
            }
            else
            {
                ESP_LOGI(TAG, "Could not acquire a mutex for alarm time global variable!");
            }
        }

        // Log current time to serial
        ESP_LOGI(TAG ,"Current time: %04d-%02d-%02d %02d:%02d:%02d\n", t.tm_year + 1900, t.tm_mon + 1,
            t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);

        // Delay next task execution by 500ms = twice a second
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}


void app_main(void)
{
    // Initialization of core modules
    ESP_ERROR_CHECK(i2cdev_init());
    ESP_ERROR_CHECK(nvs_flash_init() );
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */

    // Helper fuction from esp-idf configuring Wi-Fi from ESP-IDF menuconfig.
    ESP_ERROR_CHECK(example_connect());
    
    // Creating a mutex
    xMutex = xSemaphoreCreateMutex();

    ledc_channel_config_t ledc_channel;

	ledc_timer_config_t ledc_timer = {
    .speed_mode = LEDC_HIGH_SPEED_MODE,
    .timer_num  = LEDC_TIMER_0,
    .duty_resolution = LEDC_TIMER_8_BIT,
    .freq_hz = 3000,
    .clk_cfg = LEDC_AUTO_CLK
    };

	// Set configuration of timer0 for high speed channels
	ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
	
	ledc_channel.channel    = LEDC_CHANNEL_0;
	ledc_channel.duty       = 125;
	ledc_channel.gpio_num   = 27;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
	ledc_channel.speed_mode = LEDC_HIGH_SPEED_MODE;
	ledc_channel.hpoint     = 0;
	ledc_channel.timer_sel  = LEDC_TIMER_0;

	ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    ESP_ERROR_CHECK(ledc_timer_pause(LEDC_HIGH_SPEED_MODE, LEDC_TIMER_0));

    gpio_config_t io_conf = {};
    //interrupt of rising edge
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    //bit mask of the pins
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 0;
    io_conf.pull_down_en = 1;
    gpio_config(&io_conf);

    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    //start gpio task
    xTaskCreate(gpio_task, "gpio_task", 2048, NULL, 10, NULL);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT, gpio_isr_handler, (void*) GPIO_INPUT);

    // Task definition
    xTaskCreate(&http_get_task, "http_get_task", 4096, NULL, 5, &TaskHandle_HTTP_Request);
    xTaskCreate(&ds3231_task, "ds3231_task", 4096, NULL, 5, NULL);
}
