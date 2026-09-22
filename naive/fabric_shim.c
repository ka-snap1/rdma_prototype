#include "fabric_shim.h"

#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "rdma/fabric.h"
#include <rdma/fi_errno.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <stddef.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_eq.h>

struct shim_info {
    struct fi_info *raw;
};

/* Avoid feature macro requirements from strdup. */
static char *copy_string(const char *s) {
    size_t len = strlen(s) +1;
    char *copy = malloc(len);
    if(copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}
uint32_t shim_fabric_version(void) {
    return fi_version();
}
static const char *display_name(const char *s) {
    return s != NULL ? s : "(null)";
}
int shim_print_info(const char *local_ip, const char *domain_name) {
    struct fi_info *hints = NULL;
    struct fi_info *results = NULL;
    int ret;
    if(local_ip ==NULL || local_ip[0] == '\0') {
        fprintf(stderr, "local_ip is NULL or empty\n");
        return -FI_EINVAL;
    }
    hints = fi_allocinfo();
    if(hints == NULL) {
        return -FI_ENOMEM;
    }
    hints->caps = FI_MSG;
    hints->ep_attr->type = FI_EP_MSG;
    hints->addr_format = FI_SOCKADDR_IN;
    /*
    Register send/receive buffers and provide MR descriptors for data operations.
    */
    hints->domain_attr->mr_mode = FI_MR_LOCAL;

    hints->fabric_attr->prov_name = copy_string("verbs");
    if(hints->fabric_attr->prov_name == NULL) {
        ret = -FI_ENOMEM;
        goto out;
    }
    if(domain_name != NULL){
        hints->domain_attr->name = copy_string(domain_name);
        if(hints->domain_attr->name == NULL) {
            ret = -FI_ENOMEM;
            goto out;
        }
    }
    ret = fi_getinfo(
        FI_VERSION(1, 11),
        local_ip,
        NULL,
        FI_SOURCE,
        hints,
        &results
    );

    if (ret != 0) {
        fprintf(stderr, "fi_getinfo: %s (%d)\n",
                fi_strerror(-ret), ret);
        goto out;
    }

    unsigned int index = 0;

    for (const struct fi_info *p = results; p != NULL; p = p->next) {
        printf("candidate[%u]\n", index++);
        printf("  provider: %s\n",
               display_name(p->fabric_attr->prov_name));
        printf("  fabric:   %s\n",
               display_name(p->fabric_attr->name));
        printf("  domain:   %s\n",
               display_name(p->domain_attr->name));

        printf("  endpoint: %s\n",
               p->ep_attr->type == FI_EP_MSG
                   ? "FI_EP_MSG" : "unexpected");

        printf("  caps:     0x%" PRIx64 "\n", p->caps);
        printf("  mode:     0x%" PRIx64 "\n", p->mode);
        printf("  mr_mode:  0x%x\n",
               (unsigned int)p->domain_attr->mr_mode);

        printf("  max_msg_size: %zu\n", p->ep_attr->max_msg_size);
        printf("  tx_size:      %zu\n", p->tx_attr->size);
        printf("  rx_size:      %zu\n", p->rx_attr->size);
        printf("  inject_size:  %zu\n", p->tx_attr->inject_size);
    }

out:
   if(results != NULL){fi_freeinfo(results);}
   fi_freeinfo(hints);
   return ret;
}

int shim_info_open(
    const char *node,
    const char *service,
    const char *domain_name,
    int is_source,
    shim_info_t **out
) {
    struct fi_info *hints = NULL;
    struct fi_info *results = NULL;
    const struct fi_info *selected = NULL;
    shim_info_t *wrapper = NULL;
    int ret;
    if(out == NULL) {
        return -FI_EINVAL;
    }
    *out = NULL;
    /*requires explicit specification of the address and port*/
    if(node ==NULL || node[0] == '\0') {
        fprintf(stderr, "node is NULL or empty\n");
        return -FI_EINVAL;
    }
    if(service == NULL || service[0] == '\0') {
        fprintf(stderr, "service is NULL or empty\n");
        return -FI_EINVAL;
    }
    if(is_source != 0 && is_source != 1) {
        fprintf(stderr, "is_source must be 0 or 1\n");
        return -FI_EINVAL;
    }
    if(domain_name != NULL && domain_name[0] == '\0') {
        fprintf(stderr, "domain_name is empty\n");
        return -FI_EINVAL;
    }

    hints = fi_allocinfo();
    if(hints == NULL) {
        return -FI_ENOMEM;
    }
    hints->caps = FI_MSG;
    hints->ep_attr->type = FI_EP_MSG;
    hints->addr_format = FI_SOCKADDR_IN;
    /*
    Register send/receive buffers and provide MR descriptors for data operations.
    */
    hints->domain_attr->mr_mode = FI_MR_LOCAL;
    hints->fabric_attr->prov_name = copy_string("verbs");
    if(hints->fabric_attr->prov_name == NULL) {
        ret = -FI_ENOMEM;
        goto cleanup;
    }
    if(domain_name != NULL){
        hints->domain_attr->name = copy_string(domain_name);
        if(hints->domain_attr->name == NULL) {
            ret = -FI_ENOMEM;
            goto cleanup;
        }
    }
    ret = fi_getinfo(
        FI_VERSION(1, 11),
        node,
        service,
        is_source ? FI_SOURCE : 0,
        hints,
        &results
    );
    if (ret != 0) {
        fprintf(stderr, "fi_getinfo: %s (%d)\n",
                fi_strerror(-ret), ret);
        goto cleanup;
    }
    for (const struct fi_info *p = results; p != NULL; p = p->next) {
        if (p->fabric_attr == NULL ||
            p->domain_attr == NULL ||
            p->ep_attr == NULL ||
            p->tx_attr == NULL ||
            p->rx_attr == NULL) {
            continue;
        }

        if (p->fabric_attr->prov_name == NULL ||
            strcmp(p->fabric_attr->prov_name, "verbs") != 0) {
            continue;
        }

        if (p->ep_attr->type != FI_EP_MSG ||
            (p->caps & FI_MSG) == 0 ||
            p->addr_format != FI_SOCKADDR_IN) {
            continue;
        }

        if (domain_name != NULL &&
            (p->domain_attr->name == NULL ||
             strcmp(p->domain_attr->name, domain_name) != 0)) {
            continue;
        }

        if (is_source) {
            if (p->src_addr == NULL || p->src_addrlen == 0)
                continue;
        } else {
            if (p->dest_addr == NULL || p->dest_addrlen == 0)
                continue;
        }

        selected = p;
        break;
    }

    if (selected == NULL) {
        ret = -FI_ENODATA;
        goto cleanup;
    }

    wrapper = calloc(1, sizeof(*wrapper));
    if (wrapper == NULL) {
        ret = -FI_ENOMEM;
        goto cleanup;
    }

    /*
     * Copy one configuration before releasing the result list.
     */
    wrapper->raw = fi_dupinfo(selected);
    if (wrapper->raw == NULL) {
        ret = -FI_ENOMEM;
        goto cleanup;
    }

    /* Transfer ownership to the caller. */
    *out = wrapper;
    wrapper = NULL;
    ret = 0;

cleanup:
   if (wrapper != NULL) {
       if (wrapper->raw != NULL) fi_freeinfo(wrapper->raw);
       free(wrapper);
   }
   if(results != NULL){fi_freeinfo(results);}
   fi_freeinfo(hints);
   return ret;
}
void shim_info_free(shim_info_t *info)
{
    if (info == NULL)
        return;

    if (info->raw != NULL)
        fi_freeinfo(info->raw);

    free(info);
}

/* Parent references prevent releasing a domain/fabric before its children. */
struct shim_fabric { struct fid_fabric *raw; size_t children; };
struct shim_domain {
    struct fid_domain *raw;
    shim_fabric_t *fabric;
    size_t children;
};
struct shim_eq { struct fid_eq *raw; shim_fabric_t *fabric; size_t users; };
struct shim_cq {
    struct fid_cq *raw;
    shim_domain_t *domain;
    shim_op_t *pending;
    size_t capacity;
    size_t count;
    int used;
    int attached;
    int retired;
};
struct shim_listener {
    struct fid_pep *raw;
    shim_fabric_t *fabric;
    shim_eq_t *eq;
    int ready;
};
struct shim_endpoint {
    struct fid_ep *raw;
    shim_domain_t *domain;
    shim_eq_t *eq;
    shim_cq_t *tx;
    shim_cq_t *rx;
    size_t max_msg;
    int ready;
    int closing;
};
struct shim_mr {
    struct fid_mr *raw;
    shim_domain_t *domain;
    void *buffer;
    size_t capacity;
    size_t pending;
    uint32_t access;
};
struct shim_op {
    struct fi_context context;      /* first member matches op_context */
    uint64_t id;
    uint64_t direction;
    size_t len;
    shim_cq_t *cq;
    shim_mr_t *mr;
    shim_op_t *next;
};

const char *shim_error_string(int error)
{
    /* libfabric errors are small positive values or their negative form. */
    if (error == INT32_MIN) return "invalid error code";
    return fi_strerror(error < 0 ? -error : error);
}

int shim_error_is_again(int error)
{
    return error == -FI_EAGAIN;
}

int shim_fabric_open(shim_info_t *info, shim_fabric_t **out)
{
    if (out == NULL) return -FI_EINVAL;
    *out = NULL;
    if (info == NULL || info->raw == NULL) return -FI_EINVAL;
    shim_fabric_t *obj = calloc(1, sizeof(*obj));
    if (obj == NULL) return -FI_ENOMEM;
    int ret = fi_fabric(info->raw->fabric_attr, &obj->raw, obj);
    if (ret != 0) { free(obj); return ret; }
    *out = obj;
    return 0;
}

int shim_domain_open(shim_fabric_t *fabric, shim_info_t *info, shim_domain_t **out)
{
    if (out == NULL) return -FI_EINVAL;
    *out = NULL;
    if (fabric == NULL || info == NULL) return -FI_EINVAL;
    shim_domain_t *obj = calloc(1, sizeof(*obj));
    if (obj == NULL) return -FI_ENOMEM;
    int ret = fi_domain(fabric->raw, info->raw, &obj->raw, obj);
    if (ret != 0) { free(obj); return ret; }
    obj->fabric = fabric;
    fabric->children++;
    *out = obj;
    return 0;
}

int shim_eq_open(shim_fabric_t *fabric, shim_eq_t **out)
{
    if (out == NULL) return -FI_EINVAL;
    *out = NULL;
    if (fabric == NULL) return -FI_EINVAL;
    shim_eq_t *obj = calloc(1, sizeof(*obj));
    if (obj == NULL) return -FI_ENOMEM;
    struct fi_eq_attr attr = {0};
    attr.wait_obj = FI_WAIT_NONE;
    int ret = fi_eq_open(fabric->raw, &attr, &obj->raw, obj);
    if (ret != 0) { free(obj); return ret; }
    obj->fabric = fabric;
    fabric->children++;
    *out = obj;
    return 0;
}

int shim_cq_open(shim_domain_t *domain, size_t capacity, shim_cq_t **out)
{
    if (out == NULL) return -FI_EINVAL;
    *out = NULL;
    if (domain == NULL || capacity == 0) return -FI_EINVAL;
    shim_cq_t *obj = calloc(1, sizeof(*obj));
    if (obj == NULL) return -FI_ENOMEM;
    struct fi_cq_attr attr = {0};
    attr.format = FI_CQ_FORMAT_MSG;
    attr.size = capacity;
    attr.wait_obj = FI_WAIT_NONE;
    int ret = fi_cq_open(domain->raw, &attr, &obj->raw, obj);
    if (ret != 0) { free(obj); return ret; }
    obj->domain = domain;
    obj->capacity = capacity;
    domain->children++;
    *out = obj;
    return 0;
}

int shim_listener_open(shim_fabric_t *fabric, shim_info_t *info,
                       shim_eq_t *eq, shim_listener_t **out)
{
    if (out == NULL) return -FI_EINVAL;
    *out = NULL;
    if (fabric == NULL || info == NULL || eq == NULL || eq->fabric != fabric)
        return -FI_EINVAL;
    shim_listener_t *obj = calloc(1, sizeof(*obj));
    if (obj == NULL) return -FI_ENOMEM;
    int ret = fi_passive_ep(fabric->raw, info->raw, &obj->raw, obj);
    if (ret != 0) { free(obj); return ret; }
    obj->fabric = fabric;
    obj->eq = eq;
    fabric->children++;
    eq->users++;
    ret = fi_pep_bind(obj->raw, &eq->raw->fid, 0);
    if (ret == 0) ret = fi_listen(obj->raw);
    if (ret != 0) {
        /* Keep the handle reachable if the provider refuses rollback. */
        if (shim_listener_close(obj) != 0) *out = obj;
        return ret;
    }
    obj->ready = 1;
    *out = obj;
    return 0;
}

int shim_endpoint_open(shim_domain_t *domain, shim_info_t *info,
                       shim_eq_t *eq, shim_cq_t *tx, shim_cq_t *rx,
                       shim_endpoint_t **out)
{
    if (out == NULL) return -FI_EINVAL;
    *out = NULL;
    if (domain == NULL || info == NULL || eq == NULL || tx == NULL || rx == NULL ||
        tx == rx || tx->domain != domain || rx->domain != domain ||
        eq->fabric != domain->fabric || tx->used || rx->used)
        return -FI_EINVAL;
    /* This shim reports every operation and does not support message prefixes. */
    if ((info->raw->mode & ~(FI_CONTEXT | FI_LOCAL_MR)) != 0 ||
        info->raw->tx_attr->op_flags != 0 || info->raw->rx_attr->op_flags != 0)
        return -FI_ENOSYS;
    shim_endpoint_t *obj = calloc(1, sizeof(*obj));
    if (obj == NULL) return -FI_ENOMEM;
    int ret = fi_endpoint(domain->raw, info->raw, &obj->raw, obj);
    if (ret != 0) { free(obj); return ret; }
    obj->domain = domain;
    obj->eq = eq;
    obj->tx = tx;
    obj->rx = rx;
    obj->max_msg = info->raw->ep_attr->max_msg_size;
    domain->children++;
    eq->users++;
    tx->used = rx->used = tx->attached = rx->attached = 1;
    ret = fi_ep_bind(obj->raw, &eq->raw->fid, 0);
    if (ret == 0) ret = fi_ep_bind(obj->raw, &tx->raw->fid, FI_TRANSMIT);
    if (ret == 0) ret = fi_ep_bind(obj->raw, &rx->raw->fid, FI_RECV);
    if (ret == 0) ret = fi_enable(obj->raw);
    if (ret != 0) {
        if (shim_endpoint_close(obj) != 0) *out = obj;
        return ret;
    }
    obj->ready = 1;
    *out = obj;
    return 0;
}

int shim_connect(shim_endpoint_t *ep, shim_info_t *info)
{
    if (ep == NULL || !ep->ready || ep->closing || info == NULL ||
        info->raw->dest_addr == NULL) return -FI_EINVAL;
    return fi_connect(ep->raw, info->raw->dest_addr, NULL, 0);
}

int shim_accept(shim_endpoint_t *ep)
{
    if (ep == NULL || !ep->ready || ep->closing) return -FI_EINVAL;
    return fi_accept(ep->raw, NULL, 0);
}

int shim_reject(shim_listener_t *listener, shim_info_t *info)
{
    if (listener == NULL || !listener->ready || info == NULL ||
        info->raw->handle == NULL) return -FI_EINVAL;
    int ret = fi_reject(listener->raw, info->raw->handle, NULL, 0);
    if (ret == 0) info->raw->handle = NULL;
    return ret;
}

int shim_eq_poll(shim_eq_t *eq, shim_connect_event_t *out)
{
    if (eq == NULL || out == NULL) return -FI_EINVAL;
    memset(out, 0, sizeof(*out));
    /* Allocate the request wrapper before consuming a possible CONNREQ. */
    shim_info_t *request = calloc(1, sizeof(*request));
    if (request == NULL) return -FI_ENOMEM;
    struct fi_eq_cm_entry entry = {0};
    uint32_t event = 0;
    ssize_t ret = fi_eq_read(eq->raw, &event, &entry, sizeof(entry), 0);
    if (ret == -FI_EAVAIL) {
        struct fi_eq_err_entry error = {0};
        ret = fi_eq_readerr(eq->raw, &error, 0);
        free(request);
        if (ret == -FI_EAGAIN || ret == 0) return 0;
        if (ret < 0) return (int)ret;
        out->type = SHIM_EVENT_ERROR;
        out->source = error.fid != NULL ? (uintptr_t)error.fid->context : 0;
        out->err = error.err;
        out->prov_err = error.prov_errno;
        char text[SHIM_ERROR_TEXT_SIZE] = {0};
        const char *message = fi_eq_strerror(eq->raw, error.prov_errno,
                                            error.err_data, text, sizeof(text));
        snprintf(out->message, sizeof(out->message), "%s",
                 message != NULL ? message : fi_strerror(error.err));
        return 1;
    }
    if (ret == -FI_EAGAIN || ret == 0) { free(request); return 0; }
    if (ret < 0) { free(request); return (int)ret; }
    if ((size_t)ret < sizeof(entry)) { free(request); return -FI_EIO; }
    out->source = entry.fid != NULL ? (uintptr_t)entry.fid->context : 0;
    switch (event) {
    case FI_CONNREQ:
        if (entry.info == NULL) { free(request); return -FI_EIO; }
        request->raw = entry.info;
        out->info = request;
        out->type = SHIM_EVENT_CONNREQ;
        return 1;
    case FI_CONNECTED: out->type = SHIM_EVENT_CONNECTED; break;
    case FI_SHUTDOWN: out->type = SHIM_EVENT_SHUTDOWN; break;
    default:
        if (entry.info != NULL) fi_freeinfo(entry.info);
        free(request);
        return -FI_ENOSYS;
    }
    if (entry.info != NULL) fi_freeinfo(entry.info);
    free(request);
    return 1;
}

/* Detach one completed context, making its MR and operation reusable. */
static void release_op(shim_op_t *op)
{
    if (op->mr != NULL) op->mr->pending--;
    op->mr = NULL;
    op->cq = NULL;
    op->next = NULL;
}

int shim_cq_poll(shim_cq_t *cq, shim_completion_t *out)
{
    if (cq == NULL || out == NULL) return -FI_EINVAL;
    memset(out, 0, sizeof(*out));
    /* EP close may discard completions; keep old contexts until CQ close. */
    if (cq->retired) return -FI_EOPBADSTATE;
    struct fi_cq_msg_entry entry = {0};
    ssize_t ret = fi_cq_read(cq->raw, &entry, 1);
    void *context = NULL;
    if (ret == -FI_EAVAIL) {
        struct fi_cq_err_entry error = {0};
        ret = fi_cq_readerr(cq->raw, &error, 0);
        if (ret == -FI_EAGAIN || ret == 0) return 0;
        if (ret < 0) return (int)ret;
        context = error.op_context;
        out->err = error.err;
        out->prov_err = error.prov_errno;
        char text[SHIM_ERROR_TEXT_SIZE] = {0};
        const char *message = fi_cq_strerror(cq->raw, error.prov_errno,
                                            error.err_data, text, sizeof(text));
        snprintf(out->message, sizeof(out->message), "%s",
                 message != NULL ? message : fi_strerror(error.err));
    } else {
        if (ret == -FI_EAGAIN || ret == 0) return 0;
        if (ret < 0) return (int)ret;
        context = entry.op_context;
        out->len = entry.len;
    }
    /* Compare addresses before dereferencing provider-returned context. */
    shim_op_t **link = &cq->pending;
    while (*link != NULL && &(*link)->context != context) link = &(*link)->next;
    if (*link == NULL) return -FI_EIO;
    shim_op_t *op = *link;
    *link = op->next;
    cq->count--;
    out->op_id = op->id;
    out->flags = op->direction;
    if (out->err != 0) out->len = 0;
    else if (op->direction == SHIM_COMPLETION_SEND) out->len = op->len;
    release_op(op);
    return 1;
}

int shim_mr_register(shim_domain_t *domain, void *buf, size_t capacity,
                      uint32_t access, shim_mr_t **out)
{
    if (out == NULL) return -FI_EINVAL;
    *out = NULL;
    if (domain == NULL || buf == NULL || capacity == 0 || access == 0 ||
        (access & ~(SHIM_ACCESS_SEND | SHIM_ACCESS_RECV)) != 0)
        return -FI_EINVAL;
    shim_mr_t *obj = calloc(1, sizeof(*obj));
    if (obj == NULL) return -FI_ENOMEM;
    uint64_t flags = 0;
    if (access & SHIM_ACCESS_SEND) flags |= FI_SEND;
    if (access & SHIM_ACCESS_RECV) flags |= FI_RECV;
    int ret = fi_mr_reg(domain->raw, buf, capacity, flags, 0, 0, 0, &obj->raw, obj);
    if (ret != 0) { free(obj); return ret; }
    obj->domain = domain;
    obj->buffer = buf;
    obj->capacity = capacity;
    obj->access = access;
    domain->children++;
    *out = obj;
    return 0;
}

int shim_op_create(uint64_t id, shim_op_t **out)
{
    if (out == NULL) return -FI_EINVAL;
    *out = calloc(1, sizeof(**out));
    if (*out == NULL) return -FI_ENOMEM;
    (*out)->id = id;
    return 0;
}

int shim_op_free(shim_op_t *op)
{
    if (op == NULL) return 0;
    if (op->cq != NULL) return -FI_EBUSY;
    free(op);
    return 0;
}

static int post_message(shim_endpoint_t *ep, shim_op_t *op, shim_mr_t *mr,
                        size_t len, const void *buf, int receive)
{
    if (ep == NULL || op == NULL || mr == NULL || buf == NULL ||
        !ep->ready || ep->closing || mr->domain != ep->domain)
        return -FI_EINVAL;
    if (op->cq != NULL) return -FI_EBUSY;
    uint32_t access = receive ? SHIM_ACCESS_RECV : SHIM_ACCESS_SEND;
    if ((mr->access & access) == 0) return -FI_EACCES;
    uintptr_t start = (uintptr_t)mr->buffer;
    uintptr_t address = (uintptr_t)buf;
    if (address < start || address - start > mr->capacity ||
        len > mr->capacity - (address - start) || len > ep->max_msg)
        return -FI_EINVAL;
    shim_cq_t *cq = receive ? ep->rx : ep->tx;
    /* Reserve a completion slot for each successful submission. */
    if (cq->count >= cq->capacity) return -FI_EAGAIN;
    memset(&op->context, 0, sizeof(op->context));
    ssize_t ret = receive
        ? fi_recv(ep->raw, (void *)buf, len, fi_mr_desc(mr->raw),
                  FI_ADDR_UNSPEC, &op->context)
        : fi_send(ep->raw, buf, len, fi_mr_desc(mr->raw),
                  FI_ADDR_UNSPEC, &op->context);
    if (ret != 0) return (int)ret;
    op->direction = receive ? SHIM_COMPLETION_RECV : SHIM_COMPLETION_SEND;
    op->len = len;
    op->mr = mr;
    op->cq = cq;
    op->next = cq->pending;
    cq->pending = op;
    cq->count++;
    mr->pending++;
    return 0;
}

int shim_post_send(shim_endpoint_t *ep, shim_op_t *op, shim_mr_t *mr,
                    size_t len, const void *buf)
{
    return post_message(ep, op, mr, len, buf, 0);
}

int shim_post_recv(shim_endpoint_t *ep, shim_op_t *op, shim_mr_t *mr,
                    size_t capacity, void *buf)
{
    return post_message(ep, op, mr, capacity, buf, 1);
}

int shim_shutdown(shim_endpoint_t *ep)
{
    if (ep == NULL || !ep->ready) return -FI_EINVAL;
    ep->closing = 1;
    return fi_shutdown(ep->raw, 0);
}

/* Endpoint close releases buffer borrows, but CQ contexts remain pinned. */
static void retire_cq(shim_cq_t *cq)
{
    cq->attached = 0;
    cq->retired = 1;
    for (shim_op_t *op = cq->pending; op != NULL; op = op->next) {
        if (op->mr != NULL) op->mr->pending--;
        op->mr = NULL;
    }
}

int shim_endpoint_close(shim_endpoint_t *ep)
{
    if (ep == NULL) return 0;
    ep->closing = 1;
    int ret = fi_close(&ep->raw->fid);
    if (ret != 0) return ret;
    retire_cq(ep->tx);
    retire_cq(ep->rx);
    ep->eq->users--;
    ep->domain->children--;
    free(ep);
    return 0;
}

int shim_listener_close(shim_listener_t *listener)
{
    if (listener == NULL) return 0;
    int ret = fi_close(&listener->raw->fid);
    if (ret != 0) return ret;
    listener->eq->users--;
    listener->fabric->children--;
    free(listener);
    return 0;
}

int shim_mr_close(shim_mr_t *mr)
{
    if (mr == NULL) return 0;
    if (mr->pending != 0) return -FI_EBUSY;
    int ret = fi_close(&mr->raw->fid);
    if (ret != 0) return ret;
    mr->domain->children--;
    free(mr);
    return 0;
}

int shim_cq_close(shim_cq_t *cq)
{
    if (cq == NULL) return 0;
    if (cq->attached) return -FI_EBUSY;
    int ret = fi_close(&cq->raw->fid);
    if (ret != 0) return ret;
    while (cq->pending != NULL) {
        shim_op_t *op = cq->pending;
        cq->pending = op->next;
        release_op(op);
    }
    cq->domain->children--;
    free(cq);
    return 0;
}

int shim_eq_close(shim_eq_t *eq)
{
    if (eq == NULL) return 0;
    if (eq->users != 0) return -FI_EBUSY;
    int ret = fi_close(&eq->raw->fid);
    if (ret != 0) return ret;
    eq->fabric->children--;
    free(eq);
    return 0;
}

int shim_domain_close(shim_domain_t *domain)
{
    if (domain == NULL) return 0;
    if (domain->children != 0) return -FI_EBUSY;
    int ret = fi_close(&domain->raw->fid);
    if (ret != 0) return ret;
    domain->fabric->children--;
    free(domain);
    return 0;
}

int shim_fabric_close(shim_fabric_t *fabric)
{
    if (fabric == NULL) return 0;
    if (fabric->children != 0) return -FI_EBUSY;
    int ret = fi_close(&fabric->raw->fid);
    if (ret != 0) return ret;
    free(fabric);
    return 0;
}
