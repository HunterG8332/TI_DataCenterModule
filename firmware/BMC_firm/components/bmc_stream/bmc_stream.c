#include "bmc_stream.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "bmc_net.h"
#include "bmc_telemetry.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "bmc_stream";

#define MAX_SUBSCRIBERS       4
#define SUBSCRIBER_TIMEOUT_US (10 * 1000000LL)
#define HEADER_EVERY_N        200
#define SERVICE_PERIOD_MS     50     /* worst-case latency to notice a HELLO */

#define CSV_HEADER "bmc_id,seq,t_us,temp_c,power_w,fan_duty_pct,fan_en,fan_rpm,drops,flags\n"

typedef struct {
    struct sockaddr_in addr;
    int64_t last_hello_us;
    bool need_header;
    bool in_use;
} subscriber_t;

static subscriber_t s_subs[MAX_SUBSCRIBERS];

static void service_hello(int sock)
{
    char buf[32];
    struct sockaddr_in from;
    socklen_t from_len;
    for (;;) {
        from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            return;                                   /* nothing pending */
        }
        if (n < 5 || memcmp(buf, "HELLO", 5) != 0) {
            continue;
        }
        subscriber_t *slot = NULL;
        for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
            if (s_subs[i].in_use && s_subs[i].addr.sin_addr.s_addr == from.sin_addr.s_addr &&
                s_subs[i].addr.sin_port == from.sin_port) {
                slot = &s_subs[i];
                break;
            }
            if (!s_subs[i].in_use && !slot) {
                slot = &s_subs[i];
            }
        }
        if (!slot) {
            continue;                                 /* table full: ignore extra viewers */
        }
        if (!slot->in_use) {
            slot->in_use = true;
            slot->addr = from;
            slot->need_header = true;
            ESP_LOGI(TAG, "subscriber added: %s:%u", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
        }
        slot->last_hello_us = esp_timer_get_time();
    }
}

static void expire_subscribers(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].in_use && now - s_subs[i].last_hello_us > SUBSCRIBER_TIMEOUT_US) {
            s_subs[i].in_use = false;
            ESP_LOGI(TAG, "subscriber timed out: %s", inet_ntoa(s_subs[i].addr.sin_addr));
        }
    }
}

static int format_record(char *buf, size_t size, const telemetry_sample_t *s)
{
    char temp[16] = "", power[16] = "";
    if (s->temp_valid) {
        snprintf(temp, sizeof(temp), "%.4f", s->temp_c);
    }
    if (s->power_valid) {
        snprintf(power, sizeof(power), "%.3f", s->power_w);
    }
    return snprintf(buf, size, "%s,%lu,%lld,%s,%s,%.1f,%d,%lu,%lu,%lu\n",
                    CONFIG_BMC_STREAM_ID, (unsigned long)s->seq, (long long)s->t_us, temp, power,
                    s->fan_duty_pct, s->fan_enabled ? 1 : 0, (unsigned long)s->fan_rpm,
                    (unsigned long)telemetry_drops(), (unsigned long)s->flags);
}

static void send_to(int sock, subscriber_t *sub, const char *data, int len)
{
    if (sendto(sock, data, len, 0, (struct sockaddr *)&sub->addr, sizeof(sub->addr)) < 0) {
        telemetry_count_drop();                       /* ENOMEM/EAGAIN: never block, just count */
    }
}

static void stream_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_BMC_STREAM_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "bind() failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);         /* sendto must never block either */
    ESP_LOGI(TAG, "streaming on UDP :%d on core %d (waiting for HELLO)", CONFIG_BMC_STREAM_PORT,
             xPortGetCoreID());

    unsigned sent = 0;
    for (;;) {
        telemetry_sample_t s;
        bool have = telemetry_receive(&s, pdMS_TO_TICKS(SERVICE_PERIOD_MS));
        service_hello(sock);
        expire_subscribers();
        if (!have) {
            continue;
        }

        bool any = false;
        for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
            any |= s_subs[i].in_use;
        }
        if (!any) {
            continue;                                 /* nobody listening: not a loss */
        }
        if (!bmc_net_ready()) {
            telemetry_count_drop();
            continue;
        }

        char line[160];
        int len = format_record(line, sizeof(line), &s);
        if (len <= 0 || len >= (int)sizeof(line)) {
            telemetry_count_drop();
            continue;
        }
        bool periodic_header = (sent % HEADER_EVERY_N) == 0;
        for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
            subscriber_t *sub = &s_subs[i];
            if (!sub->in_use) {
                continue;
            }
            if (sub->need_header || periodic_header) {
                send_to(sock, sub, CSV_HEADER, sizeof(CSV_HEADER) - 1);
                sub->need_header = false;
            }
            send_to(sock, sub, line, len);
        }
        sent++;
    }
}

esp_err_t bmc_stream_start(int core, UBaseType_t priority)
{
    BaseType_t ok = xTaskCreatePinnedToCore(stream_task, "net_tx", 6144, NULL, priority, NULL, core);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
