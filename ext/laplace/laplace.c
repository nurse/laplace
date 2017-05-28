#include "laplace.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <ruby/debug.h>

VALUE rb_mLaplace;

/* laplace_buf_t */
typedef struct {
    char *head;
    char *cur;
    char *flushed;
    char *end;
} laplace_buf_t;

static void
laplace_buf_init(laplace_buf_t *buf) {
    size_t size = 1048576 * 5;
    buf->head = malloc(size);
    buf->cur = buf->head;
    buf->cur = buf->head;
    buf->flushed = buf->head;
    buf->end = buf->head + size;
}

static void
laplace_buf_flush(laplace_buf_t *buf, int fd) {
    size_t len;
    int ret;
    if (buf->flushed == buf->cur) {
    } else if (buf->flushed < buf->cur) {
	write(fd, buf->flushed, buf->cur - buf->flushed);
	buf->flushed = buf->cur;
    } else if (buf->flushed > buf->cur) {
#ifdef HAVE_WRITEV
#else
	if (write(fd, buf->flushed, buf->end - buf->flushed) == 0) {
	    buf->flushed = buf->head;
	}
#endif
    }
}

static void
laplace_buf_write(laplace_buf_t *buf, const char *p, size_t len) {
    size_t n = buf->end - buf->cur;
    if (n >= len) {
	memcpy(buf->flushed, p, len);
	buf->flushed += len;
	if (n == len) buf->flushed = buf->head;
    } else {
	memcpy(buf->flushed, p, n);
	p += n;
	len -= n;
	memcpy(buf->cur, p, len);
	buf->flushed = buf->head + len;
    }
}

/* laplace_event_t */
typedef struct {
} laplace_event_t;

/* laplace_t */
typedef struct {
    int fd;
    pthread_mutex_t mutex;
    laplace_buf_t buf;
} laplace_t;

static laplace_t *
laplace_new(int fd) {
    laplace_t *m = malloc(sizeof(laplace_t));
    pthread_mutex_init(&m->mutex, NULL);
    m->fd = dup(fd);
    fcntl(F_SETFL, m->fd, O_NONBLOCK);
    laplace_buf_init(&m->buf);
    return m;
}

static void
laplace_flush(laplace_t *m) {
    if (pthread_mutex_trylock(&m->mutex) == 0) {
	laplace_buf_flush(&m->buf, m->fd);
    } else if (errno == EBUSY) {
	/* should retry later */
    } else {
	fprintf(stderr, "%d: unknown errno: %s\n", errno, strerror(errno));
    }
}

static void
laplace_add(laplace_t *m, laplace_event_t *e) {
    if (pthread_mutex_trylock(&m->mutex) == 0) {
	//laplace_buf_add(&m->buf, m->fd);
    } else if (errno == EBUSY) {
	/* should retry later */
    } else {
	fprintf(stderr, "%d: unknown errno: %s\n", errno, strerror(errno));
    }
}

typedef struct {
    rb_event_flag_t event;
    ID id;
    ID called_id;
    int lineno;
    //rb_thread_t *th;
    VALUE path;
    struct timespec time;
} lp_event_t;

static void
callback(VALUE tpval, void *data) {
    VALUE io = (VALUE)data;
    laplace_t *m = (laplace_t *)data;
    rb_trace_arg_t *trace_arg = rb_tracearg_from_tracepoint(tpval);
    //rb_event_flag_t ef = rb_tracearg_event_flag(trace_arg);
    VALUE event = rb_tracearg_event(trace_arg);
    VALUE path = rb_tracearg_path(trace_arg);
    VALUE lineno = rb_tracearg_lineno(trace_arg);
    VALUE klass = rb_tracearg_defined_class(trace_arg);
    VALUE method = rb_tracearg_method_id(trace_arg);
    //VALUE callee = rb_tracearg_callee_id(trace_arg);
    VALUE callee = rb_funcall(tpval, rb_intern("callee_id"), 0);
    rb_io_write(io, rb_sprintf("%s %s#%s %s %s:%d\n",
	    RSTRING_PTR(rb_sym2str(event)),
	    rb_class2name(klass),
	    RSTRING_PTR(rb_sym2str(method)),
	    RSTRING_PTR(rb_sym2str(callee)),
	    RSTRING_PTR(path), FIX2INT(lineno)
	    ));
}

/*
 * call-seq:
 *	Laplace.trace(io) -> tracepoint
 *
 *  Returns tracepoint which outputs method call and return,
 *  and raised exceptions.
 */

static VALUE
lp_trace(VALUE dummy, VALUE io)
{
    rb_event_flag_t events = RUBY_EVENT_CALL|RUBY_EVENT_C_CALL|RUBY_EVENT_RETURN|RUBY_EVENT_C_RETURN|RUBY_EVENT_RAISE;
    return rb_tracepoint_new(Qnil, events, callback, (void *)io);
}

/*
 * Document-class: Laplace
 *
 * For fast method tracing.
 */
void
Init_laplace(void)
{
  rb_mLaplace = rb_define_module("Laplace");
  rb_define_singleton_method(rb_mLaplace, "trace", lp_trace, 1);
}
