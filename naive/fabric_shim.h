#ifndef FABRIC_SHIM_H
#define FABRIC_SHIM_H
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
typedef struct shim_endpoint shim_endpoint_t;
typedef struct shim_mr shim_mr_t;
typedef struct shim_op shim_op_t;
typedef struct shim_info shim_info_t;
typedef struct shim_domain shim_domain_t;
typedef struct shim_fabric shim_fabric_t;
typedef struct shim_cq shim_cq_t;
typedef struct shim_eq shim_eq_t;

typedef struct shim_listener shim_listener_t;
typedef struct shim_connect_event shim_connect_event_t;

int shim_post_send(
    shim_endpoint_t *ep,
    shim_op_t *op,
    shim_mr_t *mr,
    size_t len,
    const void *buf
);
int shim_post_recv(
    shim_endpoint_t *ep,
    shim_op_t *op,
    shim_mr_t *mr,
    size_t capacity,
    void *buf
);

typedef struct shim_completion {
    uint64_t op_id;
    uint64_t flags;
    size_t len;
    int32_t err;
    int32_t prov_err;
} shim_completion_t;

/**
 * @brief get the Device information
 * @param a local_ip: the local ipv4 address to bind to Rxe device
 * @param b domain name: NULL or fiter by domain name in the device list
 * @return 0 on success, negative number on libfabric error 
 */
int shim_print_info(const char *local_ip, const char *domain_name);

/*
* query and open a fabric, domain, endpoint and completion queue
*/
int shim_info_open(
    const char *node,
    const char *service,
    const char *domain_name,
    int is_source,
    shim_info_t **out
);

void shim_info_free(shim_info_t *info);

int shim_fabric_open(
    shim_info_t *info,
    shim_fabric_t **out
);

int shim_domain_open(
    shim_fabric_t *fabric,
    shim_info_t *info,
    shim_domain_t **out
);

int shim_ep_open(
    shim_fabric_t *fabric,
    shim_endpoint_t **out
);

int shim_cq_open(
    shim_domain_t *domain,
    size_t capacity,
    shim_cq_t **out
);

/*listener and build connection*/
int shim_listener_open(
    shim_fabric_t *fabric,
    shim_info_t *info,
    shim_eq_t *eq,
    shim_listener_t **out
);
/*build active_EP, bind EQ\TX\CQ\RX, and enable it*/
int shim_endpoint_open(
    shim_fabric_t *fabric,
    shim_domain_t *domain,
    shim_info_t *info,
    shim_endpoint_t **out
);
/*client use the dest_addr of target in query*/
int shim_connect(
    shim_endpoint_t *ep,
    shim_info_t *info
);
int shim_accept(shim_endpoint_t *ep);
/* request_info from FI_CONNREQ*/
int shim_reject(
    shim_listener_t *listener,
    shim_info_t *info
);

int shim_eq_poll(
    shim_eq_t *eq,
    shim_completion_t *out
);
int shim_cq_poll(
    shim_cq_t *cq,
    shim_completion_t *out
);
/*acccess use send/recv signal defined in shim*/
int shim_mr_register(
    shim_domain_t *domain,
    void *buf,
    size_t capacity,
    uint32_t access,
    shim_mr_t **out
);
/*assigh context to operation*/
int shim_op_create(uint32_t id, shim_op_t **out);
void shim_op_free(shim_op_t *op);

int shim_shutdown(shim_endpoint_t *ep);


int shim_post_send(
    shim_endpoint_t *ep,
    shim_op_t *op,
    shim_mr_t *mr,
    size_t len,
    const void *buf
);

int shim_post_recv(
    shim_endpoint_t *ep,
    shim_op_t *op,
    shim_mr_t *mr,
    size_t capacity,
    void *buf
);

int shim_endpoint_close(shim_endpoint_t *ep);
int shim_listener_close(shim_listener_t *listener);
int shim_mr_close(shim_mr_t *mr);
int shim_cq_close(shim_cq_t *cq);
int shim_eq_close(shim_eq_t *eq);
int shim_domain_close(shim_domain_t *domain);
int shim_fabric_close(shim_fabric_t *fabric);
uint32_t shim_fabric_version(void);
#endif