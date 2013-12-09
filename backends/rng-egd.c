/*
 * QEMU Random Number Generator Backend
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "sysemu/rng.h"
#include "sysemu/char.h"
#include "qapi/qmp/qerror.h"
#include "hw/qdev.h" /* just for DEFINE_PROP_CHR */

#define TYPE_RNG_EGD "rng-egd"
#define RNG_EGD(obj) OBJECT_CHECK(RngEgd, (obj), TYPE_RNG_EGD)

#define MAX_BUFFER_SIZE 65536

typedef struct RngEgd
{
    RngBackend parent;

    CharDriverState *chr;
    char *chr_name;

    EntropyReceiveFunc *receive_entropy;
    GSList *requests;
    void *opaque;
    size_t req_size;
    uint32_t buf_size;
} RngEgd;

typedef struct RngRequest
{
    uint8_t *data;
    size_t offset;
    size_t size;
} RngRequest;


static void rng_egd_free_request(RngRequest *req)
{
    g_free(req->data);
    g_free(req);
}

static int get_available_data_size(RngEgd *s)
{
    GSList *i;
    RngRequest *req;
    int total = 0;

    for (i = s->requests; i; i = i->next) {
        req = i->data;
        total += req->offset;
    }
    return total;
}

static int get_free_buf_size(RngEgd *s)
{

    GSList *i;
    RngRequest *req;
    int total = 0;

    for (i = s->requests; i; i = i->next) {
        req = i->data;
        total += req->size - req->offset;
    }
    return total;
}

static int get_total_buf_size(RngEgd *s)
{

    GSList *i;
    RngRequest *req;
    int total = 0;

    for (i = s->requests; i; i = i->next) {
        req = i->data;
        total += req->size;
    }
    return total;
}

static void rng_egd_append_request(RngBackend *b, size_t size,
                                   EntropyReceiveFunc *receive_entropy,
                                   void *opaque)
{
    RngEgd *s = RNG_EGD(b);
    RngRequest *req;

    s->receive_entropy = receive_entropy;
    s->opaque = opaque;

    req = g_malloc(sizeof(*req));

    req->offset = 0;
    req->size = size;
    req->data = g_malloc(req->size);

    while (size > 0) {
        uint8_t header[2];
        uint8_t len = MIN(size, 255);

        /* synchronous entropy request */
        header[0] = 0x02;
        header[1] = len;

        qemu_chr_fe_write(s->chr, header, sizeof(header));

        size -= len;
    }

    s->requests = g_slist_append(s->requests, req);
}


static void rng_egd_expend_request(RngEgd *s, size_t size,
                                   EntropyReceiveFunc *receive_entropy,
                                   void *opaque)
{
    GSList *cur = s->requests;

    while (size > 0 && cur) {
        RngRequest *req = cur->data;
        int len = MIN(size, req->offset);

        s->receive_entropy(s->opaque, req->data, len);
        req->offset -= len;
        size -= len;
        cur = cur->next;
    }
}

static void rng_egd_request_entropy(RngBackend *b, size_t size,
                                    EntropyReceiveFunc *receive_entropy,
                                    void *opaque)
{
    RngEgd *s = RNG_EGD(b);

    s->receive_entropy = receive_entropy;
    s->opaque = opaque;
    s->req_size += size;

    if (get_available_data_size(s) >= size) {
        rng_egd_expend_request(s, size, receive_entropy, opaque);
        s->req_size -= size;
    }

    int total_size = get_total_buf_size(s);
    int buf_size;

    if (s->buf_size != 0) {
        buf_size = MIN(s->buf_size, MAX_BUFFER_SIZE);
    } else {
        buf_size = MAX_BUFFER_SIZE;
    }

    while (total_size < buf_size)  {
        int add_size = MIN(buf_size - total_size, 255);
        total_size += add_size;
        rng_egd_append_request(b, add_size, receive_entropy, opaque);
    }
}

static int rng_egd_chr_can_read(void *opaque)
{
    RngEgd *s = RNG_EGD(opaque);
    int size = 0;

    size = get_free_buf_size(s);

    if (size == 0 && s->req_size > 0) {
        int len = MIN(s->req_size, get_available_data_size(s));
        rng_egd_expend_request(s, len, s->receive_entropy, opaque);
        s->req_size -= len;
        size = get_free_buf_size(s);
    }

    return size;
}

static void rng_egd_chr_read(void *opaque, const uint8_t *buf, int size)
{
    RngEgd *s = RNG_EGD(opaque);
    size_t buf_offset = 0;
    int len;
    GSList *cur = s->requests;

    while (size > 0 && s->requests) {
        RngRequest *req = cur->data;
        len = MIN(size, req->size - req->offset);

        memcpy(req->data + req->offset, buf + buf_offset, len);
        buf_offset += len;
        req->offset += len;
        size -= len;
        cur = cur->next;
    }
    if (s->req_size > 0) {
        len = MIN(s->req_size, get_available_data_size(s));
        rng_egd_expend_request(s, len, s->receive_entropy, opaque);
        s->req_size -= len;
    }

}

static void rng_egd_free_requests(RngEgd *s)
{
    GSList *i;

    for (i = s->requests; i; i = i->next) {
        rng_egd_free_request(i->data);
    }

    g_slist_free(s->requests);
    s->requests = NULL;
}

static void rng_egd_cancel_requests(RngBackend *b)
{
    RngEgd *s = RNG_EGD(b);

    /* We simply delete the list of pending requests.  If there is data in the 
     * queue waiting to be read, this is okay, because there will always be
     * more data than we requested originally
     */
    rng_egd_free_requests(s);
}

static void rng_egd_opened(RngBackend *b, Error **errp)
{
    RngEgd *s = RNG_EGD(b);

    if (s->chr_name == NULL) {
        error_set(errp, QERR_INVALID_PARAMETER_VALUE,
                  "chardev", "a valid character device");
        return;
    }

    s->chr = qemu_chr_find(s->chr_name);
    if (s->chr == NULL) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, s->chr_name);
        return;
    }

    if (qemu_chr_fe_claim(s->chr) != 0) {
        error_set(errp, QERR_DEVICE_IN_USE, s->chr_name);
        return;
    }

    /* FIXME we should resubmit pending requests when the CDS reconnects. */
    qemu_chr_add_handlers(s->chr, rng_egd_chr_can_read, rng_egd_chr_read,
                          NULL, s);
}

static void rng_egd_set_buf_size(Object *obj, const char *value, Error **errp)
{
    RngBackend *b = RNG_BACKEND(obj);
    RngEgd *s = RNG_EGD(b);

    s->buf_size = atoi(value);
    assert(s->buf_size > 0);
}

static void rng_egd_set_chardev(Object *obj, const char *value, Error **errp)
{
    RngBackend *b = RNG_BACKEND(obj);
    RngEgd *s = RNG_EGD(b);

    if (b->opened) {
        error_set(errp, QERR_PERMISSION_DENIED);
    } else {
        s->chr_name = g_strdup(value);
    }
}

static char *rng_egd_get_chardev(Object *obj, Error **errp)
{
    RngEgd *s = RNG_EGD(obj);

    if (s->chr && s->chr->label) {
        return g_strdup(s->chr->label);
    }

    return NULL;
}

static void rng_egd_init(Object *obj)
{
    object_property_add_str(obj, "chardev",
                            rng_egd_get_chardev, rng_egd_set_chardev,
                            NULL);
    object_property_add_str(obj, "buf_size", NULL, rng_egd_set_buf_size, NULL);
}

static void rng_egd_finalize(Object *obj)
{
    RngEgd *s = RNG_EGD(obj);

    if (s->chr) {
        qemu_chr_add_handlers(s->chr, NULL, NULL, NULL, NULL);
        qemu_chr_fe_release(s->chr);
    }

    g_free(s->chr_name);

    rng_egd_free_requests(s);
}

static void rng_egd_class_init(ObjectClass *klass, void *data)
{
    RngBackendClass *rbc = RNG_BACKEND_CLASS(klass);

    rbc->request_entropy = rng_egd_request_entropy;
    rbc->cancel_requests = rng_egd_cancel_requests;
    rbc->opened = rng_egd_opened;
}

static const TypeInfo rng_egd_info = {
    .name = TYPE_RNG_EGD,
    .parent = TYPE_RNG_BACKEND,
    .instance_size = sizeof(RngEgd),
    .class_init = rng_egd_class_init,
    .instance_init = rng_egd_init,
    .instance_finalize = rng_egd_finalize,
};

static void register_types(void)
{
    type_register_static(&rng_egd_info);
}

type_init(register_types);
