#ifndef FABRIC_SHIM_H
#define FABRIC_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shim_endpoint shim_endpoint_t;
typedef struct shim_mr shim_mr_t;
typedef struct shim_op shim_op_t;
typedef struct shim_info shim_info_t;
typedef struct shim_domain shim_domain_t;
typedef struct shim_fabric shim_fabric_t;
typedef struct shim_cq shim_cq_t;
typedef struct shim_eq shim_eq_t;
typedef struct shim_listener shim_listener_t;

/* All handles belong to one driving thread. No function waits for completion.
 * Open/post/close return 0 on success, negative libfabric errors on failure.
 * Close accepts NULL; success consumes the handle, failure keeps it alive.
 * Never use a consumed handle. Buffers remain owned by the caller.
 */
#define SHIM_ACCESS_SEND 1u
#define SHIM_ACCESS_RECV 2u
#define SHIM_EVENT_CONNREQ 1u
#define SHIM_EVENT_CONNECTED 2u
#define SHIM_EVENT_SHUTDOWN 3u
#define SHIM_EVENT_ERROR 4u
#define SHIM_COMPLETION_SEND 1u
#define SHIM_COMPLETION_RECV 2u
#define SHIM_ERROR_TEXT_SIZE 256

typedef struct shim_completion {
    uint64_t op_id;
    uint64_t flags;                 /* normalized SHIM_COMPLETION_* flag */
    size_t len;                     /* valid bytes for successful receives */
    int32_t err;                    /* positive libfabric error, 0 on success */
    int32_t prov_err;
    char message[SHIM_ERROR_TEXT_SIZE];
} shim_completion_t;

typedef struct shim_connect_event {
    uint32_t type;
    uintptr_t source;              /* listener/endpoint handle identity; never dereference */
    shim_info_t *info;              /* owned request, only for CONNREQ */
    int32_t err;
    int32_t prov_err;
    char message[SHIM_ERROR_TEXT_SIZE];
} shim_connect_event_t;

uint32_t shim_fabric_version(void);
const char *shim_error_string(int error);
int shim_error_is_again(int error);

/* Query and print device information without retaining resources. */
int shim_print_info(const char *local_ip, const char *domain_name);

/* Query one verbs/FI_EP_MSG configuration; is_source selects local addressing.
 * domain_name filters a local device on both server and client.
 * On failure *out is NULL. This function does not create an endpoint.
 */
int shim_info_open(const char *node, const char *service,
                   const char *domain_name, int is_source, shim_info_t **out);
/* A CONNREQ must be accepted or rejected before freeing its info.
 * Freeing info alone does not reject a pending connection request.
 */
void shim_info_free(shim_info_t *info);
int shim_fabric_open(shim_info_t *info, shim_fabric_t **out);
int shim_domain_open(shim_fabric_t *fabric, shim_info_t *info, shim_domain_t **out);
int shim_eq_open(shim_fabric_t *fabric, shim_eq_t **out);
int shim_cq_open(shim_domain_t *domain, size_t capacity, shim_cq_t **out);

/* Create a passive endpoint, bind its EQ and start listening.
 * Composite opens normally roll back on failure. If rollback close fails,
 * *out remains non-NULL: use only the matching close function to retry.
 */
int shim_listener_open(shim_fabric_t *fabric, shim_info_t *info,
                       shim_eq_t *eq, shim_listener_t **out);
/* Create an active endpoint, bind separate TX/RX CQs and EQ, then enable.
 * Each CQ is dedicated to one endpoint, including after endpoint close.
 * Use CONNREQ info on the server. Failed request setup retains the endpoint:
 * reject the request before closing that cleanup-only handle. Other rollback
 * follows listener_open rules.
 */
int shim_endpoint_open(shim_domain_t *domain, shim_info_t *info,
                       shim_eq_t *eq, shim_cq_t *tx_cq, shim_cq_t *rx_cq,
                       shim_endpoint_t **out);
int shim_connect(shim_endpoint_t *ep, shim_info_t *info);
int shim_accept(shim_endpoint_t *ep);
int shim_reject(shim_listener_t *listener, shim_info_t *info);

/* Poll returns 1 for an event, 0 when empty, negative on a polling failure.
 * An ERROR event is still a returned event; inspect err and prov_err.
 * CONNREQ transfers info ownership to the caller. Private CM data is unused.
 * Poll EQ while its endpoint/listener source handles are still alive.
 */
int shim_eq_poll(shim_eq_t *eq, shim_connect_event_t *out);
int shim_cq_poll(shim_cq_t *cq, shim_completion_t *out);

/* Register stable caller-owned memory. access uses SHIM_ACCESS_* flags.
 * Do not release memory until MR close succeeds, or resize it while registered.
 */
int shim_mr_register(shim_domain_t *domain, void *buf, size_t capacity,
                      uint32_t access, shim_mr_t **out);
int shim_op_create(uint64_t id, shim_op_t **out);
/* Returns -FI_EBUSY while a CQ can still refer to this context. */
int shim_op_free(shim_op_t *op);
/* A successful post borrows buf, MR and op until completion or EP close.
 * Do not touch receive memory or modify send memory while in flight.
 * On -FI_EAGAIN nothing was submitted; drive EQ/CQs before retrying.
 */
int shim_post_send(shim_endpoint_t *ep, shim_op_t *op, shim_mr_t *mr,
                    size_t len, const void *buf);
int shim_post_recv(shim_endpoint_t *ep, shim_op_t *op, shim_mr_t *mr,
                    size_t capacity, void *buf);

/* Stop submission before shutdown. Timeout/cancel closes the whole endpoint;
 * verbs MSG in libfabric 1.11 does not support fi_cancel.
 * After EP close, close its CQs before freeing unfinished operation contexts.
 * Parent resources return -FI_EBUSY while dependent handles remain alive.
 */
int shim_shutdown(shim_endpoint_t *ep);
int shim_endpoint_close(shim_endpoint_t *ep);
int shim_listener_close(shim_listener_t *listener);
int shim_mr_close(shim_mr_t *mr);
int shim_cq_close(shim_cq_t *cq);
int shim_eq_close(shim_eq_t *eq);
int shim_domain_close(shim_domain_t *domain);
int shim_fabric_close(shim_fabric_t *fabric);

#ifdef __cplusplus
}
#endif
#endif
