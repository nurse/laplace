#include "laplace.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <ruby/debug.h>

/* laplace_event_t */
typedef struct {
    rb_event_flag_t event;
    int lineno;
    VALUE method;
    VALUE thread;
    VALUE klass;
    VALUE path;
    struct timespec time;
} laplace_event_t;

/* laplace_buf_t */
typedef struct {
    char *head;
    char *cur;
    char *flushed;
    char *end;
    pthread_mutex_t mutex;
} laplace_buf_t;

static void
laplace_buf_init(laplace_buf_t *buf) {
    size_t size = 1048576 * 5;
    buf->head = xmalloc(size);
    buf->cur = buf->head;
    buf->cur = buf->head;
    buf->flushed = buf->head;
    buf->end = buf->head + size;
    pthread_mutex_init(&buf->mutex, NULL);
}

static void
laplace_buf_free(laplace_buf_t *buf) {
    xfree(buf->head);
    pthread_mutex_destroy(&buf->mutex);
}

static int
laplace_buf_flush(laplace_buf_t *buf, int fd) {
    int r = pthread_mutex_lock(&buf->mutex);
    if (r == 0) {
	size_t len;
	int ret;
	//fprintf(stderr,"%s:%d: %d %zx %zx\n",__func__,__LINE__,fd, buf->cur-buf->head,buf->flushed-buf->head);
	if (buf->flushed == buf->cur) {
	} else if (buf->flushed < buf->cur) {
	    ret = write(fd, buf->flushed, buf->cur - buf->flushed);
	    if (ret < 0) {
		pthread_mutex_unlock(&buf->mutex);
		fprintf(stderr,"%s:%d: %d %s\n",__func__,__LINE__,errno, strerror(errno));
		return -1;
	    }
	    //fprintf(stderr,"%s:%d: %d\n",__func__,__LINE__,r);
	    buf->flushed = buf->cur;
	} else if (buf->flushed > buf->cur) {
	    ret = write(fd, buf->flushed, buf->end - buf->flushed);
	    if (ret < 0) {
		pthread_mutex_unlock(&buf->mutex);
		fprintf(stderr,"%s:%d: %d %s\n",__func__,__LINE__,errno, strerror(errno));
		return -1;
	    }
	    buf->flushed = buf->head;
	}
	pthread_mutex_unlock(&buf->mutex);
	return 0;
    } else {
	//fprintf(stderr, "%s:%d: %d %s\n", __func__, __LINE__, r, strerror(r));
    }
    return -1;
}

static void
laplace_buf_write(laplace_buf_t *buf, const laplace_event_t *e) {
    int r = pthread_mutex_lock(&buf->mutex);
    if (r == 0) {
	const char *p = (const char *)e;
	char *old = buf->cur;
	size_t len = sizeof(laplace_event_t);
	// assert(buf->end - buf->head > len)
	buf->cur += len;
	if (buf->end < buf->cur) {
	    if (old <= buf->flushed) {
		/* lost events */
		buf->flushed = buf->head;
	    }
	    old = buf->head;
	    buf->cur = buf->head + len;
	} else {
	    if (old < buf->flushed && buf->flushed <= buf->cur) {
		/* lost events */
		buf->flushed = buf->head;
	    }
	}
	memcpy(old, p, len);
	pthread_mutex_unlock(&buf->mutex);
	//fprintf(stderr,"%s:%d: %zx %zx\n",__func__,__LINE__,buf->cur-buf->head,buf->flushed-buf->head);
    } else {
	fprintf(stderr, "%s:%d: %d %s\n", __func__, __LINE__, r, strerror(r));
    }
}

/* laplace_t */
typedef struct laplace_object {
    VALUE io; /* to prevent GC */
    laplace_buf_t buf;
    VALUE tpval;
    int fd;
    int enabled;
    pthread_t thread;
    pthread_cond_t cond;
} laplace_t;

static void
laplace_mark(void *ptr)
{
    laplace_t *tobj = ptr;
    rb_gc_mark(tobj->io);
    rb_gc_mark(tobj->tpval);
}

static void laplace_disable0(laplace_t *tobj);
static void
laplace_free(void *ptr)
{
    laplace_t *tobj = ptr;
    laplace_disable0(tobj);
    laplace_buf_free(&tobj->buf);
    pthread_cond_destroy(&tobj->cond);
}

static size_t
laplace_memsize(const void *ptr)
{
    laplace_t *tobj = (laplace_t *)ptr;
    return sizeof(laplace_t) + sizeof(tobj->buf.head);
}

static const rb_data_type_t laplace_data_type = {
    "laplace",
    {
	laplace_mark, laplace_free, laplace_memsize,
    },
#ifdef RUBY_TYPED_FREE_IMMEDIATELY
    0,
    0,
    RUBY_TYPED_FREE_IMMEDIATELY|RUBY_TYPED_WB_PROTECTED
#endif
};

static VALUE
laplace_s_alloc(VALUE klass)
{
    VALUE obj;
    struct laplace_object *tobj;
    obj = TypedData_Make_Struct(klass, struct laplace_object, &laplace_data_type, tobj);
    return obj;
}

#define LAPLACE_INIT_P(tobj) ((tobj)->io)

static struct laplace_object *
get_laplace_val(VALUE obj)
{
    struct laplace_object *tobj;
    TypedData_Get_Struct(obj, struct laplace_object, &laplace_data_type, tobj);
    if (!LAPLACE_INIT_P(tobj)) {
	rb_raise(rb_eTypeError, "uninitialized %" PRIsVALUE, rb_obj_class(obj));
    }
    return tobj;
}
#define GetLaplaceVal(obj, tobj) ((tobj) = get_laplace_val(obj))

static struct laplace_object *
get_new_laplace_val(VALUE obj)
{
    struct laplace_object *tobj;
    TypedData_Get_Struct(obj, struct laplace_object, &laplace_data_type, tobj);
    if (LAPLACE_INIT_P(tobj)) {
	rb_raise(rb_eTypeError, "already initialized %" PRIsVALUE, rb_obj_class(obj));
    }
    return tobj;
}
#define GetNewLaplaceVal(obj, tobj) ((tobj) = get_new_laplace_val(obj))

/*
 * @overload new(io)
 *   @param io [IO] an IO object written result
 *
 * returns laplace object
 */
static VALUE
laplace_init(VALUE self, VALUE io)
{
    struct laplace_object *tobj;
    TypedData_Get_Struct(self, struct laplace_object, &laplace_data_type, tobj);
    laplace_buf_init(&tobj->buf);
    tobj->io = io;
    /* avoid io closes the fd */
    tobj->fd = dup(FIX2INT(rb_funcall(io, rb_intern("fileno"), 0)));
    tobj->tpval = Qnil;
    pthread_cond_init(&tobj->cond, NULL);
    return self;
}

/* @api private
 * For Ruby VM internal.
 */
static VALUE
laplace_init_copy(VALUE copy, VALUE self)
{
    struct laplace_object *tobj, *tcopy;

    if (!OBJ_INIT_COPY(copy, self)) return copy;
    GetLaplaceVal(self, tobj);
    GetNewLaplaceVal(copy, tcopy);
    MEMCPY(tcopy, tobj, struct laplace_object, 1);
    return copy;
}

static int
laplace_flush(laplace_t *m) {
    return laplace_buf_flush(&m->buf, m->fd);
}

static void
laplace_add(laplace_t *m, laplace_event_t *e) {
    return laplace_buf_write(&m->buf, e);
}

static void *
laplace_flush_loop(void *data) {
    laplace_t *m = (laplace_t *)data;
    pthread_cond_t *cond = &m->cond;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    //fprintf(stderr,"%s:%d: start\n",__func__,__LINE__);
    for (;;) {
	laplace_flush(m);
	if (!__atomic_load_n(&m->enabled, __ATOMIC_SEQ_CST)) break;
	struct timespec req;
	clock_gettime(CLOCK_REALTIME, &req);
	req.tv_nsec += 100*1000*1000;
	if (req.tv_nsec >= 1000*1000*1000) {
	    req.tv_nsec -= 1000*1000*1000;
	    req.tv_sec += 1;
	}
	int r = pthread_cond_timedwait(cond, &mutex, &req);
	//fprintf(stderr,"%s:%d: pthread_cond_timedwait:%d\n",__func__,__LINE__,r);
    }
    //fprintf(stderr,"%s:%d: finish\n",__func__,__LINE__);
    pthread_mutex_destroy(&mutex);
    return NULL;
}

static void
callback(VALUE tpval, void *data) {
    laplace_t *m = (laplace_t *)data;
    rb_trace_arg_t *trace_arg = rb_tracearg_from_tracepoint(tpval);
    laplace_event_t e;
    e.event = rb_tracearg_event_flag(trace_arg);
    //VALUE event = rb_tracearg_event(trace_arg);
    e.path = rb_tracearg_path(trace_arg);
    e.lineno = FIX2INT(rb_tracearg_lineno(trace_arg));
    e.klass = rb_tracearg_defined_class(trace_arg);
    e.method = rb_tracearg_method_id(trace_arg);
    e.thread = rb_thread_current();
    //VALUE callee = rb_tracearg_callee_id(trace_arg);
    //VALUE callee = rb_funcall(tpval, rb_intern("callee_id"), 0);
    clock_gettime(CLOCK_REALTIME, &e.time);
    laplace_add(m, &e);
    /*
    VALUE io = (VALUE)data;
    rb_io_write(io, rb_sprintf("%ld.%09ld: %s %s#%s %s\n",
		time.tv_sec, time.tv_nsec,
		RSTRING_PTR(rb_sym2str(event)),
		rb_class2name(klass),
		RSTRING_PTR(rb_sym2str(method)),
		RSTRING_PTR(rb_sym2str(callee)),
		RSTRING_PTR(path), FIX2INT(lineno)
		));
		*/
}

/*
 * call-seq:
 *	laplace.enabled? -> bool
 *
 *  Returns whether the tracepoint is enabled or not.
 */

static VALUE
laplace_enabled_p(VALUE self)
{
    int r;
    struct laplace_object *tobj;
    GetLaplaceVal(self, tobj);
    return __atomic_load_n(&tobj->enabled, __ATOMIC_SEQ_CST) ? Qtrue : Qfalse;
}

/*
 * call-seq:
 *	laplace.enable -> Qnil
 *
 *  Enable trace.
 */

static VALUE
laplace_enable(VALUE self)
{
    struct laplace_object *tobj;
    GetLaplaceVal(self, tobj);
    if (!__atomic_exchange_n(&tobj->enabled, true, __ATOMIC_SEQ_CST)) {
	int r = pthread_create(&tobj->thread, NULL, laplace_flush_loop, (void *)tobj);
	if (r) rb_sys_fail("pthread_create");
	if (NIL_P(tobj->tpval)) {
	    rb_event_flag_t events = RUBY_EVENT_CALL|RUBY_EVENT_RETURN|
		//RUBY_EVENT_C_CALL|RUBY_EVENT_C_RETURN|
		RUBY_EVENT_RAISE;
	    tobj->tpval = rb_tracepoint_new(Qnil, events, callback, (void *)tobj);
	}
	rb_tracepoint_enable(tobj->tpval);
    }
    return Qnil;

}

static void
laplace_disable0(laplace_t *tobj) {
    if (__atomic_exchange_n(&tobj->enabled, false, __ATOMIC_SEQ_CST)) {
	int r = pthread_cond_signal(&tobj->cond);
	if (r) rb_sys_fail("pthread_cond_signal");
	pthread_join(tobj->thread, NULL);
	if (r) rb_sys_fail("pthread_join");
	rb_tracepoint_disable(tobj->tpval);
    }
}

/*
 * call-seq:
 *	laplace.disable -> Qnil
 *
 *  Disable trace.
 */

static VALUE
laplace_disable(VALUE self)
{
    struct laplace_object *tobj;
    GetLaplaceVal(self, tobj);
    laplace_disable0(tobj);
    return Qnil;
}

VALUE rb_cLaplace;

/*
 * Document-class: Laplace
 *
 * For fast method tracing.
 */
void
Init_laplace(void)
{
    rb_cLaplace = rb_define_class("Laplace", rb_cObject);
    rb_define_alloc_func(rb_cLaplace, laplace_s_alloc);
    rb_define_method(rb_cLaplace, "initialize", laplace_init, 1);
    rb_define_method(rb_cLaplace, "initialize_copy", laplace_init_copy, 1);
    rb_define_method(rb_cLaplace, "enabled?", laplace_enabled_p, 0);
    rb_define_method(rb_cLaplace, "enable",   laplace_enable, 0);
    rb_define_method(rb_cLaplace, "disable",  laplace_disable, 0);
}
