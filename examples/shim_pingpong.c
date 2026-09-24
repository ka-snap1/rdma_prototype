/* Standalone two-process smoke test for the C shim.
 * Usage: shim_pingpong server|client <server-ip> <port> [local-domain]
 * Each wait has a five-second deadline; no private CM data is exchanged.
 */
#define _POSIX_C_SOURCE 200809L
#include "fabric_shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct session {
    shim_info_t *info;
    shim_info_t *request;
    shim_fabric_t *fabric;
    shim_domain_t *domain;
    shim_eq_t *eq;
    shim_cq_t *tx_cq;
    shim_cq_t *rx_cq;
    shim_listener_t *listener;
    shim_endpoint_t *ep;
    shim_mr_t *tx_mr;
    shim_mr_t *rx_mr;
    shim_op_t *tx_op;
    shim_op_t *rx_op;
    char *tx;
    char *rx;
    int tx_done;
    int rx_done;
    size_t rx_len;
    int accepted;
};

static double now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) abort();
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static int check(int ret, const char *operation)
{
    if (ret < 0)
        fprintf(stderr, "%s: %s (%d)\n", operation, shim_error_string(ret), ret);
    return ret;
}

static void pause_poll(void)
{
    struct timespec delay = {.tv_nsec = 1000000};
    nanosleep(&delay, NULL);
}

static int wait_event(struct session *s, uint32_t wanted)
{
    double deadline = now() + 5.0;
    while (now() < deadline) {
        shim_connect_event_t event;
        int ret = shim_eq_poll(s->eq, &event);
        if (check(ret, "EQ poll") < 0) return -1;
        if (ret == 0) { pause_poll(); continue; }
        if (event.type == SHIM_EVENT_CONNREQ) {
            if (wanted == SHIM_EVENT_CONNREQ && s->request == NULL) {
                s->request = event.info;
                return 0;
            }
            if (s->listener != NULL) shim_reject(s->listener, event.info);
            shim_info_free(event.info);
            continue;
        }
        if (event.type == wanted) return 0;
        fprintf(stderr, "unexpected EQ event %u: %s\n", event.type, event.message);
        return -1;
    }
    fprintf(stderr, "connection event timeout\n");
    return -1;
}

/* Drive both CQs before considering a shutdown event. */
static int progress(struct session *s)
{
    shim_completion_t done;
    int ret = shim_cq_poll(s->tx_cq, &done);
    if (check(ret, "TX CQ poll") < 0) return -1;
    if (ret == 1) {
        if (done.err || done.op_id != 1 || done.flags != SHIM_COMPLETION_SEND) {
            fprintf(stderr, "TX completion failed: %s\n", done.message);
            return -1;
        }
        s->tx_done = 1;
    }
    ret = shim_cq_poll(s->rx_cq, &done);
    if (check(ret, "RX CQ poll") < 0) return -1;
    if (ret == 1) {
        if (done.err || done.op_id != 2 || done.flags != SHIM_COMPLETION_RECV) {
            fprintf(stderr, "RX completion failed: %s\n", done.message);
            return -1;
        }
        s->rx_done = 1;
        s->rx_len = done.len;
    }
    if (s->tx_done && s->rx_done) return 0;
    shim_connect_event_t event;
    ret = shim_eq_poll(s->eq, &event);
    if (check(ret, "EQ poll") < 0) return -1;
    if (ret == 1) {
        if (event.type == SHIM_EVENT_CONNREQ) {
            if (s->listener != NULL) shim_reject(s->listener, event.info);
            shim_info_free(event.info);
        } else if (event.type == SHIM_EVENT_ERROR || event.type == SHIM_EVENT_SHUTDOWN) {
            fprintf(stderr, "connection ended before completion: %s\n", event.message);
            return -1;
        }
    }
    return 0;
}

static int post_receive(struct session *s)
{
    s->rx_done = 0;
    double deadline = now() + 5.0;
    while (now() < deadline) {
        int ret = shim_post_recv(s->ep, s->rx_op, s->rx_mr, 64, s->rx);
        if (!shim_error_is_again(ret)) return check(ret, "post receive");
        if (progress(s) < 0) return -1;
        pause_poll();
    }
    fprintf(stderr, "receive submission timeout\n");
    return -1;
}

static int send_message(struct session *s, const char *message)
{
    size_t len = strlen(message);
    if (len > 64 || !s->tx_done) return -1;
    memcpy(s->tx, message, len);
    s->tx_done = 0;
    double deadline = now() + 5.0;
    while (now() < deadline) {
        int ret = shim_post_send(s->ep, s->tx_op, s->tx_mr, len, s->tx);
        if (!shim_error_is_again(ret)) return check(ret, "post send");
        if (progress(s) < 0) return -1;
        pause_poll();
    }
    fprintf(stderr, "send submission timeout\n");
    return -1;
}

static int wait_data(struct session *s, const char *expected)
{
    double deadline = now() + 5.0;
    while (now() < deadline) {
        if (progress(s) < 0) return -1;
        if (s->tx_done && s->rx_done) {
            if (expected != NULL && (s->rx_len != strlen(expected) ||
                memcmp(s->rx, expected, s->rx_len) != 0)) {
                fprintf(stderr, "unexpected message content or length\n");
                return -1;
            }
            return 0;
        }
        pause_poll();
    }
    fprintf(stderr, "data completion timeout\n");
    return -1;
}

/* Close once, report failures and retain dependent memory until process exit.
 * A production driver may retry cleanup within its own deadline.
 */
static int cleanup(struct session *s)
{
    int failed = 0;
    if (s->request != NULL && !s->accepted && s->listener != NULL)
        check(shim_reject(s->listener, s->request), "reject request");
    if (s->ep != NULL) shim_shutdown(s->ep);
#define CLOSE(fn, value) do { \
    if (check(fn(value), #fn) < 0) failed = 1; \
} while (0)
    CLOSE(shim_endpoint_close, s->ep);
    CLOSE(shim_cq_close, s->tx_cq);
    CLOSE(shim_cq_close, s->rx_cq);
    CLOSE(shim_op_free, s->tx_op);
    CLOSE(shim_op_free, s->rx_op);
    if (check(shim_mr_close(s->tx_mr), "TX MR close") == 0) free(s->tx);
    else failed = 1;
    if (check(shim_mr_close(s->rx_mr), "RX MR close") == 0) free(s->rx);
    else failed = 1;
    CLOSE(shim_listener_close, s->listener);
    CLOSE(shim_eq_close, s->eq);
    CLOSE(shim_domain_close, s->domain);
    CLOSE(shim_fabric_close, s->fabric);
#undef CLOSE
    shim_info_free(s->request);
    shim_info_free(s->info);
    return failed ? -1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 4 || argc > 5 ||
        (strcmp(argv[1], "server") != 0 && strcmp(argv[1], "client") != 0)) {
        fprintf(stderr, "usage: %s server|client <server-ip> <port> [local-domain]\n", argv[0]);
        return 2;
    }
    int server = strcmp(argv[1], "server") == 0;
    struct session s = {.tx_done = 1};
    int status = 1;
#define TRY(call) do { if (check((call), #call) < 0) goto out; } while (0)
    TRY(shim_info_open(argv[2], argv[3], argc == 5 ? argv[4] : NULL, server, &s.info));
    TRY(shim_fabric_open(s.info, &s.fabric));
    TRY(shim_eq_open(s.fabric, &s.eq));
    if (server) {
        TRY(shim_listener_open(s.fabric, s.info, s.eq, &s.listener));
        puts("listening; start the client within five seconds");
        fflush(stdout);
        TRY(wait_event(&s, SHIM_EVENT_CONNREQ));
    }
    shim_info_t *active = server ? s.request : s.info;
    TRY(shim_domain_open(s.fabric, active, &s.domain));
    TRY(shim_cq_open(s.domain, 16, &s.tx_cq));
    TRY(shim_cq_open(s.domain, 16, &s.rx_cq));
    TRY(shim_endpoint_open(s.domain, active, s.eq, s.tx_cq, s.rx_cq, &s.ep));
    s.tx = calloc(1, 64);
    s.rx = calloc(1, 64);
    if (s.tx == NULL || s.rx == NULL) goto out;
    TRY(shim_mr_register(s.domain, s.tx, 64, SHIM_ACCESS_SEND, &s.tx_mr));
    TRY(shim_mr_register(s.domain, s.rx, 64, SHIM_ACCESS_RECV, &s.rx_mr));
    TRY(shim_op_create(1, &s.tx_op));
    TRY(shim_op_create(2, &s.rx_op));
    TRY(post_receive(&s));
    if (server) { TRY(shim_accept(s.ep)); s.accepted = 1; }
    else TRY(shim_connect(s.ep, s.info));
    TRY(wait_event(&s, SHIM_EVENT_CONNECTED));
    if (server) {
        TRY(wait_data(&s, "hello"));
        TRY(post_receive(&s));
        TRY(send_message(&s, "world"));
        TRY(wait_data(&s, "ack"));
    } else {
        TRY(send_message(&s, "hello"));
        TRY(wait_data(&s, "world"));
        TRY(send_message(&s, "ack"));
        TRY(wait_data(&s, NULL));
    }
    puts("hello/world passed; TX and RX completions observed");
    status = 0;
out:
    if (cleanup(&s) < 0) status = 1;
    return status;
#undef TRY
}
