#include "ctx.h"
#include "iotable.h"
#include "timer-ng.h"
#include "ioutils.h"
#include <stdio.h>

#define CTX_FD(ctx) (ctx)->fd
#define CTX_SD(ctx) (ctx)->sd
#define CTX_IOT(ctx) (ctx)->io
#include "rw-inl.h"

#define LOGARGS(c, lvl) (c)->sock->settings, "ioctx", LCB_LOG_##lvl, __FILE__, __LINE__
static const lcb_host_t * get_ctx_host(const lcbio_CTX *ctx) {
    static lcb_host_t host = { "NOHOST", "NOPORT" };
    if (!ctx) { return &host; }
    if (!ctx->sock) { return &host; }
    if (!ctx->sock->info) { return &host; }
    return &ctx->sock->info->ep;
}
#define CTX_LOGFMT "<%s:%s> (CTX=%p,%s) "
#define CTX_LOGID(ctx) get_ctx_host(ctx)->host, get_ctx_host(ctx)->port, (void*)ctx, ctx?ctx->subsys : ""

typedef enum {
    ES_ACTIVE = 0,
    ES_DETACHED
} easy_state;

static void
err_handler(void *cookie)
{
    lcbio_CTX *ctx = (void *)cookie;
    ctx->procs.cb_err(ctx, ctx->err);
}

static lcb_error_t
convert_lcberr(const lcbio_CTX *ctx, lcbio_IOSTATUS status)
{
    const lcb_settings *settings = ctx->sock->settings;
    lcbio_OSERR oserr = IOT_ERRNO(ctx->sock->io);
    if (status == LCBIO_SHUTDOWN) {
        return lcbio_mklcberr(0, settings);
    } else if (oserr != 0) {
        return lcbio_mklcberr(oserr, settings);
    } else {
        return LCB_NETWORK_ERROR;
    }
}

lcbio_CTX *
lcbio_ctx_new(lcbio_SOCKET *sock, void *data, const lcbio_EASYPROCS *procs)
{
    lcbio_CTX *ctx = calloc(1, sizeof(*ctx));
    ctx->sock = sock;
    sock->ctx = ctx;
    ctx->io = sock->io;
    ctx->data = data;
    ctx->procs = *procs;
    ctx->state = ES_ACTIVE;
    ctx->as_err = lcbio_timer_new(ctx->io, ctx, err_handler);
    ctx->subsys = "unknown";

    rdb_init(&ctx->ior, sock->settings->allocator_factory());
    lcbio_ref(sock);

    if (IOT_IS_EVENT(ctx->io)) {
        ctx->event = IOT_V0EV(ctx->io).create(IOT_ARG(ctx->io));
        ctx->fd = sock->u.fd;
    } else {
        ctx->sd = sock->u.sd;
    }

    ctx->procs = *procs;
    ctx->state = ES_ACTIVE;

    lcb_log(LOGARGS(ctx, DEBUG), CTX_LOGFMT "Pairing with SOCK=%p", CTX_LOGID(ctx), (void*)sock);
    return ctx;
}

static void
free_ctx(lcbio_CTX *ctx)
{
    rdb_cleanup(&ctx->ior);
    lcbio_unref(ctx->sock);
    if (ctx->output) {
        ringbuffer_destruct(&ctx->output->rb);
        free(ctx->output);
    }
    if (ctx->procs.cb_flush_ready) {
        /* dtor */
        ctx->procs.cb_flush_ready(ctx);
    }
    free(ctx);
}

static void
deactivate_watcher(lcbio_CTX *ctx)
{
    if (ctx->evactive && ctx->event) {
        IOT_V0EV(CTX_IOT(ctx)).cancel(
                IOT_ARG(CTX_IOT(ctx)), CTX_FD(ctx), ctx->event);
        ctx->evactive = 0;
    }
}

void
lcbio_ctx_close_ex(lcbio_CTX *ctx, lcbio_CTXCLOSE_cb cb, void *arg,
                   lcbio_CTXDTOR_cb dtor, void *dtor_arg)
{
    unsigned oldrc;
    ctx->state = ES_DETACHED;
    assert(ctx->sock);

    if (ctx->event) {
        deactivate_watcher(ctx);
        IOT_V0EV(CTX_IOT(ctx)).destroy(IOT_ARG(CTX_IOT(ctx)), ctx->event);
        ctx->event = NULL;
    }

    if (ctx->as_err) {
        lcbio_timer_destroy(ctx->as_err);
        ctx->as_err = NULL;
    }

    oldrc = ctx->sock->refcount;
    lcb_log(LOGARGS(ctx, DEBUG), CTX_LOGFMT "Destroying. PND=%d,ENT=%d,SORC=%d", CTX_LOGID(ctx), (int)ctx->npending, (int)ctx->entered, oldrc);

    if (cb) {
        int reusable =
                ctx->npending == 0 && /* no pending events */
                ctx->err == LCB_SUCCESS && /* no socket errors */
                ctx->rdwant == 0 && /* no expected input */
                ctx->wwant == 0 && /* no expected output */
                (ctx->output == NULL || ctx->output->rb.nbytes == 0);
        cb(ctx->sock, reusable, arg);
    }

    if (oldrc == ctx->sock->refcount) {
        lcbio_shutdown(ctx->sock);
    }

    if (ctx->output) {
        ringbuffer_destruct(&ctx->output->rb);
        free(ctx->output);
        ctx->output = NULL;
    }

    ctx->fd = INVALID_SOCKET;
    ctx->sd = NULL;

    if (dtor) {
        ctx->data = dtor_arg;
        ctx->procs.cb_flush_ready = dtor;

    } else {
        ctx->procs.cb_flush_ready = NULL;
    }

    if (ctx->npending == 0 && ctx->entered == 0) {
        free_ctx(ctx);
    }
}

void
lcbio_ctx_close(lcbio_CTX *ctx, lcbio_CTXCLOSE_cb cb, void *arg)
{
    lcbio_ctx_close_ex(ctx, cb, arg, NULL, NULL);
}

void
lcbio_ctx_put(lcbio_CTX *ctx, const void *buf, unsigned nbuf)
{
    lcbio__EASYRB *erb = ctx->output;

    if (!erb) {
        ctx->output = erb = calloc(1, sizeof(*ctx->output));

        if (!erb) {
            lcbio_ctx_senderr(ctx, LCB_CLIENT_ENOMEM);
            return;
        }

        erb->parent = ctx;

        if (!ringbuffer_initialize(&erb->rb, nbuf)) {
            lcbio_ctx_senderr(ctx, LCB_CLIENT_ENOMEM);
            return;
        }
    }

    if (!ringbuffer_ensure_capacity(&erb->rb, nbuf)) {
        lcbio_ctx_senderr(ctx, LCB_CLIENT_ENOMEM);
        return;
    }

    ringbuffer_write(&erb->rb, buf, nbuf);
}

void
lcbio_ctx_rwant(lcbio_CTX *ctx, unsigned n)
{
    ctx->rdwant = n;
}

static void
set_iterbuf(lcbio_CTX *ctx, lcbio_CTXRDITER *iter)
{
    if ((iter->nbuf = rdb_get_contigsize(&ctx->ior))) {
        if (iter->nbuf > iter->remaining) {
            iter->nbuf = iter->remaining;
        }
        iter->buf = rdb_get_consolidated(&ctx->ior, iter->nbuf);
    } else {
        iter->buf = NULL;
    }
}

void
lcbio_ctx_ristart(lcbio_CTX *ctx, lcbio_CTXRDITER *iter, unsigned nb)
{
    iter->remaining = nb;
    set_iterbuf(ctx, iter);
}

void
lcbio_ctx_rinext(lcbio_CTX *ctx, lcbio_CTXRDITER *iter)
{
    rdb_consumed(&ctx->ior, iter->nbuf);
    iter->remaining -= iter->nbuf;
    set_iterbuf(ctx, iter);
}

static int
E_free_detached(lcbio_CTX *ctx)
{
    if (ctx->state == ES_DETACHED) {
        free_ctx(ctx);
        return 1;
    }
    return 0;
}

static void
invoke_read_cb(lcbio_CTX *ctx, unsigned nb)
{
    ctx->rdwant = 0;
    ctx->entered++;
    ctx->procs.cb_read(ctx, nb);
    ctx->entered--;
}

static void
E_handler(lcb_socket_t sock, short which, void *arg)
{
    lcbio_CTX *ctx = arg;
    lcbio_IOSTATUS status;
    (void)sock;

    if (which & LCB_READ_EVENT) {
        unsigned nb;
        status = lcbio_E_rdb_slurp(ctx, &ctx->ior);
        nb = rdb_get_nused(&ctx->ior);

        if (nb >= ctx->rdwant) {
            invoke_read_cb(ctx, nb);
            if (E_free_detached(ctx)) {
                return;
            }
        }
        if (!LCBIO_IS_OK(status)) {
            lcb_error_t err = convert_lcberr(ctx, status);
            lcbio_ctx_senderr(ctx, err);
            return;
        }
    }

    if (which & LCB_WRITE_EVENT) {
        if (ctx->wwant) {
            ctx->wwant = 0;
            ctx->procs.cb_flush_ready(ctx);
            if (ctx->err) {
                return;
            }
        } else if (ctx->output) {
            status = lcbio_E_rb_write(ctx, &ctx->output->rb);
            if (!LCBIO_IS_OK(status)) {
                deactivate_watcher(ctx);
                ctx->err = convert_lcberr(ctx, status);
                err_handler(ctx);
                return;
            }
        }
    }

    lcbio_ctx_schedule(ctx);
}

static void
invoke_entered_errcb(lcbio_CTX *ctx, lcb_error_t err)
{
    ctx->err = err;
    ctx->entered++;
    ctx->procs.cb_err(ctx, err);
    ctx->entered--;
}

static void
Cw_handler(lcb_sockdata_t *sd, int status, void *arg)
{
    lcbio__EASYRB *erb = arg;
    lcbio_CTX *ctx = erb->parent;
    (void)sd;

    ctx->npending--;

    if (!ctx->output) {
        ctx->output = erb;
        ringbuffer_reset(&erb->rb);

    } else {
        ringbuffer_destruct(&erb->rb);
        free(erb);
    }

    if (ctx->state == ES_ACTIVE && status) {
        invoke_entered_errcb(ctx, convert_lcberr(ctx, LCBIO_IOERR));
    }

    if (ctx->state != ES_ACTIVE && ctx->npending == 0) {
        free_ctx(ctx);
    }
}

static void C_schedule(lcbio_CTX *ctx);

static void
Cr_handler(lcb_sockdata_t *sd, lcb_ssize_t nr, void *arg)
{
    lcbio_CTX *ctx = arg;
    sd->is_reading = 0;
    ctx->npending--;

    if (ctx->state == ES_ACTIVE) {
        if (nr > 0) {
            unsigned total;
            rdb_rdend(&ctx->ior, nr);
            total = rdb_get_nused(&ctx->ior);
            if (total >= ctx->rdwant) {
                invoke_read_cb(ctx, total);
            }

            lcbio_ctx_schedule(ctx);
        } else {
            lcb_error_t err =
                    convert_lcberr(ctx, nr ? LCBIO_SHUTDOWN : LCBIO_IOERR);
            ctx->rdwant = 0;
            invoke_entered_errcb(ctx, err);
        }
    }

    if (ctx->state != ES_ACTIVE && ctx->npending == 0) {
        free_ctx(ctx);
    }
}

static void
C_schedule(lcbio_CTX *ctx)
{
    lcbio_TABLE *io = ctx->io;
    lcb_sockdata_t *sd = CTX_SD(ctx);
    int rv;

    if (ctx->output && ctx->output->rb.nbytes) {
        /** Schedule a write */
        lcb_IOV iov[2];
        unsigned niov;

        ringbuffer_get_iov(&ctx->output->rb, RINGBUFFER_READ, iov);
        niov = iov[1].iov_len ? 2 : 1;
        rv = IOT_V1(io).write2(IOT_ARG(io), sd, iov, niov, ctx->output, Cw_handler);
        if (rv) {
            lcbio_ctx_senderr(ctx, convert_lcberr(ctx, LCBIO_IOERR));
            return;
        } else {
            ctx->output = NULL;
            ctx->npending++;
        }
    }

    if (ctx->rdwant && sd->is_reading == 0) {
        lcb_IOV iov[RWINL_IOVSIZE];
        unsigned ii;
        unsigned niov = rdb_rdstart(&ctx->ior, (nb_IOV *)iov, RWINL_IOVSIZE);

        assert(niov);
        for (ii = 0; ii < niov; ++ii) {
            assert(iov[ii].iov_len);
        }

        rv = IOT_V1(io).read2(IOT_ARG(io), sd, iov, niov, ctx, Cr_handler);
        if (rv) {
            lcbio_ctx_senderr(ctx, convert_lcberr(ctx, LCBIO_IOERR));

        } else {
            sd->is_reading = 1;
            ctx->npending++;
        }
    }
}

static void
E_schedule(lcbio_CTX *ctx)
{
    lcbio_TABLE *io = ctx->io;
    short which = 0;

    if (ctx->rdwant) {
        which |= LCB_READ_EVENT;
    }
    if (ctx->wwant || (ctx->output && ctx->output->rb.nbytes)) {
        which |= LCB_WRITE_EVENT;
    }

    if (!which) {
        deactivate_watcher(ctx);
        return;
    }

    IOT_V0EV(io).watch(IOT_ARG(io), CTX_FD(ctx), ctx->event, which, ctx, E_handler);
    ctx->evactive = 1;
}

void
lcbio_ctx_schedule(lcbio_CTX *ctx)
{
    if (ctx->entered || ctx->err || ctx->state != ES_ACTIVE) {
        /* don't schedule events on i/o errors or on entered state */
        return;
    }
    if (IOT_IS_EVENT(ctx->io)) {
        E_schedule(ctx);
    } else {
        C_schedule(ctx);
    }
}

/** Extended function used for write-on-callback mode */
static int
E_put_ex(lcbio_CTX *ctx, lcb_IOV *iov, unsigned niov, unsigned nb)
{
    lcb_ssize_t nw;
    lcbio_TABLE *iot = ctx->io;
    lcb_socket_t fd = CTX_FD(ctx);

    GT_WRITE_AGAIN:
    nw = IOT_V0IO(iot).sendv(IOT_ARG(iot), fd, iov, niov);
    if (nw > 0) {
        ctx->procs.cb_flush_done(ctx, nb, nw);
        return 1;

    } else if (nw == -1) {
        switch (IOT_ERRNO(iot)) {
        case EINTR:
            /* jump back to retry */
            goto GT_WRITE_AGAIN;

        case C_EAGAIN:
        case EWOULDBLOCK:
            nw = 0;
            /* indicate zero bytes were written, but don't send an error */
            goto GT_WRITE0;
        default:
            /* pretend all the bytes were written and deliver an error during
             * the next event loop iteration. */
            nw = nb;
            lcbio_ctx_senderr(ctx, convert_lcberr(ctx, LCBIO_IOERR));
            goto GT_WRITE0;
        }
    } else {
        /* connection closed. pretend everything was written and send an error */
        nw = nb;
        lcbio_ctx_senderr(ctx, convert_lcberr(ctx, LCBIO_SHUTDOWN));
        goto GT_WRITE0;
    }

    GT_WRITE0:
    ctx->procs.cb_flush_done(ctx, nb, nw);
    return 0;
}

static void
Cw_ex_handler(lcb_sockdata_t *sd, int status, void *wdata)
{
    lcbio_CTX *ctx = ((lcbio_SOCKET *)sd->lcbconn)->ctx;
    unsigned nflushed = (uintptr_t)wdata;
    ctx->procs.cb_flush_done(ctx, nflushed, nflushed);

    ctx->npending--;
    assert(ctx->state == ES_ACTIVE);

    if (status != 0) {
        lcbio_ctx_senderr(ctx, convert_lcberr(ctx, LCBIO_IOERR));
    }
}

static int
C_put_ex(lcbio_CTX *ctx, lcb_IOV *iov, unsigned niov, unsigned nb)
{
    lcbio_TABLE *iot = ctx->io;
    lcb_sockdata_t *sd = CTX_SD(ctx);
    int status = IOT_V1(iot).write2(IOT_ARG(iot),
        sd, iov, niov, (void *)(uintptr_t)nb, Cw_ex_handler);
    if (status) {
        /** error! */
        lcbio_OSERR saverr = IOT_ERRNO(iot);
        ctx->procs.cb_flush_done(ctx, nb, nb);
        lcbio_ctx_senderr(ctx, lcbio_mklcberr(saverr, ctx->sock->settings));
        return 0;
    } else {
        ctx->npending++;
        return 1;
    }
}

int
lcbio_ctx_put_ex(lcbio_CTX *ctx, lcb_IOV *iov, unsigned niov, unsigned nb)
{
    lcbio_TABLE *iot = ctx->io;
    if (IOT_IS_EVENT(iot)) {
        return E_put_ex(ctx, iov, niov, nb);
    } else {
        return C_put_ex(ctx, iov, niov, nb);
    }
}

void
lcbio_ctx_wwant(lcbio_CTX *ctx)
{
    if (!IOT_IS_EVENT(ctx->io)) {
        ctx->procs.cb_flush_ready(ctx);
    } else {
        ctx->wwant = 1;
    }
}

void
lcbio_ctx_senderr(lcbio_CTX *ctx, lcb_error_t err)
{
    if (ctx->err == LCB_SUCCESS) {
        ctx->err = err;
    }
    deactivate_watcher(ctx);
    lcbio_async_signal(ctx->as_err);
}

void
lcbio_ctx_dump(lcbio_CTX *ctx)
{
    printf("IOCTX=%p. SUBSYS=%s\n", (void*)ctx, ctx->subsys);
    printf("  Pending=%d\n", ctx->npending);
    printf("  ReqRead=%d\n", ctx->rdwant);
    printf("  WantWrite=%d\n", ctx->wwant);
    printf("  Entered=%d\n", ctx->entered);
    printf("  Active=%d\n", ctx->state == ES_ACTIVE);
    printf("  SOCKET=%p\n", (void*)ctx->sock);
    printf("    Model=%s\n", ctx->io->model == LCB_IOMODEL_EVENT ? "Event" : "Completion");
    if (IOT_IS_EVENT(ctx->io)) {
        printf("    FD=%d\n", ctx->sock->u.fd);
        printf("    Watcher Active=%d\n", ctx->evactive);
    } else {
        printf("    SD=%p\n", (void *)ctx->sock->u.sd);
        printf("    Reading=%d\n", ctx->sock->u.sd->is_reading);
    }
}
