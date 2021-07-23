
/*
 * Copyright (C) Roman Arutyunyan
 */


#include <Python.h>

/*
 * The Python.h file is included first in contrast with normal nginx
 * practice.  Python headers deliberately define the following macros:
 * _GNU_SOURCE, _POSIX_C_SOURCE, _XOPEN_SOURCE.
 * When compiling on Linux, they are defined by nginx or system headers
 * with Python possibly redefining them.  Since nginx is built with -Werror
 * compilation option, macros redefinition causes compilation errors.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event_posted.h>
#include <ucontext.h>
#include "ngx_python.h"


typedef struct {
    PyObject              *ns;
    size_t                 stack_size;
} ngx_python_conf_t;


struct ngx_python_ctx_s {
    PyCodeObject          *code;
    PyObject              *ns;
    PyObject              *result;

    ngx_event_t           *wake;
    ngx_pool_t            *pool;
    ngx_log_t             *log;

    ngx_resolver_t        *resolver;
    ngx_msec_t             resolver_timeout;

    size_t                 stack_size;

#if !(NGX_PYTHON_SYNC)

    void                  *stack;

    ucontext_t             uc;
    ucontext_t             ruc;

    int                    recursion_depth;
    struct _frame         *frame;
    PyObject              *exc_type;
    PyObject              *exc_value;
    PyObject              *exc_traceback;

    ngx_uint_t             terminate;  /* unsigned  terminate:1; */

#endif
};


typedef struct {
    PyObject              *ns;
    u_char                *name;
} ngx_python_ns_cleanup_t;


#if !(NGX_PYTHON_SYNC)
static ngx_python_ctx_t *ngx_python_set_ctx(ngx_python_ctx_t *ctx);
static void ngx_python_task_handler();
static void ngx_python_cleanup_ctx(void *data);
#endif
static char *ngx_python_include_file(ngx_conf_t *cf, PyObject *ns, char *file);
static void ngx_python_decref(void *data);
static PyObject *ngx_python_init_namespace(ngx_conf_t *cf);
static void ngx_python_cleanup_namespace(void *data);

static void *ngx_python_create_conf(ngx_cycle_t *cycle);
static char *ngx_python_init_conf(ngx_cycle_t *cycle, void *conf);
static ngx_int_t ngx_python_init_worker(ngx_cycle_t *cycle);


static ngx_command_t  ngx_python_commands[] = {

    { ngx_string("python"),
      NGX_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_python_set_slot,
      0,
      0,
      NULL },

    { ngx_string("python_include"),
      NGX_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_python_include_set_slot,
      0,
      0,
      NULL },

    { ngx_string("python_stack_size"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      0,
      offsetof(ngx_python_conf_t, stack_size),
      NULL },

      ngx_null_command
};


static ngx_core_module_t  ngx_python_module_ctx = {
    ngx_string("python"),
    ngx_python_create_conf,
    ngx_python_init_conf
};


ngx_module_t  ngx_python_module = {
    NGX_MODULE_V1,
    &ngx_python_module_ctx,                /* module context */
    ngx_python_commands,                   /* module directives */
    NGX_CORE_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    ngx_python_init_worker,                /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


#if !(NGX_PYTHON_SYNC)

ngx_python_ctx_t  * volatile ngx_python_ctx;


ngx_python_ctx_t *
ngx_python_get_ctx()
{
    return ngx_python_ctx;
}


static ngx_python_ctx_t *
ngx_python_set_ctx(ngx_python_ctx_t *ctx)
{
    ngx_python_ctx_t  *pctx;

    pctx = ngx_python_ctx;
    ngx_python_ctx = ctx;

    return pctx;
}


ngx_int_t
ngx_python_yield()
{
    ngx_python_ctx_t  *ctx;

    /* TODO throw more specific exceptions */

    ctx = ngx_python_get_ctx();
    if (ctx == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "blocking calls are not allowed");
        return NGX_ERROR;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_CORE, ctx->log, 0, "python yield");

    if (swapcontext(&ctx->uc, &ctx->ruc)) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NGX_ERROR;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_CORE, ctx->log, 0, "python regain");

    if (ctx->terminate) {
        PyErr_SetString(PyExc_RuntimeError, "terminated");
        ngx_log_debug0(NGX_LOG_DEBUG_CORE, ctx->log, 0, "python terminate");
        return NGX_ERROR;
    }

    return NGX_OK;
}


void
ngx_python_wakeup(ngx_python_ctx_t *ctx)
{
    if (!ctx->terminate) {
        ngx_post_event(ctx->wake, &ngx_posted_events);
    }
}

#endif


ngx_python_ctx_t *
ngx_python_create_ctx(ngx_pool_t *pool, ngx_log_t *log)
{
    ngx_python_ctx_t    *ctx;
    ngx_python_conf_t   *pcf;
#if !(NGX_PYTHON_SYNC)
    ngx_pool_cleanup_t  *cln;
#endif

    pcf = (ngx_python_conf_t *) ngx_get_conf(ngx_cycle->conf_ctx,
                                             ngx_python_module);
    if (pcf->ns == NULL) {
        return NULL;
    }

    ctx = ngx_pcalloc(pool, sizeof(ngx_python_ctx_t));
    if (ctx == NULL) {
        return NULL;
    }

#if !(NGX_PYTHON_SYNC)

    cln = ngx_pool_cleanup_add(pool, 0);
    if (cln == NULL) {
        return NULL;
    }

    cln->handler = ngx_python_cleanup_ctx;
    cln->data = ctx;

#endif

    ctx->pool = pool;
    ctx->log = log;
    ctx->ns = pcf->ns;
    ctx->stack_size = pcf->stack_size;

    return ctx;
}


#if !(NGX_PYTHON_SYNC)

static void
ngx_python_cleanup_ctx(void *data)
{
    ngx_python_ctx_t  *ctx = data;

    PyObject  *result;

    ctx->terminate = 1;

    result = ctx->result;

    while (result == NGX_PYTHON_AGAIN) {
        result = ngx_python_eval(ctx, NULL, ctx->wake);
    }

    Py_XDECREF(result);
}

#endif


PyObject *
ngx_python_eval(ngx_python_ctx_t *ctx, PyCodeObject *code, ngx_event_t *wake)
{
    PyObject          *result;

#if !(NGX_PYTHON_SYNC)

    int                recursion_depth;
    PyObject          *exc_type, *exc_value, *exc_traceback;
    struct _frame     *frame;
    PyThreadState     *ps;
    ngx_python_ctx_t  *pctx;

    if (wake) {
        if (ctx->result == NULL) {
            if (ctx->stack == NULL) {
                ctx->stack = ngx_palloc(ctx->pool, ctx->stack_size);
                if (ctx->stack == NULL) {
                    return NULL;
                }
            }

            if (getcontext(&ctx->uc) == -1) {
                ngx_log_debug0(NGX_LOG_DEBUG_CORE, ctx->log, ngx_errno,
                               "getcontext() failed");
                return NULL;
            }

            ctx->uc.uc_stack.ss_size = ctx->stack_size;
            ctx->uc.uc_stack.ss_sp = ctx->stack;
            ctx->uc.uc_link = &ctx->ruc;

            makecontext(&ctx->uc, &ngx_python_task_handler, 0);

            ctx->code = code;
            ctx->wake = wake;
            ctx->result = NGX_PYTHON_AGAIN;
        }

        pctx = ngx_python_set_ctx(ctx);

        ps = PyThreadState_GET();

        recursion_depth = ps->recursion_depth;
        frame = ps->frame;
        exc_type = ps->curexc_type;
        exc_value = ps->curexc_value;
        exc_traceback = ps->curexc_traceback;

        ps->recursion_depth = ctx->recursion_depth;
        ps->frame = ctx->frame;
        ps->curexc_type = ctx->exc_type;
        ps->curexc_value = ctx->exc_value;
        ps->curexc_traceback = ctx->exc_traceback;

        if (swapcontext(&ctx->ruc, &ctx->uc) == -1) {
            ngx_log_error(NGX_LOG_ERR, ctx->log, ngx_errno,
                          "swapcontext() failed");
        }

        ctx->recursion_depth = ps->recursion_depth;
        ctx->frame = ps->frame;
        ctx->exc_type = ps->curexc_type;
        ctx->exc_value = ps->curexc_value;
        ctx->exc_traceback = ps->curexc_traceback;

        ps->recursion_depth = recursion_depth;
        ps->frame = frame;
        ps->curexc_type = exc_type;
        ps->curexc_value = exc_value;
        ps->curexc_traceback = exc_traceback;

        (void) ngx_python_set_ctx(pctx);

        result = ctx->result;
        if (result != NGX_PYTHON_AGAIN) {
            ctx->code = NULL;
            ctx->wake = NULL;
            ctx->result = NULL;
        }

        return result;
    }

    pctx = ngx_python_set_ctx(NULL);

#endif

    result = PyEval_EvalCode((PyObject *) code, ctx->ns, ctx->ns);
    if (result == NULL) {
        ngx_log_error(NGX_LOG_ERR, ctx->log, 0, "python error: %s",
                      ngx_python_get_error(ctx->pool));
    }

#if !(NGX_PYTHON_SYNC)
    (void) ngx_python_set_ctx(pctx);
#endif

    return result;
}


#if !(NGX_PYTHON_SYNC)

static void
ngx_python_task_handler()
{
    ngx_python_ctx_t  *ctx;

    ctx = ngx_python_get_ctx();

    ngx_log_debug0(NGX_LOG_DEBUG_CORE, ctx->log, 0, "python task handler");

    ctx->result = PyEval_EvalCode((PyObject *) ctx->code, ctx->ns, ctx->ns);
    if (ctx->result == NULL) {
        ngx_log_error(NGX_LOG_ERR, ctx->log, 0, "python error: %s",
                      ngx_python_get_error(ctx->pool));
    }
}

#endif


void
ngx_python_set_resolver(ngx_python_ctx_t *ctx, ngx_resolver_t *resolver,
    ngx_msec_t timeout)
{
    ctx->resolver = resolver;
    ctx->resolver_timeout = timeout;
}


ngx_resolver_t *
ngx_python_get_resolver(ngx_python_ctx_t *ctx, ngx_msec_t *timeout)
{
    *timeout = ctx->resolver_timeout;
    return ctx->resolver;
}


PyObject *
ngx_python_set_value(ngx_python_ctx_t *ctx, const char *name, PyObject *value)
{
    PyObject  *old;

    old = PyDict_GetItemString(ctx->ns, name);

    if (old == NULL) {
        if (PyDict_SetItemString(ctx->ns, name, value) < 0) {
            ngx_log_error(NGX_LOG_ERR, ctx->log, 0,
                          "python error: %s", ngx_python_get_error(ctx->pool));
        }
    }

    return old;
}


void
ngx_python_reset_value(ngx_python_ctx_t *ctx, const char *name, PyObject *old)
{
    if (old == NULL) {
        if (PyDict_DelItemString(ctx->ns, name) < 0) {
            ngx_log_error(NGX_LOG_ERR, ctx->log, 0,
                          "python error: %s", ngx_python_get_error(ctx->pool));
        }
    }
}


ngx_int_t
ngx_python_active(ngx_conf_t *cf)
{
    ngx_python_conf_t  *pcf;

    pcf = (ngx_python_conf_t *) ngx_get_conf(cf->cycle->conf_ctx,
                                             ngx_python_module);

    return pcf->ns ? NGX_OK : NGX_DECLINED;
}


char *
ngx_python_set_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    PyObject   *ret, *ns;
    ngx_str_t  *value;

    ns = ngx_python_init_namespace(cf);
    if (ns == NULL) {
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    ret = PyRun_StringFlags((char *) value[1].data, Py_file_input, ns, ns,
                            NULL);
    if (ret == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "python error: %s",
                           ngx_python_get_error(cf->pool));
        return NGX_CONF_ERROR;
    }

    Py_DECREF(ret);

    return NGX_CONF_OK;
}


char *
ngx_python_include_set_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char         *rv;
    PyObject     *ns;
    ngx_int_t     n;
    ngx_str_t    *value, file, name;
    ngx_glob_t    gl;

    ns = ngx_python_init_namespace(cf);
    if (ns == NULL) {
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;
    file = value[1];

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, cf->log, 0, "python_include %s",
                   file.data);

    if (ngx_conf_full_name(cf->cycle, &file, 1) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (strpbrk((char *) file.data, "*?[") == NULL) {

        ngx_log_debug1(NGX_LOG_DEBUG_CORE, cf->log, 0, "python_include %s",
                       file.data);

        return ngx_python_include_file(cf, ns, (char *) file.data);
    }

    ngx_memzero(&gl, sizeof(ngx_glob_t));

    gl.pattern = file.data;
    gl.log = cf->log;
    gl.test = 1;

    if (ngx_open_glob(&gl) != NGX_OK) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                           ngx_open_glob_n " \"%s\" failed", file.data);
        return NGX_CONF_ERROR;
    }

    rv = NGX_CONF_OK;

    for ( ;; ) {
        n = ngx_read_glob(&gl, &name);

        if (n != NGX_OK) {
            break;
        }

        file.len = name.len++;
        file.data = ngx_pstrdup(cf->pool, &name);
        if (file.data == NULL) {
            return NGX_CONF_ERROR;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_CORE, cf->log, 0, "python_include %s",
                       file.data);

        rv = ngx_python_include_file(cf, ns, (char *) file.data);

        if (rv != NGX_CONF_OK) {
            break;
        }
    }

    ngx_close_glob(&gl);

    return rv;
}


static char *
ngx_python_include_file(ngx_conf_t *cf, PyObject *ns, char *file)
{
    FILE      *fp;
    PyObject  *ret;

    fp = fopen(file, "r");
    if (fp == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                           "fopen() \"%s\" failed", file);
        return NGX_CONF_ERROR;
    }

    ret = PyRun_FileExFlags(fp, file, Py_file_input, ns, ns, 0, NULL);

    fclose(fp);

    if (ret == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "python error: %s",
                           ngx_python_get_error(cf->pool));
        return NGX_CONF_ERROR;
    }

    Py_DECREF(ret);

    return NGX_CONF_OK;
}


PyCodeObject *
ngx_python_compile(ngx_conf_t *cf, u_char *script)
{
    u_char              *p;
    size_t               len;
    PyObject            *code;
    ngx_pool_cleanup_t  *cln;

    if (ngx_python_init_namespace(cf) == NULL) {
        return NULL;
    }

    len = cf->conf_file->file.name.len + 1 + NGX_INT_T_LEN + 1;

    p = ngx_pnalloc(cf->pool, len);
    if (p == NULL) {
        return NULL;
    }

    ngx_sprintf(p, "%V:%ui%Z", &cf->conf_file->file.name, cf->conf_file->line);

    code = Py_CompileString((char *) script, (char *) p, Py_eval_input);

    if (code == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "python error: %s",
                           ngx_python_get_error(cf->pool));
        return NULL;
    }

    cln = ngx_pool_cleanup_add(cf->pool, 0);
    if (cln == NULL) {
        Py_DECREF(code);
        return NULL;
    }

    cln->handler = ngx_python_decref;
    cln->data = code;

    return (PyCodeObject *) code;
}


static void
ngx_python_decref(void *data)
{
    PyObject *obj = data;

    Py_DECREF(obj);
}


static PyObject *
ngx_python_init_namespace(ngx_conf_t *cf)
{
    u_char                   *name;
    PyObject                 *ns, *m;
    ngx_python_conf_t        *pcf;
    ngx_pool_cleanup_t       *cln;
    ngx_python_ns_cleanup_t  *nc;
    static ngx_int_t          initialized;
    static ngx_uint_t         counter;

    pcf = (ngx_python_conf_t *) ngx_get_conf(cf->cycle->conf_ctx,
                                             ngx_python_module);
    if (pcf->ns) {
        return pcf->ns;
    }

    if (!initialized) {
        initialized = 1;

        Py_Initialize();

        m = PyImport_ImportModule("ngx");
        if (m == NULL) {
            return NULL;
        }

        PyModule_AddIntConstant(m, "OK", NGX_OK);
        PyModule_AddIntConstant(m, "ERROR", NGX_ERROR);
        PyModule_AddIntConstant(m, "AGAIN", NGX_AGAIN);
        PyModule_AddIntConstant(m, "BUSY", NGX_BUSY);
        PyModule_AddIntConstant(m, "DONE", NGX_DONE);
        PyModule_AddIntConstant(m, "DECLINED", NGX_DECLINED);
        PyModule_AddIntConstant(m, "ABORT", NGX_ABORT);

        PyModule_AddIntConstant(m, "LOG_EMERG", NGX_LOG_EMERG);
        PyModule_AddIntConstant(m, "LOG_ALERT", NGX_LOG_ALERT);
        PyModule_AddIntConstant(m, "LOG_CRIT", NGX_LOG_CRIT);
        PyModule_AddIntConstant(m, "LOG_ERR", NGX_LOG_ERR);
        PyModule_AddIntConstant(m, "LOG_WARN", NGX_LOG_WARN);
        PyModule_AddIntConstant(m, "LOG_NOTICE", NGX_LOG_NOTICE);
        PyModule_AddIntConstant(m, "LOG_INFO", NGX_LOG_INFO);
        PyModule_AddIntConstant(m, "LOG_DEBUG", NGX_LOG_DEBUG);

        PyModule_AddIntConstant(m, "SEND_LAST", 1);
        PyModule_AddIntConstant(m, "SEND_FLUSH", 2);
    }

    nc = ngx_palloc(cf->pool, sizeof(ngx_python_ns_cleanup_t));
    if (nc == NULL) {
        return NULL;
    }

    cln = ngx_pool_cleanup_add(cf->pool, 0);
    if (cln == NULL) {
        return NULL;
    }

    name = ngx_pnalloc(cf->pool, 4 + NGX_INT_T_LEN);
    if (name == NULL) {
        return NULL;
    }

    /* generate a unique namespace name */

    ngx_sprintf(name, "ngx%ui%Z", counter++);

    m = PyImport_AddModule((char *) name);
    if (m == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "could not add \"%s\" Python module", name);
        return NULL;
    }

    ns = PyModule_GetDict(m);
    if (ns == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "could not get \"%s\" Python module dictionary",
                           name);
        return NULL;
    }

    Py_INCREF(ns);

    nc->ns = ns;
    nc->name = name;

    cln->handler = ngx_python_cleanup_namespace;
    cln->data = nc;

    if (PyDict_SetItemString(ns, "__builtins__", PyEval_GetBuiltins()) < 0) {
        return NULL;
    }

    pcf->ns = ns;

    return ns;
}


static void
ngx_python_cleanup_namespace(void *data)
{
    ngx_python_ns_cleanup_t  *nc = data;

    PyObject  *modules;

    Py_DECREF(nc->ns);

    modules = PyImport_GetModuleDict();

    if (PyDict_GetItemString(modules, (char *) nc->name) == NULL) {
        return;
    }

    if (PyDict_DelItemString(modules, (char *) nc->name) < 0) {
        /* XXX error removing module from sys.modules */
    }
}


u_char *
ngx_python_get_error(ngx_pool_t *pool)
{
    long         line;
    char        *text, *file;
    size_t       len;
    u_char      *p;
    PyObject    *type, *value, *traceback, *str, *module, *func, *ret, *frame,
                *obj;
    Py_ssize_t   size;

    /* PyErr_Print(); */

    str = NULL;
    module = NULL;
    func = NULL;
    ret = NULL;

    text = "";
    file = "";
    line = 0;

    PyErr_Fetch(&type, &value, &traceback);
    if (type == NULL) {
        goto done;
    }

    PyErr_NormalizeException(&type, &value, &traceback);
    if (type == NULL) {
        goto done;
    }

    str = PyObject_Str(value);
    if (str && PyBytes_Check(str)) {
        text = PyBytes_AsString(str);
    }

    module = PyImport_ImportModule("traceback");
    if (module == NULL) {
        goto done;
    }

    func = PyObject_GetAttrString(module, "extract_tb");
    if (func == NULL || !PyCallable_Check(func)) {
        goto done;
    }

    ret = PyObject_CallFunctionObjArgs(func, traceback, NULL);
    if (ret == NULL || !PyList_Check(ret)) {
        goto done;
    }

    size = PyList_Size(ret);
    if (size <= 0) {
        goto done;
    }

    frame = PyList_GetItem(ret, size - 1);
    if (frame == NULL || !PyTuple_Check(frame)) {
        goto done;
    }

    obj = PyTuple_GetItem(frame, 0);
    if (obj &&  PyBytes_Check(obj)) {
        file = PyBytes_AsString(obj);
    }

    obj = PyTuple_GetItem(frame, 1);
    if (obj && PyLong_Check(obj)) {
        line = PyLong_AsLong(obj);
    }

done:

    PyErr_Clear();

    len = ngx_strlen(text) + 2 + ngx_strlen(file) + 1 + NGX_INT_T_LEN + 2;

    p = ngx_pnalloc(pool, len);
    if (p == NULL) {
        return (u_char *) "";
    }

    ngx_sprintf(p, "%s [%s:%l]%Z", text, file, line);

    Py_XDECREF(str);
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);
    Py_XDECREF(module);
    Py_XDECREF(func);
    Py_XDECREF(ret);

    return p;
}


static void *
ngx_python_create_conf(ngx_cycle_t *cycle)
{
    ngx_python_conf_t  *pcf;

    pcf = ngx_pcalloc(cycle->pool, sizeof(ngx_python_conf_t));
    if (pcf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     pcf->ns = NULL;
     *
     */

    pcf->stack_size = NGX_CONF_UNSET_SIZE;

    return pcf;
}


static char *
ngx_python_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_python_conf_t *pcf = conf;

    ngx_conf_init_size_value(pcf->stack_size, 32768);

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_python_init_worker(ngx_cycle_t *cycle)
{
#if !(NGX_PYTHON_SYNC)

    ngx_python_conf_t  *pcf;

    pcf = (ngx_python_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                             ngx_python_module);

    if (pcf->ns) {
        if (ngx_python_sleep_install(cycle) != NGX_OK) {
            return NGX_ERROR;
        }

        if (ngx_python_socket_install(cycle) != NGX_OK) {
            return NGX_ERROR;
        }

        if (ngx_python_resolve_install(cycle) != NGX_OK) {
            return NGX_ERROR;
        }
    }

#endif

    return NGX_OK;
}
