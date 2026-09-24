/* Exercise in-flight ownership using a deterministic provider operation table.
 * No RXE device is required. Include the implementation only in this test.
 */
#include <assert.h>
#include "../naive/fabric_shim.c"

/* Linker wrappers isolate discovery and inject a duplication failure. */
static int duplicate_failure;
static uint64_t observed_query_flags;
struct fi_info *__real_fi_dupinfo(const struct fi_info *info);
struct fi_info *__wrap_fi_dupinfo(const struct fi_info *info)
{
    if (info != NULL && duplicate_failure) return NULL;
    return __real_fi_dupinfo(info);
}

int __wrap_fi_getinfo(uint32_t version, const char *node, const char *service,
                     uint64_t flags, const struct fi_info *hints,
                     struct fi_info **out)
{
    assert(version == FI_VERSION(1, 11));
    assert(strcmp(node, "192.0.2.1") == 0);
    assert(strcmp(service, "7471") == 0);
    assert(strcmp(hints->fabric_attr->prov_name, "verbs") == 0);
    assert(hints->domain_attr->mr_mode == (FI_MR_LOCAL | FI_MR_ALLOCATED));
    assert(strcmp(hints->domain_attr->name, "rxe_test") == 0);
    observed_query_flags = flags;
    struct fi_info *result = __real_fi_dupinfo(hints);
    assert(result != NULL);
    result->src_addr = calloc(1, 16);
    result->dest_addr = calloc(1, 16);
    assert(result->src_addr != NULL && result->dest_addr != NULL);
    result->src_addrlen = result->dest_addrlen = 16;
    *out = result;
    return 0;
}

static int post_result;
static int close_result;
static int cq_error;
static void *completed_context;

static int fake_close(struct fid *fid)
{
    (void)fid;
    return close_result;
}

static ssize_t fake_send(struct fid_ep *ep, const void *buf, size_t len,
                          void *desc, fi_addr_t addr, void *context)
{
    (void)ep; (void)buf; (void)len; (void)desc; (void)addr; (void)context;
    return post_result;
}

static ssize_t fake_recv(struct fid_ep *ep, void *buf, size_t len,
                          void *desc, fi_addr_t addr, void *context)
{
    return fake_send(ep, buf, len, desc, addr, context);
}

static ssize_t fake_read(struct fid_cq *cq, void *buf, size_t count)
{
    (void)cq; (void)count;
    if (cq_error) return -FI_EAVAIL;
    if (completed_context == NULL) return -FI_EAGAIN;
    struct fi_cq_msg_entry *entry = buf;
    entry->op_context = completed_context;
    entry->len = 5;
    completed_context = NULL;
    return 1;
}

static ssize_t fake_readerr(struct fid_cq *cq, struct fi_cq_err_entry *err,
                             uint64_t flags)
{
    (void)cq; (void)flags;
    err->op_context = completed_context;
    err->err = FI_EIO;
    err->prov_errno = 123;
    completed_context = NULL;
    cq_error = 0;
    return 1;
}

static const char *fake_strerror(struct fid_cq *cq, int err, const void *data,
                                 char *buf, size_t len)
{
    (void)cq; (void)err; (void)data; (void)buf; (void)len;
    return "injected provider error";
}

/* Simulate a bind/enable failure and a provider refusing rollback close. */
static struct fid_ep *created_ep;
static int setup_step;
static int setup_failure_step;

static int fake_bind(struct fid *fid, struct fid *other, uint64_t flags)
{
    (void)fid; (void)other; (void)flags;
    return ++setup_step == setup_failure_step ? -FI_EIO : 0;
}

static int fake_control(struct fid *fid, int command, void *arg)
{
    (void)fid; (void)command; (void)arg;
    return ++setup_step == setup_failure_step ? -FI_EIO : 0;
}

static int fake_endpoint(struct fid_domain *domain, struct fi_info *info,
                          struct fid_ep **out, void *context)
{
    (void)domain; (void)info;
    created_ep->fid.context = context;
    *out = created_ep;
    return 0;
}

static void test_endpoint_rollback(void)
{
    struct fi_ops ops = {.size = sizeof(ops), .close = fake_close,
                        .bind = fake_bind, .control = fake_control};
    struct fi_ops_domain dom_ops = {.size = sizeof(dom_ops), .endpoint = fake_endpoint};
    struct fid_domain native_domain = {.ops = &dom_ops};
    struct fid_ep native_ep = {.fid = {.ops = &ops}};
    struct fid_eq native_eq = {0};
    struct fid_cq native_tx = {.fid = {.ops = &ops}};
    struct fid_cq native_rx = {.fid = {.ops = &ops}};
    shim_fabric_t fabric = {0};
    shim_domain_t domain = {.raw = &native_domain, .fabric = &fabric};
    shim_eq_t eq = {.raw = &native_eq, .fabric = &fabric};
    shim_info_t info = {.raw = fi_allocinfo()};
    assert(info.raw != NULL);
    info.raw->ep_attr->max_msg_size = 64;
    created_ep = &native_ep;
    struct fid request_handle = {.fclass = FI_CLASS_CONNREQ};
    for (int is_request = 0; is_request <= 1; is_request++) {
    info.raw->handle = is_request ? &request_handle : NULL;
    for (int fail_at = 1; fail_at <= 4; fail_at++) {
        for (int refuse_close = 0; refuse_close <= 1; refuse_close++) {
            shim_cq_t *tx = calloc(1, sizeof(*tx));
            shim_cq_t *rx = calloc(1, sizeof(*rx));
            assert(tx && rx);
            *tx = (shim_cq_t){.raw = &native_tx, .domain = &domain, .capacity = 1};
            *rx = (shim_cq_t){.raw = &native_rx, .domain = &domain, .capacity = 1};
            domain.children = 2;
            setup_step = 0;
            setup_failure_step = fail_at;
            close_result = refuse_close ? -FI_EBUSY : 0;
            shim_endpoint_t *ep = NULL;
            assert(shim_endpoint_open(&domain, &info, &eq, tx, rx, &ep) == -FI_EIO);
            if (refuse_close || is_request) {
                assert(ep != NULL && eq.users == 1 && domain.children == 3);
                assert(shim_cq_close(tx) == -FI_EBUSY);
                close_result = 0;
                assert(shim_endpoint_close(ep) == 0);
            } else {
                assert(ep == NULL);
            }
            assert(eq.users == 0 && domain.children == 2);
            assert(shim_cq_close(tx) == 0);
            assert(shim_cq_close(rx) == 0);
            assert(domain.children == 0);
        }
    }
    }
    fi_freeinfo(info.raw);
}

static uint32_t next_event;
static struct fid *event_source;
static struct fi_info *request_info;

static ssize_t fake_eq_read(struct fid_eq *eq, uint32_t *event, void *buf,
                             size_t len, uint64_t flags)
{
    (void)eq; (void)flags;
    if (next_event == 0) return -FI_EAGAIN;
    if (next_event == SHIM_EVENT_ERROR + 100) return -FI_EAVAIL;
    assert(len >= sizeof(struct fi_eq_cm_entry));
    struct fi_eq_cm_entry *entry = buf;
    entry->fid = event_source;
    entry->info = request_info;
    *event = next_event;
    next_event = 0;
    request_info = NULL;
    return sizeof(*entry);
}

static ssize_t fake_eq_readerr(struct fid_eq *eq, struct fi_eq_err_entry *err,
                                uint64_t flags)
{
    (void)eq; (void)flags;
    err->fid = event_source;
    err->err = FI_ECONNREFUSED;
    err->prov_errno = 42;
    next_event = 0;
    return sizeof(*err);
}

static const char *fake_eq_strerror(struct fid_eq *eq, int err, const void *data,
                                    char *buf, size_t len)
{
    (void)eq; (void)err; (void)data; (void)buf; (void)len;
    return "injected connection error";
}

static void test_connection_events(void)
{
    struct fi_ops_eq eq_ops = {.size = sizeof(eq_ops), .read = fake_eq_read,
        .readerr = fake_eq_readerr, .strerror = fake_eq_strerror};
    struct fid_eq native_eq = {.ops = &eq_ops};
    shim_eq_t eq = {.raw = &native_eq};
    shim_listener_t listener = {0};
    struct fid source = {.context = &listener};
    event_source = &source;
    shim_connect_event_t event;
    assert(shim_eq_poll(&eq, &event) == 0);
    request_info = fi_allocinfo();
    assert(request_info != NULL);
    struct fi_info *original = request_info;
    next_event = FI_CONNREQ;
    assert(shim_eq_poll(&eq, &event) == 1);
    assert(event.type == SHIM_EVENT_CONNREQ && event.info->raw == original);
    assert(event.source == (uintptr_t)&listener);
    shim_info_free(event.info);
    next_event = FI_CONNECTED;
    assert(shim_eq_poll(&eq, &event) == 1);
    assert(event.type == SHIM_EVENT_CONNECTED && event.info == NULL);
    next_event = FI_SHUTDOWN;
    assert(shim_eq_poll(&eq, &event) == 1 && event.type == SHIM_EVENT_SHUTDOWN);
    next_event = SHIM_EVENT_ERROR + 100;
    assert(shim_eq_poll(&eq, &event) == 1 && event.type == SHIM_EVENT_ERROR);
    assert(event.err == FI_ECONNREFUSED && event.prov_err == 42);
    assert(strcmp(event.message, "injected connection error") == 0);
}

int main(void)
{
    test_endpoint_rollback();
    test_connection_events();
    shim_info_t *query = NULL;
    assert(shim_info_open("192.0.2.1", "7471", "rxe_test", 1, &query) == 0);
    assert(observed_query_flags == FI_SOURCE);
    assert(strcmp(query->raw->domain_attr->name, "rxe_test") == 0);
    shim_info_free(query);
    assert(shim_info_open("192.0.2.1", "7471", "rxe_test", 0, &query) == 0);
    assert(observed_query_flags == 0 && query->raw->dest_addr != NULL);
    shim_info_free(query);
    duplicate_failure = 1;
    assert(shim_info_open("192.0.2.1", "7471", "rxe_test", 1, &query) == -FI_ENOMEM);
    assert(query == NULL);
    duplicate_failure = 0;

    struct fi_ops ops = {.size = sizeof(ops), .close = fake_close};
    struct fi_ops_msg msg = {.size = sizeof(msg), .send = fake_send, .recv = fake_recv};
    struct fi_ops_cq cq_ops = {.size = sizeof(cq_ops), .read = fake_read,
                             .readerr = fake_readerr, .strerror = fake_strerror};
    struct fid_ep native_ep = {.fid = {.ops = &ops}, .msg = &msg};
    struct fid_cq native_tx = {.fid = {.ops = &ops}, .ops = &cq_ops};
    struct fid_cq native_rx = {.fid = {.ops = &ops}, .ops = &cq_ops};
    struct fid_mr native_mr = {.fid = {.ops = &ops}};
    shim_domain_t domain = {.children = 4};
    shim_eq_t eq = {.users = 1};
    shim_cq_t *tx = calloc(1, sizeof(*tx));
    shim_cq_t *rx = calloc(1, sizeof(*rx));
    shim_endpoint_t *ep = calloc(1, sizeof(*ep));
    shim_mr_t *mr = calloc(1, sizeof(*mr));
    assert(tx && rx && ep && mr);
    char buffer[64] = {0};
    *tx = (shim_cq_t){.raw = &native_tx, .domain = &domain,
                     .capacity = 1, .attached = 1, .used = 1};
    *rx = (shim_cq_t){.raw = &native_rx, .domain = &domain,
                     .capacity = 1, .attached = 1, .used = 1};
    *ep = (shim_endpoint_t){.raw = &native_ep, .domain = &domain, .eq = &eq,
                            .tx = tx, .rx = rx, .max_msg = 64, .ready = 1};
    *mr = (shim_mr_t){.raw = &native_mr, .domain = &domain, .buffer = buffer,
                      .capacity = 64, .access = SHIM_ACCESS_SEND | SHIM_ACCESS_RECV};
    shim_op_t *send = NULL, *recv = NULL;
    assert(shim_op_create(UINT64_C(1) << 40, &send) == 0);
    assert(shim_op_create(2, &recv) == 0);

    /* Invalid inputs clear outputs without trying to discover a device. */
    shim_info_t *info = (shim_info_t *)(uintptr_t)1;
    assert(shim_info_open(NULL, "7471", NULL, 1, &info) == -FI_EINVAL);
    assert(info == NULL);
    assert(shim_post_send(ep, send, mr, 65, buffer) == -FI_EINVAL);
    assert(shim_post_send(ep, send, mr, 2, buffer + 63) == -FI_EINVAL);
    post_result = -FI_EAGAIN;
    assert(shim_post_send(ep, send, mr, 5, buffer) == -FI_EAGAIN);
    assert(tx->count == 0 && mr->pending == 0 && send->cq == NULL);

    post_result = 0;
    assert(shim_post_send(ep, send, mr, 5, buffer) == 0);
    assert(shim_post_send(ep, send, mr, 5, buffer) == -FI_EBUSY);
    assert(shim_post_send(ep, recv, mr, 5, buffer) == -FI_EAGAIN);
    assert(shim_mr_close(mr) == -FI_EBUSY);
    assert(shim_op_free(send) == -FI_EBUSY);
    assert(shim_cq_close(tx) == -FI_EBUSY);
    assert(shim_domain_close(&domain) == -FI_EBUSY);
    assert(shim_eq_close(&eq) == -FI_EBUSY);

    shim_completion_t done;
    assert(shim_cq_poll(tx, &done) == 0);
    completed_context = &send->context;
    assert(shim_cq_poll(tx, &done) == 1);
    assert(done.op_id == (UINT64_C(1) << 40) && done.len == 5);
    assert(done.flags == SHIM_COMPLETION_SEND && done.err == 0);
    assert(mr->pending == 0 && tx->count == 0);

    /* Error CQ entries still release their operation and preserve direction. */
    assert(shim_post_recv(ep, recv, mr, sizeof(buffer), buffer) == 0);
    cq_error = 1;
    completed_context = &recv->context;
    assert(shim_cq_poll(rx, &done) == 1);
    assert(done.err == FI_EIO && done.prov_err == 123 && done.len == 0);
    assert(done.flags == SHIM_COMPLETION_RECV && done.op_id == 2);
    assert(strcmp(done.message, "injected provider error") == 0);

    assert(shim_post_recv(ep, recv, mr, sizeof(buffer), buffer) == 0);
    close_result = -FI_EBUSY;
    assert(shim_endpoint_close(ep) == -FI_EBUSY);
    assert(mr->pending == 1 && eq.users == 1 && rx->attached);
    assert(shim_mr_close(mr) == -FI_EBUSY);
    close_result = 0;
    assert(shim_endpoint_close(ep) == 0);
    assert(eq.users == 0 && mr->pending == 0);
    assert(shim_cq_poll(rx, &done) == -FI_EOPBADSTATE);
    assert(shim_op_free(recv) == -FI_EBUSY);
    close_result = -FI_EBUSY;
    assert(shim_cq_close(rx) == -FI_EBUSY);
    assert(shim_op_free(recv) == -FI_EBUSY);
    close_result = 0;
    assert(shim_mr_close(mr) == 0);
    assert(shim_cq_close(tx) == 0);
    assert(shim_cq_close(rx) == 0);
    assert(shim_op_free(send) == 0);
    assert(shim_op_free(recv) == 0);
    assert(domain.children == 0);
    puts("shim lifecycle tests passed");
    return 0;
}
