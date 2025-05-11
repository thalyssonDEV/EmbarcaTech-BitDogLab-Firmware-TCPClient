#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/adc.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/tcp.h"

#define WIFI_SSID "teste"
#define WIFI_PASSWORD "teste123"

#define PROXY_HOST "tramway.proxy.rlwy.net"
#define PROXY_PORT 11444

#define VRX_JOYSTICK 27
#define VRY_JOYSTICK 26
#define BUTTON_PIN 5
#define TRIG_PIN 17     
#define ECHO_PIN 16

const uint8_t JOYSTICK_DEADZONE_MIN = 40;
const uint8_t JOYSTICK_DEADZONE_MAX = 59;


bool last_button_state = false;
bool button_status = false; 


void check_button_press(uint button_pin, bool* last_state, bool* status_flag) {
    bool current_state = gpio_get(button_pin);

    if (current_state && !(*last_state)) {
        *status_flag = true;
    }

    if (!current_state && *last_state) {
        *status_flag = false;
    }

    *last_state = current_state;
}


float get_reading_sensor() {
    const uint32_t timeout = 38000; /**< Timeout threshold to prevent infinite loops (~65ms -> ~4m max range) */

    // Trigger pulse to initiate measurement
    gpio_put(TRIG_PIN, 0);  /**< Sends the state to the TRIG pin */
    sleep_us(2);
    gpio_put(TRIG_PIN, 1);  /**< Sends the state to the TRIG pin */
    sleep_us(10);
    gpio_put(TRIG_PIN, 0);  /**< Sends the state to the TRIG pin */

    // Wait for the echo signal to start, with timeout
    uint32_t start_time = time_us_32();
    while (!gpio_get(ECHO_PIN)) {
        if ((time_us_32() - start_time) > timeout) {
            return -1.0;  /**< Return -1 to indicate timeout (no object detected) */
        }
    }

    // Capture the start time
    absolute_time_t start = get_absolute_time();

    // Wait for the echo signal to end, with timeout
    start_time = time_us_32();
    while (gpio_get(ECHO_PIN)) {
        if ((time_us_32() - start_time) > timeout) {
            return -1.0;  /**< Return -1 to indicate timeout */
        }
    }

    // Capture the end time
    absolute_time_t end = get_absolute_time();

    // Compute the duration of the echo pulse
    int64_t duration = absolute_time_diff_us(start, end);

    // Convert duration to distance in cm
    float distance = duration / 58.0;

    // Optional: Ignore unrealistic readings (e.g., too close or too far)
    if (distance < 1.0 || distance > 400.0) {
        return -1.0;  /**< Invalid reading */
    }

    return distance;
}


uint8_t value_joystick_x() 
{
    adc_select_input(1);
    return adc_read() * 100 / 4095;
}


uint8_t value_joystick_y()
{

    adc_select_input(0);
    return adc_read() * 100 / 4095;;
}


const char *get_direction(uint8_t vrx, uint8_t vry) {
    bool center_x = (vrx >= JOYSTICK_DEADZONE_MIN && vrx <= JOYSTICK_DEADZONE_MAX);
    bool center_y = (vry >= JOYSTICK_DEADZONE_MIN && vry <= JOYSTICK_DEADZONE_MAX);

    if (center_x && center_y) return "Center";

    if (vrx < JOYSTICK_DEADZONE_MIN) {
        if (vry < JOYSTICK_DEADZONE_MIN) return "Southwest";
        else if (vry > JOYSTICK_DEADZONE_MAX) return "Northwest";
        else return "West";
    } else if (vrx > JOYSTICK_DEADZONE_MAX) {
        if (vry < JOYSTICK_DEADZONE_MIN) return "Southeast";
        else if (vry > JOYSTICK_DEADZONE_MAX) return "Northeast";
        else return "East";
    } else {
        if (vry < JOYSTICK_DEADZONE_MIN) return "South";
        else if (vry > JOYSTICK_DEADZONE_MAX) return "North";
    }

    return "Unknown";
}


static err_t callback_response_received(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (!p) {
        printf("Connection closed by the server.\n");
        tcp_close(pcb);
        return ERR_OK;
    }

    printf("Server response:\n");
    char *data = (char *)malloc(p->tot_len + 1);
    if (data) {
        pbuf_copy_partial(p, data, p->tot_len, 0);
        data[p->tot_len] = '\0';
        printf("%s\n", data);
        free(data);
    }

    pbuf_free(p);
    return ERR_OK;
}

static err_t callback_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    if (err != ERR_OK) {
        printf("Connection error: %d\n", err);
        tcp_abort(pcb);
        return err;
    }

    tcp_recv(pcb, callback_response_received);

    // Collect data
    uint8_t vrx = value_joystick_x();
    uint8_t vry = value_joystick_y();
    const char *direction = get_direction(vrx, vry);
    check_button_press(BUTTON_PIN, &last_button_state, &button_status);
    float sensor_status = get_reading_sensor();

    char json_body[128];
    snprintf(json_body, sizeof(json_body),
             "{\"direction\": \"%s\", \"vrx\": %d, \"vry\": %d, \"button_status\": %d, \"sensor_status\": %.2f}",
             direction, vrx, vry, button_status, sensor_status);

    char request[512];
    // Use PROXY_HOST in the Host header
    snprintf(request, sizeof(request),
             "POST /data HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n"
             "\r\n"
             "%s",
             PROXY_HOST, strlen(json_body), json_body);

    cyw43_arch_lwip_begin();
    err_t send_error = tcp_write(pcb, request, strlen(request), TCP_WRITE_FLAG_COPY);
    if (send_error == ERR_OK) {
        tcp_output(pcb);
        printf("Request sent to %s:%d:\n%s\n", PROXY_HOST, PROXY_PORT, request);
    } else {
        printf("Error sending data: %d\n", send_error);
        tcp_abort(pcb);
    }
    cyw43_arch_lwip_end();

    return ERR_OK;
}

static void callback_dns_resolved(const char *hostname, const ip_addr_t *resolved_ip, void *arg) {
    if (!resolved_ip) {
        printf("Error: DNS failed for %s\n", hostname);
        return;
    }

    printf("DNS resolved %s to %s\n", hostname, ipaddr_ntoa(resolved_ip));

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        printf("Error creating pcb\n");
        return;
    }

    // Connect to the PROXY port
    err_t error = tcp_connect(pcb, resolved_ip, PROXY_PORT, callback_connected);
    if (error != ERR_OK) {
        printf("Error connecting to %s:%d: %d\n", hostname, PROXY_PORT, error);
        tcp_abort(pcb);
    }
}

void send_data_to_cloud() {
    ip_addr_t ip_address;
    // Use PROXY_HOST for DNS resolution
    err_t dns_result = dns_gethostbyname(PROXY_HOST, &ip_address, callback_dns_resolved, NULL);

    if (dns_result == ERR_OK) {
        // Already resolved (from cache), connect directly
        printf("Host %s already resolved to %s\n", PROXY_HOST, ipaddr_ntoa(&ip_address));

        struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!pcb) {
            printf("Error creating pcb (cache)\n");
            return;
        }

        err_t error = tcp_connect(pcb, &ip_address, PROXY_PORT, callback_connected);
        if (error != ERR_OK) {
            printf("Error connecting (cache) to %s:%d: %d\n", PROXY_HOST, PROXY_PORT, error);
            tcp_abort(pcb);
        }
    } else if (dns_result == ERR_INPROGRESS) {
        printf("DNS resolution in progress for %s...\n", PROXY_HOST);
    } else {
        printf("Error starting DNS for %s: %d\n", PROXY_HOST, dns_result);
    }
}


int wifi_init() {

    // Initialise the Wi-Fi chip
    if (cyw43_arch_init()) {
        printf("Wi-Fi Init Failed\n");
        return 1;
    }

    // Enable wifi station
    cyw43_arch_enable_sta_mode();

    printf("Connecting To Wi-Fi...\n");
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("Failed To Connect.\n");
        return 1;
    } else {
        printf("Connected.\n");
        uint8_t *ip_address = (uint8_t*)&(cyw43_state.netif[0].ip_addr.addr);
        printf("IP Address %d.%d.%d.%d\n", ip_address[0], ip_address[1], ip_address[2], ip_address[3]);
    }
    return 0;
}


void setup() {
    stdio_init_all();

    adc_init();
    adc_gpio_init(VRX_JOYSTICK);
    adc_gpio_init(VRY_JOYSTICK);

    gpio_init(BUTTON_PIN); 
    gpio_set_dir(BUTTON_PIN, GPIO_IN); 
    gpio_pull_up(BUTTON_PIN);

    gpio_init(TRIG_PIN);
    gpio_set_dir(TRIG_PIN, GPIO_OUT);
    gpio_put(TRIG_PIN, 0);

    gpio_init(ECHO_PIN);
    gpio_set_dir(ECHO_PIN, GPIO_IN);

    sleep_ms(2500); // Wait for the system to stabilize
}


int main() {
    setup();
    wifi_init();

    while (1) {
        send_data_to_cloud();
        sleep_ms(1000);

        cyw43_arch_poll();
    }
}