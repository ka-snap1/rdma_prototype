#include "fabric_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdma/fabric.h"
#include <rdma/fi_errno.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <stddef.h>

/* aviod the dependency of compile factors in strdup*/
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
    commit to registor send/receive buffers and provide MR descripters during send and receive operations
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

