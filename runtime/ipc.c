// IPC message parsing/serialization and session management.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "ipc.h"

#ifdef __APPLE__
extern const ServiceDef __start_services[] __asm__("section$start$__DATA$__services");
extern const ServiceDef __stop_services[] __asm__("section$end$__DATA$__services");
#elif defined(__linux__)
extern const ServiceDef __start_services[], __stop_services[];  // (ELF: defined by the linker)
#else
// PE/COFF: .svc$a < .svc$m (entries, ipc.h) < .svc$z (the linker sorts by the text after '$')
__attribute__((section(".svc$a"), used)) static const ServiceDef __start_services[1];
__attribute__((section(".svc$z"), used)) static const ServiceDef __stop_services[1];
#endif

static void stub_handler(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[ipc] STUB %s cmd %u (in %u bytes, x%d a%d b%d c%d) -> ok\n", s->name, req->cmd, req->data_size, req->nx,
        req->na, req->nb, req->nc);
    resp->result = 0;
    resp_size(resp, 0x40);  // zeroed outputs
}

Service *service_new(const char *name, ServiceHandler h, void *state) {
    Service *s = calloc(1, sizeof *s);
    s->name = strdup(name);
    s->handler = h;
    s->state = state;
    return s;
}

Service *service_lookup(const char *name) {
    const ServiceDef *start = __start_services, *stop = __stop_services;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    for (const ServiceDef *d = start; d < stop; d++)
        if (d->name && !strcmp(d->name, name)) return service_new(name, d->handler, NULL);
    LOG("[ipc] no HLE implementation for service '%s', using logging stub\n", name);
    return service_new(name, stub_handler, NULL);
}

void ipc_return_object(IpcResp *resp, Service *s) { resp->objects[resp->nobjects++] = s; }

// Guest threads send requests at the same time, with no kernel lock held (SendSyncRequest), and the clones of a domain
// session share its object table: the table's slots and the services' reference counts change under this lock. Two
// objects made at once could take the same free slot, and closing one then left the other "missing" (Steam Deck,
// Continue: the game's resource loading stopped on the failed request). Handlers run without it.
static pthread_mutex_t g_domain_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t session_new(Service *s) {
    KSession *k = calloc(1, sizeof *k);
    k->obj.type = KOBJ_SESSION;
    k->svc = s;
    pthread_mutex_lock(&g_domain_lock);
    s->refs++;
    pthread_mutex_unlock(&g_domain_lock);
    return kernel_new_handle(&k->obj);
}

// (caller holds g_domain_lock) -> the service to close once the lock is released (its last holder went), or NULL
static Service *service_release(Service *s) {
    return s && --s->refs == 0 && s->on_close ? s : NULL;
}

uint32_t ipc_connect_named_port(const char *name) {
    if (!strcmp(name, "sm:")) return session_new(service_lookup("sm:"));
    LOG("[ipc] ConnectToNamedPort('%s') unsupported\n", name);
    return 0;
}

// ------------------------------------------------------------------ sm:
SERVICE("sm:", sm_handler) {
    (void)s;
    switch (req->cmd) {
    case 0:  // RegisterClient
        break;
    case 1: {  // GetServiceHandle(name)
        char name[9] = {0};
        memcpy(name, req->data, 8);
        LOG("[sm] GetService %s\n", name);
        ipc_return_object(resp, service_lookup(name));
        break;
    }
    default:
        LOG("[sm] unknown cmd %u\n", req->cmd);
    }
}

// ------------------------------------------------------------------ message codec
static uint64_t buf_addr(const uint32_t *w) {
    return w[1] | ((uint64_t)(w[2] >> 28) << 32) | ((uint64_t)((w[2] >> 2) & 7) << 36);
}

// TIPC (sm: and a few other system services since 12.0.0) replies with the same HIPC header type as
// the request, the handles in the special header, and raw words holding the result followed by the
// outputs: no SFCO header, no 16-byte alignment and no domains.
static int tipc_write_response(uint32_t *msg, uint32_t type, IpcResp *resp) {
    for (int i = 0; i < resp->nobjects; i++) resp->move_handles[resp->nmove++] = session_new(resp->objects[i]);
    memset(msg, 0, 0x100);
    int has_handles = resp->ncopy || resp->nmove;
    uint32_t words = 1 + (resp->data_size + 3) / 4;
    msg[0] = type;
    msg[1] = words | (has_handles ? 0x80000000u : 0);
    uint32_t *q = msg + 2;
    if (has_handles) {
        *q++ = (resp->ncopy << 1) | (resp->nmove << 5);
        for (int i = 0; i < resp->ncopy; i++) *q++ = resp->copy_handles[i];
        for (int i = 0; i < resp->nmove; i++) *q++ = resp->move_handles[i];
    }
    *q++ = resp->result;
    memcpy(q, resp->data, resp->data_size);
    return 0;
}

int ipc_handle_request(Cpu *c, uint32_t handle) {
    KObject *o = handle_get(handle);
    if (!o || o->type != KOBJ_SESSION) {
        LOG("[ipc] SendSyncRequest on bad handle %x\n", handle);
        return 0xE401;
    }
    KSession *sess = (KSession *)o;
    uint32_t *msg = (uint32_t *)(uintptr_t)c->tpidrro;
    IpcReq req = {0};
    IpcResp resp = {0};
    req.type = msg[0] & 0xFFFF;
    int nx = (msg[0] >> 16) & 0xF, na = (msg[0] >> 20) & 0xF, nb = (msg[0] >> 24) & 0xF;
    uint32_t raw_words = msg[1] & 0x3FF;
    uint32_t cflags = (msg[1] >> 10) & 0xF;
    uint32_t *p = msg + 2;
    if (msg[1] & 0x80000000u) {
        uint32_t hd = *p++;
        if (hd & 1) { req.pid = p[0] | ((uint64_t)p[1] << 32); p += 2; }
        req.ncopy = (hd >> 1) & 0xF;
        req.nmove = (hd >> 5) & 0xF;
        for (int i = 0; i < req.ncopy; i++) req.copy_handles[i] = *p++;
        for (int i = 0; i < req.nmove; i++) req.move_handles[i] = *p++;
    }
    for (int i = 0; i < nx; i++, p += 2) {
        req.x[i].addr = p[1] | ((uint64_t)((p[0] >> 12) & 0xF) << 32) | ((uint64_t)((p[0] >> 6) & 0x7) << 36);
        req.x[i].size = p[0] >> 16;
    }
    req.nx = nx;
    for (int i = 0; i < na; i++, p += 3) {
        req.a[i].size = p[0] | ((uint64_t)((p[2] >> 24) & 0xF) << 32);
        req.a[i].addr = buf_addr(p);
    }
    req.na = na;
    for (int i = 0; i < nb; i++, p += 3) {
        req.b[i].size = p[0] | ((uint64_t)((p[2] >> 24) & 0xF) << 32);
        req.b[i].addr = buf_addr(p);
    }
    req.nb = nb;
    uint32_t *raw = p;
    // raw data is aligned to 16 bytes relative to the message start
    uint32_t *aligned = (uint32_t *)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
    if (cflags >= 2) {
        uint32_t *cdesc = raw + raw_words;
        int ncd = cflags == 2 ? 1 : (int)cflags - 2;
        for (int i = 0; i < ncd; i++) {
            req.c[i].addr = cdesc[2 * i] | ((uint64_t)(cdesc[2 * i + 1] & 0xFFFF) << 32);
            req.c[i].size = cdesc[2 * i + 1] >> 16;
        }
        req.nc = ncd;
    }

    if (req.type == 2 || req.type == 15) {  // close (CMIF / TIPC)
        LOG("[ipc] close session %s (handle %x)\n", sess->svc->name, handle);
        if (!sess->domain) {  // (a domain's objects stay: closed one by one)
            pthread_mutex_lock(&g_domain_lock);
            Service *last = service_release(sess->svc);
            pthread_mutex_unlock(&g_domain_lock);
            if (last) last->on_close(last);
        }
        return 0;
    }
    if (req.type >= 16) {  // TIPC: the command is in the type, the raw words are the arguments
        req.cmd = req.type - 16;
        req.data = (const uint8_t *)raw;
        req.data_size = raw_words * 4;
        sess->svc->handler(sess->svc, &req, &resp);
        return tipc_write_response(msg, req.type, &resp);
    }
    uint8_t *payload = (uint8_t *)aligned;
    bool dom = false;  // response is domain-wrapped
    if (req.type == 4 || req.type == 6) {
        if (sess->domain) {
            dom = true;
            uint8_t dcmd = payload[0];
            uint8_t nin = payload[1];
            uint16_t dsize = *(uint16_t *)(payload + 2);
            req.domain_obj = *(uint32_t *)(payload + 4);
            if (dcmd == 2) {  // close object
                if (req.domain_obj < 256) {
                    pthread_mutex_lock(&g_domain_lock);
                    Service *o = sess->domain->objs[req.domain_obj];
                    sess->domain->objs[req.domain_obj] = NULL;
                    Service *last = service_release(o);
                    pthread_mutex_unlock(&g_domain_lock);
                    if (last) last->on_close(last);
                }
                goto write_response;
            }
            req.ndomain_in = nin;
            for (int i = 0; i < nin; i++) req.domain_in_objs[i] = *(uint32_t *)(payload + 16 + dsize + 4 * i);
            payload += 16;
        }
        if (memcmp(payload, "SFCI", 4)) {
            LOG("[ipc] bad SFCI magic on %s (domain=%d):", sess->svc->name, sess->domain != NULL);
            for (int i = 0; i < 24; i++) LOG(" %08x", msg[i]);
            LOG("\n");
        }
        req.cmd = *(uint32_t *)(payload + 8);
        req.data = payload + 16;
        req.data_size = raw_words * 4 > 32 ? raw_words * 4 - 32 : 0;
        Service *target = sess->svc;
        if (sess->domain) {
            pthread_mutex_lock(&g_domain_lock);
            target = req.domain_obj < 256 ? sess->domain->objs[req.domain_obj] : NULL;
            pthread_mutex_unlock(&g_domain_lock);
            if (!target) {
                LOG("[ipc] domain object %u missing on %s (cmd %u)\n", req.domain_obj, sess->svc->name, req.cmd);
                return 0xE401;
            }
        }
        target->handler(target, &req, &resp);
    } else if (req.type == 5 || req.type == 7) {  // control
        req.cmd = *(uint32_t *)(payload + 8);
        LOG("[ipc] control %u on %s (handle %x)\n", req.cmd, sess->svc->name, handle);
        switch (req.cmd) {
        case 0:  // ConvertCurrentObjectToDomain
            sess->domain = calloc(1, sizeof(Domain));
            sess->domain->objs[1] = sess->svc;
            resp_size(&resp, 4);
            *(uint32_t *)resp.data = 1;
            break;
        case 2: case 4: {  // CloneCurrentObject(Ex)
            uint32_t h = session_new(sess->svc);
            ((KSession *)handle_get(h))->domain = sess->domain;
            resp.move_handles[resp.nmove++] = h;
            break;
        }
        case 3:  // QueryPointerBufferSize
            *(uint16_t *)resp.data = 0x8000;
            resp_size(&resp, 4);
            break;
        default:
            LOG("[ipc] control cmd %u\n", req.cmd);
        }
    } else {
        LOG("[ipc] message type %u\n", req.type);
    }

write_response:;
    // returned objects: domain objects or new sessions
    uint32_t domain_out[8];
    int ndomain_out = 0;
    for (int i = 0; i < resp.nobjects; i++) {
        if (dom) {
            pthread_mutex_lock(&g_domain_lock);
            int id = 1;
            while (id < 256 && sess->domain->objs[id]) id++;
            if (id == 256) FATAL("[ipc] domain of %s full (256 objects)\n", sess->svc->name);
            sess->domain->objs[id] = resp.objects[i];
            resp.objects[i]->refs++;
            pthread_mutex_unlock(&g_domain_lock);
            domain_out[ndomain_out++] = id;
        } else {
            resp.move_handles[resp.nmove++] = session_new(resp.objects[i]);
        }
    }
    memset(msg, 0, 0x100);
    uint32_t *w = msg;
    int has_handles = resp.ncopy || resp.nmove;
    uint32_t payload_words = 4 + (resp.data_size + 3) / 4;   // SFCO header + data
    if (dom) payload_words += 4 + ndomain_out;
    uint32_t raw_size = payload_words + 4;  // + alignment padding
    w[0] = 0;  // type 0 response
    w[1] = raw_size | (has_handles ? 0x80000000u : 0);
    uint32_t *q = w + 2;
    if (has_handles) {
        *q++ = (resp.ncopy << 1) | (resp.nmove << 5);
        for (int i = 0; i < resp.ncopy; i++) *q++ = resp.copy_handles[i];
        for (int i = 0; i < resp.nmove; i++) *q++ = resp.move_handles[i];
    }
    uint32_t *out = (uint32_t *)(((uintptr_t)q + 15) & ~(uintptr_t)15);
    if (dom) {
        out[0] = ndomain_out;
        out += 4;
    }
    memcpy(out, "SFCO", 4);
    out[1] = 0;
    out[2] = resp.result;
    out[3] = 0;
    memcpy(out + 4, resp.data, resp.data_size);
    if (dom)
        for (int i = 0; i < ndomain_out; i++) out[4 + (resp.data_size + 3) / 4 + i] = domain_out[i];
    return 0;
}
