#include "bmc_stream.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "bmc_net.h"
#include "bmc_sample.h"
#include "bmc_telemetry.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "bmc_stream";

#define MAX_SUBSCRIBERS       4
#define SUBSCRIBER_TIMEOUT_US (10 * 1000000LL)
#define HEADER_EVERY_N        200
#define SERVICE_PERIOD_MS     50     /* worst-case latency to notice a HELLO or a command */
#define EVENT_LINE_MAX        300
#define EVENT_QUEUE_LEN       8
#define EVENT_RESENDS         3      /* total sends per event */
#define EVENT_RESEND_MS       100
#define MAX_PENDING_EVENTS    8
#define REPLY_MAX             512
#define REPLY_QUEUE_LEN       6

typedef struct {
    struct sockaddr_in addr;
    int64_t last_hello_us;
    bool need_header;
    bool in_use;
} subscriber_t;

typedef struct {
    char line[EVENT_LINE_MAX];
    int len;
} event_msg_t;

typedef struct {
    event_msg_t msg;
    int left;
    int64_t due_us;
} pending_event_t;

typedef struct {
    bmc_peer_t to;
    uint16_t len;
    char text[REPLY_MAX];
} reply_msg_t;

static subscriber_t s_subs[MAX_SUBSCRIBERS];
static pending_event_t s_pending[MAX_PENDING_EVENTS];
static QueueHandle_t s_event_q;
static QueueHandle_t s_reply_q;
static bmc_stream_cmd_cb_t s_cmd_cb;

void bmc_stream_set_command_handler(bmc_stream_cmd_cb_t cb)
{
    s_cmd_cb = cb;
}

/* The caller is the safety task, which must never touch the network stack (a sendto
 * can wait on the TCP/IP thread). It only queues; the net task does the sending. */
void bmc_stream_reply(bmc_peer_t to, const char *text, size_t len)
{
    if (!s_reply_q || len == 0) {
        return;
    }
    reply_msg_t m;
    m.to = to;
    m.len = (uint16_t)(len > REPLY_MAX ? REPLY_MAX : len);
    memcpy(m.text, text, m.len);
    xQueueSend(s_reply_q, &m, 0);          /* full queue: the reply is lost, the host retries */
}

bool bmc_stream_post_event(const char *line, size_t len)
{
    if (!s_event_q || len == 0 || len >= EVENT_LINE_MAX) {
        return false;
    }
    event_msg_t m;
    memcpy(m.line, line, len);
    m.len = (int)len;
    return xQueueSend(s_event_q, &m, 0) == pdTRUE;
}

static subscriber_t *find_subscriber(const struct sockaddr_in *from)
{
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].in_use && s_subs[i].addr.sin_addr.s_addr == from->sin_addr.s_addr &&
            s_subs[i].addr.sin_port == from->sin_port) {
            return &s_subs[i];
        }
    }
    return NULL;
}

static void service_input(int sock)
{
    char buf[96];
    struct sockaddr_in from;
    socklen_t from_len;
    for (;;) {
        from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            return;                                   /* nothing pending */
        }
        if (n >= 5 && memcmp(buf, "HELLO", 5) == 0) {
            subscriber_t *slot = find_subscriber(&from);
            if (!slot) {
                for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
                    if (!s_subs[i].in_use) {
                        slot = &s_subs[i];
                        break;
                    }
                }
            }
            if (!slot) {
                continue;                             /* table full: ignore extra viewers */
            }
            if (!slot->in_use) {
                slot->in_use = true;
                slot->addr = from;
                slot->need_header = true;
                ESP_LOGI(TAG, "subscriber added: %s:%u", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
            }
            slot->last_hello_us = esp_timer_get_time();
            continue;
        }
        /* Anything else is a command, accepted only from a current subscriber. */
        if (s_cmd_cb && find_subscriber(&from)) {
            bmc_peer_t peer = { .ip = from.sin_addr.s_addr, .port = from.sin_port };
            s_cmd_cb(buf, (size_t)n, peer);
        }
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

static void send_to(int sock, subscriber_t *sub, const char *data, int len)
{
    if (sendto(sock, data, len, 0, (struct sockaddr *)&sub->addr, sizeof(sub->addr)) < 0) {
        telemetry_count_drop();                       /* ENOMEM/EAGAIN: never block, just count */
    }
}

static void send_all(int sock, const char *data, int len)
{
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].in_use) {
            send_to(sock, &s_subs[i], data, len);
        }
    }
}

static void service_replies(int sock)
{
    reply_msg_t m;
    while (xQueueReceive(s_reply_q, &m, 0) == pdTRUE) {
        struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = m.to.port };
        dst.sin_addr.s_addr = m.to.ip;
        sendto(sock, m.text, m.len, 0, (struct sockaddr *)&dst, sizeof(dst));   /* best effort */
    }
}

/* New events are sent immediately and then repeated, so a single lost datagram does
 * not hide a failsafe from the operator. */
static void service_events(int sock)
{
    event_msg_t m;
    while (xQueueReceive(s_event_q, &m, 0) == pdTRUE) {
        for (int i = 0; i < MAX_PENDING_EVENTS; i++) {
            if (s_pending[i].left == 0) {
                s_pending[i].msg = m;
                s_pending[i].left = EVENT_RESENDS;
                s_pending[i].due_us = 0;
                break;
            }
        }
    }
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_PENDING_EVENTS; i++) {
        pending_event_t *p = &s_pending[i];
        if (p->left > 0 && now >= p->due_us) {
            if (bmc_net_ready()) {
                send_all(sock, p->msg.line, p->msg.len);
            }
            p->left--;
            p->due_us = now + EVENT_RESEND_MS * 1000LL;
        }
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

    char header[300];
    int header_len = bmc_fmt_header(header, sizeof(header));
    unsigned sent = 0;
    for (;;) {
        telemetry_sample_t s;
        bool have = telemetry_receive(&s, pdMS_TO_TICKS(SERVICE_PERIOD_MS));
        service_input(sock);
        expire_subscribers();
        service_replies(sock);
        service_events(sock);
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

        char line[BMC_FMT_RECORD_MAX];
        int len = bmc_fmt_record(line, sizeof(line), CONFIG_BMC_STREAM_ID, &s, telemetry_drops());
        if (len <= 0) {
            telemetry_count_drop();
            continue;
        }
        bool periodic_header = (sent % HEADER_EVERY_N) == 0;
        for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
            subscriber_t *sub = &s_subs[i];
            if (!sub->in_use) {
                continue;
            }
            if ((sub->need_header || periodic_header) && header_len > 0) {
                send_to(sock, sub, header, header_len);
                sub->need_header = false;
            }
            send_to(sock, sub, line, len);
        }
        sent++;
    }
}

esp_err_t bmc_stream_start(int core, UBaseType_t priority)
{
    s_event_q = xQueueCreate(EVENT_QUEUE_LEN, sizeof(event_msg_t));
    s_reply_q = xQueueCreate(REPLY_QUEUE_LEN, sizeof(reply_msg_t));
    if (!s_event_q || !s_reply_q) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(stream_task, "net_tx", 6144, NULL, priority, NULL, core);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
