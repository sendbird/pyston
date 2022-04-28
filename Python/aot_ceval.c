
/* Execute compiled code */

/* XXX TO DO:
   XXX speed up searching for keywords by using a dictionary
   XXX document it!
   */

/* enable more aggressive intra-module optimizations, where available */
#define PY_LOCAL_AGGRESSIVE

#include "Python.h"
#include "pycore_ceval.h"
#include "pycore_code.h"
#include "pycore_object.h"
#include "pycore_pyerrors.h"
#include "pycore_pylifecycle.h"
#include "pycore_pystate.h"
#include "pycore_tupleobject.h"

#include "code.h"
#include "dictobject.h"
#include "frameobject.h"
#include "opcode.h"
#include "pydtrace.h"
#include "setobject.h"
#include "structmember.h"

#include <ctype.h>
#include <sys/mman.h>

#include "aot.h"

#ifdef Py_DEBUG
/* For debugging the interpreter: */
// Pyston change: disable LLTRACE since we don't support it right now
//#define LLTRACE  1      /* Low-level trace feature */
#define CHECKEXC 1      /* Double-check exception checking */
#endif

#if !defined(Py_BUILD_CORE)
#  error "ceval.c must be build with Py_BUILD_CORE define for best performance"
#endif

/* Private API for the LOAD_METHOD opcode. */
extern int _PyObject_GetMethod(PyObject *, PyObject *, PyObject **);

uint64_t _PyDict_GetDictKeyVersionFromSplitDict(PyObject *op);
uint64_t _PyDict_GetDictKeyVersionFromKeys(PyObject *op);
PyObject *_PyDict_GetItemFromSplitDict(PyObject *op, Py_ssize_t index);
int _PyDict_SetItemFromSplitDict(PyObject *op, PyObject *key, Py_ssize_t index, PyObject* value);
int _PyDict_SetItemInitialFromSplitDict(PyTypeObject *tp, PyObject **dict_ptr, PyObject *key, Py_ssize_t index, PyObject* value);
Py_ssize_t _PyDict_GetItemIndexSplitDict(PyObject *op, PyObject *key);

typedef PyObject *(*callproc)(PyObject *, PyObject *, PyObject *);

/* Forward declarations */
/*Py_LOCAL_INLINE(PyObject *)*/ PyObject * call_function_ceval(
    PyThreadState *tstate, PyObject ***pp_stack,
    Py_ssize_t oparg, PyObject *kwnames);
/*static*/ PyObject * do_call_core(
    PyThreadState *tstate, PyObject *func,
    PyObject *callargs, PyObject *kwdict);

#ifdef LLTRACE
static int lltrace;
static int prtrace(PyThreadState *, PyObject *, const char *);
#endif
static int call_trace(Py_tracefunc, PyObject *,
                      PyThreadState *, PyFrameObject *,
                      int, PyObject *);
static int call_trace_protected(Py_tracefunc, PyObject *,
                                PyThreadState *, PyFrameObject *,
                                int, PyObject *);
/*static*/ void call_exc_trace(Py_tracefunc, PyObject *,
                           PyThreadState *, PyFrameObject *);
static int maybe_call_line_trace(Py_tracefunc, PyObject *,
                                 PyThreadState *, PyFrameObject *,
                                 int *, int *, int *, int *jit_first_trace_for_line);
static void maybe_dtrace_line(PyFrameObject *, int *, int *, int *);
static void dtrace_function_entry(PyFrameObject *);
static void dtrace_function_return(PyFrameObject *);

/*static*/ PyObject * cmp_outcome(PyThreadState *, int, PyObject *, PyObject *);
/*static*/ PyObject * import_name(PyThreadState *, PyFrameObject *,
                              PyObject *, PyObject *, PyObject *);
/*static*/ PyObject * import_from(PyThreadState *, PyObject *, PyObject *);
/*static*/ int import_all_from(PyThreadState *, PyObject *, PyObject *);
/*static*/ void format_exc_check_arg(PyThreadState *, PyObject *, const char *, PyObject *);
/*static*/ void format_exc_unbound(PyThreadState *tstate, PyCodeObject *co, int oparg);
static PyObject * unicode_concatenate(PyThreadState *, PyObject *, PyObject *,
                                      PyFrameObject *, const _Py_CODEUNIT *);
/*static*/ PyObject * special_lookup(PyThreadState *, PyObject *, _Py_Identifier *);
/*static*/ int check_args_iterable(PyThreadState *, PyObject *func, PyObject *vararg);
/*static*/ void format_kwargs_error(PyThreadState *, PyObject *func, PyObject *kwargs);
/*static*/ void format_awaitable_error(PyThreadState *, PyTypeObject *, int);

#define NAME_ERROR_MSG \
    "name '%.200s' is not defined"
#define UNBOUNDLOCAL_ERROR_MSG \
    "local variable '%.200s' referenced before assignment"
#define UNBOUNDFREE_ERROR_MSG \
    "free variable '%.200s' referenced before assignment" \
    " in enclosing scope"

/* Dynamic execution profile */
#ifdef DYNAMIC_EXECUTION_PROFILE
#ifdef DXPAIRS
static long dxpairs[257][256];
#define dxp dxpairs[256]
#else
static long dxp[256];
#endif
#endif

/* per opcode cache */
// Pyston change: use the opcache and jit even in debug mode
//#ifdef Py_DEBUG
//// --with-pydebug is used to find memory leak.  opcache makes it harder.
//// So we disable opcache when Py_DEBUG is defined.
//// See bpo-37146
//#define OPCACHE_MIN_RUNS 0  /* disable opcache */
//#else
#define OPCACHE_INC_FUNC_ENTRY 10
#define OPCACHE_MIN_RUNS (100*OPCACHE_INC_FUNC_ENTRY)  /* create opcache when code executed this time */
#define JIT_MIN_RUNS (OPCACHE_MIN_RUNS*2)
//#endif
#define OPCACHE_STATS 0  /* Enable stats */

#define USE_LOAD_METHOD_CACHE 1
#define USE_LOAD_ATTR_CACHE 1
#define USE_STORE_ATTR_CACHE 1

#if OPCACHE_STATS
static size_t opcache_code_objects = 0;
static size_t opcache_code_objects_extra_mem = 0;

static size_t opcache_global_opts = 0;
static size_t opcache_global_hits = 0;
static size_t opcache_global_misses = 0;

static long loadattr_hits = 0, loadattr_misses = 0, loadattr_uncached = 0, loadattr_noopcache = 0;
static long storeattr_hits = 0, storeattr_misses = 0, storeattr_uncached = 0, storeattr_noopcache = 0;
static long loadmethod_hits = 0, loadmethod_misses = 0, loadmethod_uncached = 0, loadmethod_noopcache = 0;
static long loadglobal_hits = 0, loadglobal_misses = 0, loadglobal_uncached = 0, loadglobal_noopcache = 0;
#endif

#define GIL_REQUEST _Py_atomic_load_relaxed(&ceval->gil_drop_request)

/* This can set eval_breaker to 0 even though gil_drop_request became
   1.  We believe this is all right because the eval loop will release
   the GIL eventually anyway. */
#define COMPUTE_EVAL_BREAKER(ceval) \
    _Py_atomic_store_relaxed( \
        &(ceval)->eval_breaker, \
        GIL_REQUEST | \
        _Py_atomic_load_relaxed(&(ceval)->signals_pending) | \
        _Py_atomic_load_relaxed(&(ceval)->pending.calls_to_do) | \
        (ceval)->pending.async_exc)

#define SET_GIL_DROP_REQUEST(ceval) \
    do { \
        _Py_atomic_store_relaxed(&(ceval)->gil_drop_request, 1); \
        _Py_atomic_store_relaxed(&(ceval)->eval_breaker, 1); \
    } while (0)

#define RESET_GIL_DROP_REQUEST(ceval) \
    do { \
        _Py_atomic_store_relaxed(&(ceval)->gil_drop_request, 0); \
        COMPUTE_EVAL_BREAKER(ceval); \
    } while (0)

/* Pending calls are only modified under pending_lock */
#define SIGNAL_PENDING_CALLS(ceval) \
    do { \
        _Py_atomic_store_relaxed(&(ceval)->pending.calls_to_do, 1); \
        _Py_atomic_store_relaxed(&(ceval)->eval_breaker, 1); \
    } while (0)

#define UNSIGNAL_PENDING_CALLS(ceval) \
    do { \
        _Py_atomic_store_relaxed(&(ceval)->pending.calls_to_do, 0); \
        COMPUTE_EVAL_BREAKER(ceval); \
    } while (0)

#define SIGNAL_PENDING_SIGNALS(ceval) \
    do { \
        _Py_atomic_store_relaxed(&(ceval)->signals_pending, 1); \
        _Py_atomic_store_relaxed(&(ceval)->eval_breaker, 1); \
    } while (0)

#define UNSIGNAL_PENDING_SIGNALS(ceval) \
    do { \
        _Py_atomic_store_relaxed(&(ceval)->signals_pending, 0); \
        COMPUTE_EVAL_BREAKER(ceval); \
    } while (0)

#define SIGNAL_ASYNC_EXC(ceval) \
    do { \
        (ceval)->pending.async_exc = 1; \
        _Py_atomic_store_relaxed(&(ceval)->eval_breaker, 1); \
    } while (0)

#define UNSIGNAL_ASYNC_EXC(ceval) \
    do { \
        (ceval)->pending.async_exc = 0; \
        COMPUTE_EVAL_BREAKER(ceval); \
    } while (0)


#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#include "pythread.h"
#include "ceval_gil.h"

#if 0
int
PyEval_ThreadsInitialized(void)
{
    return gil_created(&_PyRuntime.ceval.gil);
}

void
PyEval_InitThreads(void)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    struct _ceval_runtime_state *ceval = &runtime->ceval;
    struct _gil_runtime_state *gil = &ceval->gil;
    if (gil_created(gil)) {
        return;
    }

    PyThread_init_thread();
    create_gil(gil);
    PyThreadState *tstate = _PyRuntimeState_GetThreadState(runtime);
    take_gil(ceval, tstate);

    struct _pending_calls *pending = &ceval->pending;
    pending->lock = PyThread_allocate_lock();
    if (pending->lock == NULL) {
        Py_FatalError("Can't initialize threads for pending calls");
    }
}

void
_PyEval_FiniThreads(struct _ceval_runtime_state *ceval)
{
    struct _gil_runtime_state *gil = &ceval->gil;
    if (!gil_created(gil)) {
        return;
    }

    destroy_gil(gil);
    assert(!gil_created(gil));

    struct _pending_calls *pending = &ceval->pending;
    if (pending->lock != NULL) {
        PyThread_free_lock(pending->lock);
        pending->lock = NULL;
    }
}
#endif
static inline void
exit_thread_if_finalizing(_PyRuntimeState *runtime, PyThreadState *tstate)
{
    /* _Py_Finalizing is protected by the GIL */
    if (runtime->finalizing != NULL && !_Py_CURRENTLY_FINALIZING(runtime, tstate)) {
        drop_gil(&runtime->ceval, tstate);
        PyThread_exit_thread();
    }
}
#if 0
void
_PyEval_Fini(void)
{
#if OPCACHE_STATS
    fprintf(stderr, "-- Opcode cache number of objects  = %zd\n",
            opcache_code_objects);

    fprintf(stderr, "-- Opcode cache total extra mem    = %zd\n",
            opcache_code_objects_extra_mem);

    fprintf(stderr, "\n");

    fprintf(stderr, "-- Opcode cache LOAD_GLOBAL hits   = %zd (%d%%)\n",
            opcache_global_hits,
            (int) (100.0 * opcache_global_hits /
                (opcache_global_hits + opcache_global_misses)));

    fprintf(stderr, "-- Opcode cache LOAD_GLOBAL misses = %zd (%d%%)\n",
            opcache_global_misses,
            (int) (100.0 * opcache_global_misses /
                (opcache_global_hits + opcache_global_misses)));

    fprintf(stderr, "-- Opcode cache LOAD_GLOBAL opts   = %zd\n",
            opcache_global_opts);

    fprintf(stderr, "\n");
#endif
}

void
PyEval_AcquireLock(void)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    struct _ceval_runtime_state *ceval = &runtime->ceval;
    PyThreadState *tstate = _PyRuntimeState_GetThreadState(runtime);
    if (tstate == NULL) {
        Py_FatalError("PyEval_AcquireLock: current thread state is NULL");
    }
    take_gil(ceval, tstate);
    exit_thread_if_finalizing(runtime, tstate);
}

void
PyEval_ReleaseLock(void)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *tstate = _PyRuntimeState_GetThreadState(runtime);
    /* This function must succeed when the current thread state is NULL.
       We therefore avoid PyThreadState_Get() which dumps a fatal error
       in debug mode.
    */
    drop_gil(&runtime->ceval, tstate);
}

void
PyEval_AcquireThread(PyThreadState *tstate)
{
    if (tstate == NULL) {
        Py_FatalError("PyEval_AcquireThread: NULL new thread state");
    }

    _PyRuntimeState *runtime = &_PyRuntime;
    struct _ceval_runtime_state *ceval = &runtime->ceval;

    /* Check someone has called PyEval_InitThreads() to create the lock */
    assert(gil_created(&ceval->gil));
    take_gil(ceval, tstate);
    exit_thread_if_finalizing(runtime, tstate);
    if (_PyThreadState_Swap(&runtime->gilstate, tstate) != NULL) {
        Py_FatalError("PyEval_AcquireThread: non-NULL old thread state");
    }
}

void
PyEval_ReleaseThread(PyThreadState *tstate)
{
    if (tstate == NULL) {
        Py_FatalError("PyEval_ReleaseThread: NULL thread state");
    }

    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *new_tstate = _PyThreadState_Swap(&runtime->gilstate, NULL);
    if (new_tstate != tstate) {
        Py_FatalError("PyEval_ReleaseThread: wrong thread state");
    }
    drop_gil(&runtime->ceval, tstate);
}

/* This function is called from PyOS_AfterFork_Child to destroy all threads
 * which are not running in the child process, and clear internal locks
 * which might be held by those threads.
 */

void
_PyEval_ReInitThreads(_PyRuntimeState *runtime)
{
    struct _ceval_runtime_state *ceval = &runtime->ceval;
    if (!gil_created(&ceval->gil)) {
        return;
    }
    recreate_gil(&ceval->gil);
    PyThreadState *current_tstate = _PyRuntimeState_GetThreadState(runtime);
    take_gil(ceval, current_tstate);

    struct _pending_calls *pending = &ceval->pending;
    pending->lock = PyThread_allocate_lock();
    if (pending->lock == NULL) {
        Py_FatalError("Can't initialize threads for pending calls");
    }

    /* Destroy all threads except the current one */
    _PyThreadState_DeleteExcept(runtime, current_tstate);
}

/* This function is used to signal that async exceptions are waiting to be
   raised. */

void
_PyEval_SignalAsyncExc(struct _ceval_runtime_state *ceval)
{
    SIGNAL_ASYNC_EXC(ceval);
}

PyThreadState *
PyEval_SaveThread(void)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    struct _ceval_runtime_state *ceval = &runtime->ceval;
    PyThreadState *tstate = _PyThreadState_Swap(&runtime->gilstate, NULL);
    if (tstate == NULL) {
        Py_FatalError("PyEval_SaveThread: NULL tstate");
    }
    assert(gil_created(&ceval->gil));
    drop_gil(ceval, tstate);
    return tstate;
}

void
PyEval_RestoreThread(PyThreadState *tstate)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    struct _ceval_runtime_state *ceval = &runtime->ceval;

    if (tstate == NULL) {
        Py_FatalError("PyEval_RestoreThread: NULL tstate");
    }
    assert(gil_created(&ceval->gil));

    int err = errno;
    take_gil(ceval, tstate);
    exit_thread_if_finalizing(runtime, tstate);
    errno = err;

    _PyThreadState_Swap(&runtime->gilstate, tstate);
}


/* Mechanism whereby asynchronously executing callbacks (e.g. UNIX
   signal handlers or Mac I/O completion routines) can schedule calls
   to a function to be called synchronously.
   The synchronous function is called with one void* argument.
   It should return 0 for success or -1 for failure -- failure should
   be accompanied by an exception.

   If registry succeeds, the registry function returns 0; if it fails
   (e.g. due to too many pending calls) it returns -1 (without setting
   an exception condition).

   Note that because registry may occur from within signal handlers,
   or other asynchronous events, calling malloc() is unsafe!

   Any thread can schedule pending calls, but only the main thread
   will execute them.
   There is no facility to schedule calls to a particular thread, but
   that should be easy to change, should that ever be required.  In
   that case, the static variables here should go into the python
   threadstate.
*/

void
_PyEval_SignalReceived(struct _ceval_runtime_state *ceval)
{
    /* bpo-30703: Function called when the C signal handler of Python gets a
       signal. We cannot queue a callback using Py_AddPendingCall() since
       that function is not async-signal-safe. */
    SIGNAL_PENDING_SIGNALS(ceval);
}
#endif
/* Push one item onto the queue while holding the lock. */
static int
_push_pending_call(struct _pending_calls *pending,
                   int (*func)(void *), void *arg)
{
    int i = pending->last;
    int j = (i + 1) % NPENDINGCALLS;
    if (j == pending->first) {
        return -1; /* Queue full */
    }
    pending->calls[i].func = func;
    pending->calls[i].arg = arg;
    pending->last = j;
    return 0;
}

/* Pop one item off the queue while holding the lock. */
static void
_pop_pending_call(struct _pending_calls *pending,
                  int (**func)(void *), void **arg)
{
    int i = pending->first;
    if (i == pending->last) {
        return; /* Queue empty */
    }

    *func = pending->calls[i].func;
    *arg = pending->calls[i].arg;
    pending->first = (i + 1) % NPENDINGCALLS;
}

/* This implementation is thread-safe.  It allows
   scheduling to be made from any thread, and even from an executing
   callback.
 */
#if 0
int
_PyEval_AddPendingCall(PyThreadState *tstate,
                       struct _ceval_runtime_state *ceval,
                       int (*func)(void *), void *arg)
{
    struct _pending_calls *pending = &ceval->pending;

    PyThread_acquire_lock(pending->lock, WAIT_LOCK);
    if (pending->finishing) {
        PyThread_release_lock(pending->lock);

        PyObject *exc, *val, *tb;
        _PyErr_Fetch(tstate, &exc, &val, &tb);
        _PyErr_SetString(tstate, PyExc_SystemError,
                        "Py_AddPendingCall: cannot add pending calls "
                        "(Python shutting down)");
        _PyErr_Print(tstate);
        _PyErr_Restore(tstate, exc, val, tb);
        return -1;
    }
    int result = _push_pending_call(pending, func, arg);
    PyThread_release_lock(pending->lock);

    /* signal main loop */
    SIGNAL_PENDING_CALLS(ceval);
    return result;
}

int
Py_AddPendingCall(int (*func)(void *), void *arg)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *tstate = _PyRuntimeState_GetThreadState(runtime);
    return _PyEval_AddPendingCall(tstate, &runtime->ceval, func, arg);
}
#endif
static int
handle_signals(_PyRuntimeState *runtime)
{
    /* Only handle signals on main thread.  PyEval_InitThreads must
     * have been called already.
     */
    if (PyThread_get_thread_ident() != runtime->main_thread) {
        return 0;
    }
    /*
     * Ensure that the thread isn't currently running some other
     * interpreter.
     */
    PyInterpreterState *interp = _PyRuntimeState_GetThreadState(runtime)->interp;
    if (interp != runtime->interpreters.main) {
        return 0;
    }

    struct _ceval_runtime_state *ceval = &runtime->ceval;
    UNSIGNAL_PENDING_SIGNALS(ceval);
    if (_PyErr_CheckSignals() < 0) {
        SIGNAL_PENDING_SIGNALS(ceval); /* We're not done yet */
        return -1;
    }
    return 0;
}

static int
make_pending_calls(_PyRuntimeState *runtime)
{
    static int busy = 0;

    /* only service pending calls on main thread */
    if (PyThread_get_thread_ident() != runtime->main_thread) {
        return 0;
    }

    /* don't perform recursive pending calls */
    if (busy) {
        return 0;
    }
    busy = 1;
    struct _ceval_runtime_state *ceval = &runtime->ceval;
    /* unsignal before starting to call callbacks, so that any callback
       added in-between re-signals */
    UNSIGNAL_PENDING_CALLS(ceval);
    int res = 0;

    /* perform a bounded number of calls, in case of recursion */
    struct _pending_calls *pending = &ceval->pending;
    for (int i=0; i<NPENDINGCALLS; i++) {
        int (*func)(void *) = NULL;
        void *arg = NULL;

        /* pop one item off the queue while holding the lock */
        PyThread_acquire_lock(pending->lock, WAIT_LOCK);
        _pop_pending_call(pending, &func, &arg);
        PyThread_release_lock(pending->lock);

        /* having released the lock, perform the callback */
        if (func == NULL) {
            break;
        }
        res = func(arg);
        if (res) {
            goto error;
        }
    }

    busy = 0;
    return res;

error:
    busy = 0;
    SIGNAL_PENDING_CALLS(ceval);
    return res;
}
#if 0
void
_Py_FinishPendingCalls(_PyRuntimeState *runtime)
{
    assert(PyGILState_Check());

    PyThreadState *tstate = _PyRuntimeState_GetThreadState(runtime);
    struct _pending_calls *pending = &runtime->ceval.pending;

    PyThread_acquire_lock(pending->lock, WAIT_LOCK);
    pending->finishing = 1;
    PyThread_release_lock(pending->lock);

    if (!_Py_atomic_load_relaxed(&(pending->calls_to_do))) {
        return;
    }

    if (make_pending_calls(runtime) < 0) {
        PyObject *exc, *val, *tb;
        _PyErr_Fetch(tstate, &exc, &val, &tb);
        PyErr_BadInternalCall();
        _PyErr_ChainExceptions(exc, val, tb);
        _PyErr_Print(tstate);
    }
}

/* Py_MakePendingCalls() is a simple wrapper for the sake
   of backward-compatibility. */
int
Py_MakePendingCalls(void)
{
    assert(PyGILState_Check());

    /* Python signal handler doesn't really queue a callback: it only signals
       that a signal was received, see _PyEval_SignalReceived(). */
    _PyRuntimeState *runtime = &_PyRuntime;
    int res = handle_signals(runtime);
    if (res != 0) {
        return res;
    }

    res = make_pending_calls(runtime);
    if (res != 0) {
        return res;
    }

    return 0;
}

/* The interpreter's recursion limit */

#ifndef Py_DEFAULT_RECURSION_LIMIT
#define Py_DEFAULT_RECURSION_LIMIT 1000
#endif

int _Py_CheckRecursionLimit = Py_DEFAULT_RECURSION_LIMIT;

void
_PyEval_Initialize(struct _ceval_runtime_state *state)
{
    state->recursion_limit = Py_DEFAULT_RECURSION_LIMIT;
    _Py_CheckRecursionLimit = Py_DEFAULT_RECURSION_LIMIT;
    _gil_initialize(&state->gil);
}

int
Py_GetRecursionLimit(void)
{
    return _PyRuntime.ceval.recursion_limit;
}

void
Py_SetRecursionLimit(int new_limit)
{
    struct _ceval_runtime_state *ceval = &_PyRuntime.ceval;
    ceval->recursion_limit = new_limit;
    _Py_CheckRecursionLimit = ceval->recursion_limit;
}

/* the macro Py_EnterRecursiveCall() only calls _Py_CheckRecursiveCall()
   if the recursion_depth reaches _Py_CheckRecursionLimit.
   If USE_STACKCHECK, the macro decrements _Py_CheckRecursionLimit
   to guarantee that _Py_CheckRecursiveCall() is regularly called.
   Without USE_STACKCHECK, there is no need for this. */
int
_Py_CheckRecursiveCall(const char *where)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *tstate = _PyRuntimeState_GetThreadState(runtime);
    int recursion_limit = runtime->ceval.recursion_limit;

#ifdef USE_STACKCHECK
    tstate->stackcheck_counter = 0;
    if (PyOS_CheckStack()) {
        --tstate->recursion_depth;
        _PyErr_SetString(tstate, PyExc_MemoryError, "Stack overflow");
        return -1;
    }
    /* Needed for ABI backwards-compatibility (see bpo-31857) */
    _Py_CheckRecursionLimit = recursion_limit;
#endif
    if (tstate->recursion_critical)
        /* Somebody asked that we don't check for recursion. */
        return 0;
    if (tstate->overflowed) {
        if (tstate->recursion_depth > recursion_limit + 50) {
            /* Overflowing while handling an overflow. Give up. */
            Py_FatalError("Cannot recover from stack overflow.");
        }
        return 0;
    }
    if (tstate->recursion_depth > recursion_limit) {
        --tstate->recursion_depth;
        tstate->overflowed = 1;
        _PyErr_Format(tstate, PyExc_RecursionError,
                      "maximum recursion depth exceeded%s",
                      where);
        return -1;
    }
    return 0;
}
#endif
/*static*/ int do_raise(PyThreadState *tstate, PyObject *exc, PyObject *cause);
/*static*/ int unpack_iterable(PyThreadState *, PyObject *, int, int, PyObject **);

#define _Py_TracingPossible(ceval) ((ceval)->tracing_possible)

#if 0
PyObject *
PyEval_EvalCode(PyObject *co, PyObject *globals, PyObject *locals)
{
    return PyEval_EvalCodeEx(co,
                      globals, locals,
                      (PyObject **)NULL, 0,
                      (PyObject **)NULL, 0,
                      (PyObject **)NULL, 0,
                      NULL, NULL);
}


/* Interpreter main loop */

PyObject *
PyEval_EvalFrame(PyFrameObject *f) {
    /* This is for backward compatibility with extension modules that
       used this API; core interpreter code should call
       PyEval_EvalFrameEx() */
    return PyEval_EvalFrameEx(f, 0);
}

PyObject *
PyEval_EvalFrameEx(PyFrameObject *f, int throwflag)
{
    PyInterpreterState *interp = _PyInterpreterState_GET_UNSAFE();
    return interp->eval_frame(f, throwflag);
}
#endif

// this function is used by the JIT but I moved it here because it uses a bunch of internal stuff
// which we would need to copy or make public if we want to use it in another translation unit
int eval_breaker_jit_helper() {
    _PyRuntimeState * const runtime = &_PyRuntime;
    PyThreadState * const tstate = _PyRuntimeState_GetThreadState(runtime);
    struct _ceval_runtime_state * const ceval = &runtime->ceval;

    if (_Py_atomic_load_relaxed(&ceval->signals_pending)) {
        if (handle_signals(runtime) != 0) {
            return -1;
        }
    }
    if (_Py_atomic_load_relaxed(&ceval->pending.calls_to_do)) {
        if (make_pending_calls(runtime) != 0) {
            return -1;
        }
    }

    if (_Py_atomic_load_relaxed(&ceval->gil_drop_request)) {
        /* Give another thread a chance */
        if (_PyThreadState_Swap(&runtime->gilstate, NULL) != tstate) {
            Py_FatalError("ceval: tstate mix-up");
        }
        drop_gil(ceval, tstate);

        /* Other threads may run now */

        take_gil(ceval, tstate);

        /* Check if we should make a quick exit. */
        exit_thread_if_finalizing(runtime, tstate);

        if (_PyThreadState_Swap(&runtime->gilstate, tstate) != NULL) {
            Py_FatalError("ceval: orphan tstate");
        }
    }
    /* Check for asynchronous exceptions. */
    if (tstate->async_exc != NULL) {
        PyObject *exc = tstate->async_exc;
        tstate->async_exc = NULL;
        UNSIGNAL_ASYNC_EXC(ceval);
        _PyErr_SetNone(tstate, exc);
        Py_DECREF(exc);
        return -1;
    }
    return 0;
}

static uint64_t getDictVersionFromDictPtr(PyObject** dictptr) {
    if (dictptr == NULL)
        return 0;

    PyDictObject* dict = (PyDictObject*)*dictptr;
    if (dict == NULL)
        return 0;

    return dict->ma_version_tag;
}

static uint64_t getSplitDictKeysVersionFromDictPtr(PyObject** dictptr) {
    if (dictptr == NULL)
        return 0;

    PyDictObject* dict = (PyDictObject*)*dictptr;
    if (dict == NULL)
        return 0;

    // return 0 if this is not a split dict
    if (dict->ma_values == NULL)
        return 0;

    return _PyDict_GetDictKeyVersionFromSplitDict((PyObject*)dict);
}

int setItemSplitDictCache(PyObject* dict, Py_ssize_t splitdict_index, PyObject* v, PyObject* name) {
    int err = _PyDict_SetItemFromSplitDict(dict, name, splitdict_index, v);
    if (err < 0 && PyErr_ExceptionMatches(PyExc_KeyError)) {
        PyErr_SetObject(PyExc_AttributeError, name);
    }
    return err;
}
int setItemInitSplitDictCache(PyObject** dictptr, PyObject* obj, PyObject* v, Py_ssize_t splitdict_index,PyObject* name) {
    int err = _PyDict_SetItemInitialFromSplitDict(Py_TYPE(obj), dictptr, name, splitdict_index, v);
    if (err < 0 && PyErr_ExceptionMatches(PyExc_KeyError)) {
        PyErr_SetObject(PyExc_AttributeError, name);
    }
    return err;
}

int __attribute__((always_inline)) __attribute__((visibility("hidden")))
storeAttrCache(PyObject* owner, PyObject* name, PyObject* v, _PyOpcache *co_opcache, int* err) {
    _PyOpcache_StoreAttr *sa = &co_opcache->u.sa;
    PyTypeObject *tp = Py_TYPE(owner);

    // do we have a valid cache entry?
    if (!co_opcache->optimized)
        return -1;

    if (unlikely(sa->type_ver != tp->tp_version_tag))
        return -1;

    if (sa->cache_type == SA_CACHE_SLOT_CACHE) {
        char* addr = (char*)owner + sa->u.slot_cache.offset;
        PyObject** slot = (PyObject**)addr;
        PyObject* old = *slot;
        /* we don't support del attr yet so v can't be null
        if (v == NULL && old == NULL) {
            PyErr_SetString(PyExc_AttributeError, name);
            *err = -1;
            goto hit;
        }
        Py_XINCREF(v);
        */
        Py_INCREF(v);
        *slot = v;
        Py_XDECREF(old);

        *err = 0;
        goto hit;
    }

    PyObject** dictptr = _PyObject_GetDictPtr(owner);
    if (dictptr && !*dictptr) {
        // Check if this is the first attribute on the object.
        // There won't be any split keys object yet, but it will
        // use the cached version from the type object, so check
        // against that.
        if (!(tp->tp_flags & Py_TPFLAGS_HEAPTYPE))
            return -1;
        PyDictKeysObject* keys = ((PyHeapTypeObject*)tp)->ht_cached_keys;
        if (!keys)
            return -1;
        if (_PyDict_GetDictKeyVersionFromKeys((PyObject*)keys) != sa->u.split_dict_cache.splitdict_keys_version)
            return -1;

        *err = setItemInitSplitDictCache(dictptr, owner, v, sa->u.split_dict_cache.splitdict_index, name);

        // mark that we hit the dict not initalized path
        sa->cache_type = SA_CACHE_IDX_SPLIT_DICT_INIT;
    } else {
        // check if this dict has the same keys as the cached one
        if (sa->u.split_dict_cache.splitdict_keys_version != getSplitDictKeysVersionFromDictPtr(dictptr))
            return -1;

        *err = setItemSplitDictCache(*dictptr, sa->u.split_dict_cache.splitdict_index, v, name);

        // mark that we hit the dict already initalized path
        sa->cache_type = SA_CACHE_IDX_SPLIT_DICT;
    }

hit:
    co_opcache->num_failed = 0;
#if OPCACHE_STATS
    storeattr_hits++;
#endif
    return 0;
}

int __attribute__((always_inline)) __attribute__((visibility("hidden")))
setupStoreAttrCache(PyObject* obj, PyObject* name, _PyOpcache *co_opcache) {
    _PyOpcache_StoreAttr *sa = &co_opcache->u.sa;
    PyTypeObject *tp = Py_TYPE(obj);

    if (co_opcache->num_failed >= 3)
        return -1;

    if (!PyType_HasFeature(tp, Py_TPFLAGS_VALID_VERSION_TAG))
        return -1;

    if (tp->tp_setattro != PyObject_GenericSetAttr)
        return -1;

    if (tp->tp_dict == NULL && PyType_Ready(tp) < 0)
        return -1;

    PyObject* descr = _PyType_Lookup(tp, name);
    if (descr != NULL) {
        PyTypeObject *dtype = Py_TYPE(descr);
        if (dtype == &PyMemberDescr_Type) {  // It's a slot
            PyMemberDescrObject *member = (PyMemberDescrObject*)descr;
            struct PyMemberDef *dmem = member->d_member;
            if (dmem->type == T_OBJECT_EX) {
                sa->cache_type = SA_CACHE_SLOT_CACHE;
                sa->u.slot_cache.offset = dmem->offset;
                goto common_cached;
            }
        }
        return -1;
    }

    PyObject** dictptr = _PyObject_GetDictPtr(obj);
    PyObject* dict;
    if (dictptr == NULL || (dict = *dictptr) == NULL)
        return -1;

    if (!_PyDict_HasSplitTable((PyDictObject*)dict))
        return -1;

    sa->cache_type = SA_CACHE_IDX_SPLIT_DICT;
    sa->u.split_dict_cache.splitdict_keys_version = getSplitDictKeysVersionFromDictPtr(dictptr);
    sa->u.split_dict_cache.splitdict_index = _PyDict_GetItemIndexSplitDict(dict, name);
    // the index must be >= 0 because otherwise _PyObject_SetAttrCanCache would return 0
    assert(sa->u.split_dict_cache.splitdict_index >= 0);

common_cached:
    if (tp->tp_dictoffset < SHRT_MIN || tp->tp_dictoffset > SHRT_MAX) {
        co_opcache->optimized = 0; // we already modified the cache entry
        return -1; // can't fit in our cache
    }
    co_opcache->optimized = 1;
    sa->type_tp_dictoffset = (short)tp->tp_dictoffset;
    sa->type_ver = tp->tp_version_tag;
    return 0;
}

PyObject* slot_tp_getattr_hook_simple(PyObject *self, PyObject *name);
PyObject* slot_tp_getattr_hook_simple_not_found(PyObject *self, PyObject *name);
PyObject* module_getattro(PyObject *_m, PyObject *name);
PyObject* module_getattro_not_found(PyObject *_m, PyObject *name);

PyObject* loadAttrCacheAttrNotFound(PyObject *owner, PyObject *name) {
    void* tp_getattro = Py_TYPE(owner)->tp_getattro;
    if (tp_getattro == PyObject_GenericGetAttr) {
        if (!PyErr_Occurred())
            PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%U'", Py_TYPE(owner)->tp_name, name);
    } else if (tp_getattro == slot_tp_getattr_hook_simple) {
        return slot_tp_getattr_hook_simple_not_found(owner, name);
    } else if (tp_getattro == module_getattro) {
        return module_getattro_not_found(owner, name);
    } else {
        printf("loadAttrCacheAttrNotFound error this should never happen: %p\n", tp_getattro);
        abort();
    }
    return NULL;
}

int64_t _PyDict_GetItemOffset(PyDictObject *mp, PyObject *key, Py_ssize_t *dk_size);
PyObject* _PyDict_GetItemByOffset(PyDictObject *mp, PyObject *key, Py_ssize_t dk_size, int64_t offset);

int __attribute__((visibility("hidden")))
loadAttrCache(PyObject* owner, PyObject* name, _PyOpcache *co_opcache, PyObject** res, int *meth_found) {
    _PyOpcache_LoadAttr *la = &co_opcache->u.la;

    // do we have a valid cache entry?
    if (!co_opcache->optimized)
        return -1;

    if (la->cache_type == LA_CACHE_POLYMORPHIC) {
        // give up if we have not had a single cache hit after that many tries
        if (co_opcache->num_failed >= 15) {
            return -1;
        }
        for (int i=0, num=la->u.poly_cache.num_used; i<num; ++i) {
            _PyOpcache* caches = la->u.poly_cache.caches;
            if (loadAttrCache(owner, name, &caches[i], res, meth_found) == 0) {
                co_opcache->num_failed = 0;
                return 0;
            }
        }
        return -1;
    }

    if (la->cache_type == LA_CACHE_BUILTIN) {
        if (unlikely(Py_TYPE(owner) != la->type))
            return -1;
    } else {
        if (unlikely(Py_TYPE(owner)->tp_version_tag != la->type_ver))
            return -1;
    }

    PyObject** dictptr = _PyObject_GetDictPtr(owner);
    if (meth_found)
        *meth_found = la->meth_found;

    if (la->cache_type == LA_CACHE_OFFSET_CACHE)
    {
        if (!dictptr || !*dictptr)
            return -1;

        *res = _PyDict_GetItemByOffset((PyDictObject*)*dictptr, name, la->u.offset_cache.dk_size, la->u.offset_cache.offset);

        if (*res == NULL)
            return -1;

        Py_INCREF(*res);
    } else if (la->cache_type == LA_CACHE_SLOT_CACHE) {
        char* addr = (char*)owner + la->u.slot_cache.offset;
        *res = *(PyObject**)addr;
        if (*res == NULL)
            return -1;
        Py_INCREF(*res);
    } else if (la->cache_type == LA_CACHE_VALUE_CACHE_DICT || la->cache_type == LA_CACHE_VALUE_CACHE_SPLIT_DICT || la->cache_type == LA_CACHE_BUILTIN)
    {
        if (la->cache_type == LA_CACHE_VALUE_CACHE_SPLIT_DICT) {
            if (la->u.value_cache.dict_ver != getSplitDictKeysVersionFromDictPtr(dictptr))
                return -1;
        } else if (la->cache_type == LA_CACHE_VALUE_CACHE_DICT) {
            if (la->u.value_cache.dict_ver != getDictVersionFromDictPtr(dictptr))
                return -1;
        } else {
            if (la->type != Py_TYPE(owner))
                return -1;
        }

        if (la->guard_tp_descr_get && Py_TYPE(la->u.value_cache.obj)->tp_descr_get != NULL)
            return -1;

        *res = la->u.value_cache.obj;
        assert(*res); // must be set because otherwise we would not have cached the value
        Py_INCREF(*res);
    } else if (la->cache_type == LA_CACHE_IDX_SPLIT_DICT) {
        // check if this dict has the same keys as the cached one
        if (la->u.split_dict_cache.splitdict_keys_version != getSplitDictKeysVersionFromDictPtr(dictptr))
            return -1;

        *res = _PyDict_GetItemFromSplitDict(*dictptr, la->u.split_dict_cache.splitdict_index);

        // can be null, call into tp_getattro specific handler
        if (*res == NULL)
            *res = loadAttrCacheAttrNotFound(owner, name);
        else
            Py_INCREF(*res);
    } else {
        PyObject* descr = la->u.descr_cache.descr;
        if (unlikely(Py_TYPE(descr)->tp_version_tag != la->u.descr_cache.descr_type_ver))
            return -1;

        *res = descr->ob_type->tp_descr_get(descr, owner, (PyObject *)owner->ob_type);

        // can be null, call into tp_getattro specific handler
        if (*res == NULL)
            *res = loadAttrCacheAttrNotFound(owner, name);
    }

    co_opcache->num_failed = 0;

#if OPCACHE_STATS
    if (meth_found)
        loadmethod_hits++;
    else
        loadattr_hits++;
#endif
    return 0;
}

static int createPolymorphicCache(_PyOpcache* co_opcache, _PyOpcache_LoadAttr *la) {
    int num_entries = 5;
    _PyOpcache* caches = PyMem_Calloc(num_entries, sizeof(_PyOpcache));
    if (!caches)
        return -1;
    // pretend the cache entries failed already a few times because
    // we prefer emitting less specific caches like LA_CACHE_OFFSET_CACHE when
    // we are already creating several entries.
    for (int i = 1; i<num_entries; ++i) {
        caches[i].num_failed = 2;
    }
    // copy over the old entry as first entry of the polymorphic cache
    memcpy(&caches[0], co_opcache, sizeof(_PyOpcache));
    la->cache_type = LA_CACHE_POLYMORPHIC;
    la->u.poly_cache.caches = caches;
    la->u.poly_cache.num_entries = num_entries;
    la->u.poly_cache.num_used = 1;
    return 0;
}

int __attribute__((visibility("hidden")))
setupLoadAttrCache(PyObject* obj, PyObject* name, _PyOpcache *co_opcache, PyObject* res, int is_load_method, int inside_interpreter) {
    _PyOpcache_LoadAttr *la = &co_opcache->u.la;
    int meth_found = 0;
    PyObject* descr = NULL;
    PyObject **dictptr = NULL, *dict = NULL;

    PyTypeObject *tp = Py_TYPE(obj);

    if (!res)
        return -1;

    if (co_opcache->num_failed >= 5)
        return -1;

    if (!PyType_HasFeature(tp, Py_TPFLAGS_VALID_VERSION_TAG))
        return -1;

    void* tp_getattro = tp->tp_getattro;
    if (tp_getattro != PyObject_GenericGetAttr) {
        // We only cache the attribute if we find it via the PyObject_GenericGetAttr mechanism which means
        // we also support module_getattro and slot_tp_getattr_hook_simple because the lookup mechanism is only different when we can't find it. And we handle that part inside loadAttrCacheAttrNotFound
        // WARNING if you add support for a new method make sure to update loadAttrCacheAttrNotFound
        if (tp_getattro != module_getattro && tp_getattro != slot_tp_getattr_hook_simple)
            return -1;
    }

    if (tp->tp_dict == NULL && PyType_Ready(tp) < 0)
        return -1;

    // We use this hash to see if this cache is for a different type.
    // Because we don't want to create a polymorphic cache when a single type get modified a
    // a lot and therefore the type version changes a lot.
    // This hash will catch most of this cases.
    uint8_t tp_hash = (uint8_t)((uint64_t)(tp)>>4);

    // support for polymorphic caches
    if (co_opcache->optimized &&
        /* enter if we are already polymorphic */
        (la->cache_type == LA_CACHE_POLYMORPHIC ||
        /* or if the hash of the type is different we create a polymorphic IC */
        la->type_hash != tp_hash)) {
        int entry_idx = 0;
        // we already have a polymorphic IC
        if (la->cache_type == LA_CACHE_POLYMORPHIC) {
            // check if the last slot should be overwritten:
            // - this can happen when we create a new slow but filling it failed because
            //   it was not possible to cache the attribute.
            // - the type hash is the same
            _PyOpcache *co_opcache_prev_entry = &la->u.poly_cache.caches[la->u.poly_cache.num_used-1];
            if (!co_opcache_prev_entry->optimized || co_opcache_prev_entry->u.la.type_hash == tp_hash) {
                entry_idx = la->u.poly_cache.num_used - 1;
            } else {
                // add a new entry
                if (la->u.poly_cache.num_used >= la->u.poly_cache.num_entries) {
                    co_opcache->num_failed = 10; // don't use the cache anymore we used all slots
                    return -1;
                }
                entry_idx = la->u.poly_cache.num_used++;
            }
        } else {
            // don't create new poly caches from the JIT helper funcs
            if (!inside_interpreter) {
                return -1;
            }
            if (createPolymorphicCache(co_opcache, la) == -1)
                return -1;
            entry_idx = la->u.poly_cache.num_used++;
        }
        co_opcache = &la->u.poly_cache.caches[entry_idx];
        la = &co_opcache->u.la;
    }

    descr = _PyType_Lookup(tp, name);
    if (descr != NULL) {
        // it's important that this function behaviour is the same as _PyObject_GetMethod because we use it to actually
        // fetch the value and this function to check what behaviour the fatched value has regarding caching.
        // This means that if tp_getattro != PyObject_GenericGetAttr we have to do the normal load attribute lookup
        // because _PyObject_GetMethod is only handling PyObject_GenericGetAttr (for non PyObject_GenericGetAttr would
        // return meth_found = 0 but this function would return 1.
        // TODO: we could investigate expanding _PyObject_GetMethod to support additional tp_getattros
        if (is_load_method && tp_getattro == PyObject_GenericGetAttr &&
            PyType_HasFeature(Py_TYPE(descr), Py_TPFLAGS_METHOD_DESCRIPTOR)) {
            meth_found = 1;
        } else {
            PyTypeObject *dtype = Py_TYPE(descr);
            if (dtype == &PyMemberDescr_Type) {  // It's a slot
                PyMemberDescrObject *member = (PyMemberDescrObject*)descr;
                struct PyMemberDef *dmem = member->d_member;
                if (dmem->type == T_OBJECT_EX) {
                    la->cache_type = LA_CACHE_SLOT_CACHE;
                    la->u.slot_cache.offset = dmem->offset;
                    goto common_cached;
                }
            }

            if (dtype->tp_descr_get) {
                if (PyDescr_IsData(descr)) {
                    if (!PyType_HasFeature(dtype, Py_TPFLAGS_VALID_VERSION_TAG))
                        return -1;

                    // we can cache this
                    la->cache_type = LA_CACHE_DATA_DESCR;
                    la->guard_tp_descr_get = 0; // we don't guard on this because we guard on dtype->tp_version_tag
                    la->u.descr_cache.descr = descr;
                    la->u.descr_cache.descr_type_ver = dtype->tp_version_tag;

                    goto common_cached;
                }
                return -1; // can't cache right now
            }
        }
    }

    dictptr = _PyObject_GetDictPtr(obj);
    if (dictptr != NULL && (dict = *dictptr) != NULL) {
        Py_INCREF(dict);
        PyObject *attr = PyDict_GetItemWithError(dict, name);
        if (attr != NULL) {
            Py_DECREF(dict);

            // we can't cache when the attr also exists in the types dict because
            // we would need additional guards if tp_descr_get gets modified / call _PyType_Lookup
            if (descr)
                return -1;

            // if this is a split dict we will cache the index of it in the value array (=LA_CACHE_IDX_SPLIT_DICT)
            // else we directly cache the value of the dict entry and guard on the exact dict version (=LA_CACHE_VALUE_CACHE_DICT)
            // if LA_CACHE_VALUE_CACHE_DICT is frequently resulting in a cache miss we switch to caching the offset of
            // the hashtable entry which does not require us to guard on the exact dict version - but retrieval
            // is more expensive (=LA_CACHE_OFFSET_CACHE)
            if (_PyDict_HasSplitTable((PyDictObject*)dict)) {
                la->cache_type = LA_CACHE_IDX_SPLIT_DICT;
                la->u.split_dict_cache.splitdict_keys_version = getSplitDictKeysVersionFromDictPtr(dictptr);
                la->u.split_dict_cache.splitdict_index = _PyDict_GetItemIndexSplitDict(dict, name);
                // <0 means we did not find the attribute but this should never happen
                assert(la->u.split_dict_cache.splitdict_index >= 0);
            } else if (co_opcache->num_failed >= 2) {
                Py_ssize_t dk_size;
                int64_t offset = _PyDict_GetItemOffset((PyDictObject*)dict, name, &dk_size);
                if (offset < 0)
                    return -1;

                la->cache_type = LA_CACHE_OFFSET_CACHE;
                la->u.offset_cache.dk_size = dk_size;
                la->u.offset_cache.offset = offset;
            } else {
                la->cache_type = LA_CACHE_VALUE_CACHE_DICT;
                la->u.value_cache.obj = res;
                la->u.value_cache.dict_ver = getDictVersionFromDictPtr(dictptr);
            }

            // because we verfied that _PyType_Lookup(tp, name) == NULL, we don't need to emit this guard
            la->guard_tp_descr_get = 0;

            goto common_cached;
        }
        else {
            Py_DECREF(dict);
            // we only call into this function if the lookup succeeded so why should it fail now
            assert(!PyErr_Occurred());
        }
    }

    if (descr == NULL)
        return -1;

    // if we reach this path it means the attribute is coming from the type and not the instance dict

    // need to do a 'descr->ob_type->tp_descr_get == NULL' check except if meth_found is set
    // because we are not guarding on Py_TYPE(descr)->tp_version_tag
    la->guard_tp_descr_get = !meth_found;

    if (!PyType_HasFeature(tp, Py_TPFLAGS_HEAPTYPE) && !dictptr) {
        // Simple case: this is a static (immutable) type, so we don't have to guard on as much.
        la->cache_type = LA_CACHE_BUILTIN;
        la->type = tp;
    } else {
        // guard on the instance dict shape if the instance dict is a splitdict and does not contain the attribute name as key.
        // else we will create guard which will check for the exact dict version (=less generic)
        int is_split_dict = dict && _PyDict_HasSplitTable((PyDictObject*)dict);
        if (is_split_dict && _PyDict_GetItemIndexSplitDict(dict, name) == -1) {
            la->u.value_cache.dict_ver = getSplitDictKeysVersionFromDictPtr(dictptr);
            la->cache_type = LA_CACHE_VALUE_CACHE_SPLIT_DICT;
        } else {
            la->u.value_cache.dict_ver = getDictVersionFromDictPtr(dictptr);
            la->cache_type = LA_CACHE_VALUE_CACHE_DICT;
        }
    }
    la->u.value_cache.obj = res;

common_cached:
    if (tp->tp_dictoffset < SHRT_MIN || tp->tp_dictoffset > SHRT_MAX) {
        co_opcache->optimized = 0; // we already modified the cache entry
        return -1; // can't fit in our cache
    }
    co_opcache->optimized = 1;
    la->type_tp_dictoffset = (short)tp->tp_dictoffset;
    la->meth_found = meth_found;
    if (la->cache_type != LA_CACHE_BUILTIN)
        la->type_ver = tp->tp_version_tag;
    la->type_hash = tp_hash;
    return 0;
}

//#define PROFILE_OPCODES 1
#if PROFILE_OPCODES
PyObject* opcode_profile_dict;
int opcode_profile_enabled;
static PyObject* update_opcode_profile_func_enter(PyFrameObject* f) {
    if (!opcode_profile_enabled)
        return 0;
    char func_path[512];
    sprintf(func_path, "%s:%d:%s() has %ld opcodes", PyUnicode_AsUTF8(f->f_code->co_filename), f->f_code->co_firstlineno, PyUnicode_AsUTF8(f->f_code->co_name), PyBytes_Size(f->f_code->co_code)/2);

    if (!opcode_profile_dict)
        opcode_profile_dict = PyDict_New();

    PyObject* entry = PyDict_GetItemString(opcode_profile_dict, func_path);
    if (!entry) {
        entry = PyDict_New();
        PyDict_SetItemString(opcode_profile_dict, func_path, entry);
    }
    static PyObject* one = NULL;
    if (!one)
        one = PyLong_FromLong(1);
    const char* func_counter_name = "num_func_called";
    PyObject* func_counter = PyDict_GetItemString(entry, func_counter_name);
    if (!func_counter)
        PyDict_SetItemString(entry, func_counter_name, one);
    else {
        PyObject* value = PyNumber_Add(func_counter, one);
        PyDict_SetItemString(entry, func_counter_name, value);
        Py_DECREF(value);
    }
    return entry;
}

static void update_opcode_profile(PyObject* func_entry, const char* opcode) {
    if (!opcode_profile_enabled)
        return;
    static PyObject* one = NULL;
    if (!one)
        one = PyLong_FromLong(1);
    PyObject* opcode_counter = PyDict_GetItemString(func_entry, opcode);
    if (!opcode_counter)
        PyDict_SetItemString(func_entry, opcode, one);
    else {
        PyObject* value = PyNumber_Add(opcode_counter, one);
        PyDict_SetItemString(func_entry, opcode, value);
        Py_DECREF(value);
    }
}
#endif

typedef struct {
    unsigned long ret_val;
    PyObject** stack_pointer;
} JitRetVal;

typedef JitRetVal (*JitFunc)(PyFrameObject* frame, PyThreadState * const tstate, PyObject** sp);

JitFunc jit_func(PyCodeObject* co, PyThreadState* tstate);
void jit_start();
void jit_finish();
static long opcache_min_runs = OPCACHE_MIN_RUNS;
static long jit_min_runs = JIT_MIN_RUNS;

#define JIT_FUNC_FAILED ((JitFunc)0x1)

static PyObject* _Py_HOT_FUNCTION
_PyEval_EvalFrame_AOT_JIT(PyFrameObject *f, PyThreadState * const tstate, PyObject** stack_pointer, JitFunc jit_code);
__attribute__((noinline)) static PyObject* _Py_HOT_FUNCTION
_PyEval_EvalFrame_AOT_Interpreter(PyFrameObject *f, int throwflag, PyThreadState * const tstate, PyObject** stack_pointer, int can_use_jit, int jit_first_trace_for_line);

PyObject* _Py_HOT_FUNCTION
_PyEval_EvalFrame_AOT_Interpreter(PyFrameObject *f, int throwflag, PyThreadState * const tstate, PyObject** stack_pointer, int can_use_jit, int jit_first_trace_for_line)
{
#ifdef DXPAIRS
    int lastopcode = 0;
#endif
    const _Py_CODEUNIT *next_instr;
    int opcode;        /* Current opcode */
    int oparg;         /* Current opcode argument, if any */
    PyObject **fastlocals, **freevars;
    PyObject *retval = NULL;            /* Return value */
    _PyRuntimeState * const runtime = &_PyRuntime;
    struct _ceval_runtime_state * const ceval = &runtime->ceval;
    _Py_atomic_int * const eval_breaker = &ceval->eval_breaker;
    PyCodeObject *co;

    /* when tracing we set things up so that

           not (instr_lb <= current_bytecode_offset < instr_ub)

       is true when the line being executed has changed.  The
       initial values are such as to make this false the first
       time it is tested. */
    int instr_ub = -1, instr_lb = 0, instr_prev = -1;

    const _Py_CODEUNIT *first_instr;
    PyObject *names;
    PyObject *consts;
    //_PyOpcache *co_opcache; // Pyston change: use local variable instead

#ifdef LLTRACE
    _Py_IDENTIFIER(__ltrace__);
#endif

/* Computed GOTOs, or
       the-optimization-commonly-but-improperly-known-as-"threaded code"
   using gcc's labels-as-values extension
   (http://gcc.gnu.org/onlinedocs/gcc/Labels-as-Values.html).

   The traditional bytecode evaluation loop uses a "switch" statement, which
   decent compilers will optimize as a single indirect branch instruction
   combined with a lookup table of jump addresses. However, since the
   indirect jump instruction is shared by all opcodes, the CPU will have a
   hard time making the right prediction for where to jump next (actually,
   it will be always wrong except in the uncommon case of a sequence of
   several identical opcodes).

   "Threaded code" in contrast, uses an explicit jump table and an explicit
   indirect jump instruction at the end of each opcode. Since the jump
   instruction is at a different address for each opcode, the CPU will make a
   separate prediction for each of these instructions, which is equivalent to
   predicting the second opcode of each opcode pair. These predictions have
   a much better chance to turn out valid, especially in small bytecode loops.

   A mispredicted branch on a modern CPU flushes the whole pipeline and
   can cost several CPU cycles (depending on the pipeline depth),
   and potentially many more instructions (depending on the pipeline width).
   A correctly predicted branch, however, is nearly free.

   At the time of this writing, the "threaded code" version is up to 15-20%
   faster than the normal "switch" version, depending on the compiler and the
   CPU architecture.

   We disable the optimization if DYNAMIC_EXECUTION_PROFILE is defined,
   because it would render the measurements invalid.


   NOTE: care must be taken that the compiler doesn't try to "optimize" the
   indirect jumps by sharing them between all opcodes. Such optimizations
   can be disabled on gcc by using the -fno-gcse flag (or possibly
   -fno-crossjumping).
*/

#ifdef DYNAMIC_EXECUTION_PROFILE
#undef USE_COMPUTED_GOTOS
#define USE_COMPUTED_GOTOS 0
#endif

#ifdef HAVE_COMPUTED_GOTOS
    #ifndef USE_COMPUTED_GOTOS
    #define USE_COMPUTED_GOTOS 1
    #endif
#else
    #if defined(USE_COMPUTED_GOTOS) && USE_COMPUTED_GOTOS
    #error "Computed gotos are not supported on this compiler."
    #endif
    #undef USE_COMPUTED_GOTOS
    #define USE_COMPUTED_GOTOS 0
#endif

#if USE_COMPUTED_GOTOS
/* Import the static jump table */
#include "opcode_targets.h"

#define TARGET(op) \
    op: \
    TARGET_##op

#ifdef LLTRACE
#define FAST_DISPATCH() \
    { \
        if (!lltrace && !_Py_TracingPossible(ceval) && !PyDTrace_LINE_ENABLED()) { \
            f->f_lasti = INSTR_OFFSET(); \
            NEXTOPARG(); \
            goto *opcode_targets[opcode]; \
        } \
        goto fast_next_opcode; \
    }
#else
#define FAST_DISPATCH() \
    { \
        if (!_Py_TracingPossible(ceval) && !PyDTrace_LINE_ENABLED()) { \
            f->f_lasti = INSTR_OFFSET(); \
            NEXTOPARG(); \
            goto *opcode_targets[opcode]; \
        } \
        goto fast_next_opcode; \
    }
#endif

#define DISPATCH() \
    { \
        if (!_Py_atomic_load_relaxed(eval_breaker)) { \
            FAST_DISPATCH(); \
        } \
        continue; \
    }

#else
#define TARGET(op) op
#define FAST_DISPATCH() goto fast_next_opcode
#define DISPATCH() continue
#endif


/* Tuple access macros */

#ifndef Py_DEBUG
#define GETITEM(v, i) PyTuple_GET_ITEM((PyTupleObject *)(v), (i))
#else
#define GETITEM(v, i) PyTuple_GetItem((v), (i))
#endif

/* Code access macros */

/* The integer overflow is checked by an assertion below. */
#define INSTR_OFFSET()  \
    (sizeof(_Py_CODEUNIT) * (int)(next_instr - first_instr))
#define NEXTOPARG()  do { \
        _Py_CODEUNIT word = *next_instr; \
        opcode = _Py_OPCODE(word); \
        oparg = _Py_OPARG(word); \
        next_instr++; \
    } while (0)

#define JUMPTO(x)       (next_instr = first_instr + (x) / sizeof(_Py_CODEUNIT))
#define JUMPBY(x)       (next_instr += (x) / sizeof(_Py_CODEUNIT))

// JUMPTO which is counting backward jumps in order to todo OSR
#define JUMPTO_WITH_OSR(x)  \
    do { \
        _Py_CODEUNIT* old_instr = next_instr; \
        JUMPTO(x); \
        if (next_instr < old_instr) { /* only increment if backwards jump */ \
            HANDLE_JUMP_BACKWARD_OSR(); \
        } \
    } while (0)


/* OpCode prediction macros
    Some opcodes tend to come in pairs thus making it possible to
    predict the second code when the first is run.  For example,
    COMPARE_OP is often followed by POP_JUMP_IF_FALSE or POP_JUMP_IF_TRUE.

    Verifying the prediction costs a single high-speed test of a register
    variable against a constant.  If the pairing was good, then the
    processor's own internal branch predication has a high likelihood of
    success, resulting in a nearly zero-overhead transition to the
    next opcode.  A successful prediction saves a trip through the eval-loop
    including its unpredictable switch-case branch.  Combined with the
    processor's internal branch prediction, a successful PREDICT has the
    effect of making the two opcodes run as if they were a single new opcode
    with the bodies combined.

    If collecting opcode statistics, your choices are to either keep the
    predictions turned-on and interpret the results as if some opcodes
    had been combined or turn-off predictions so that the opcode frequency
    counter updates for both opcodes.

    Opcode prediction is disabled with threaded code, since the latter allows
    the CPU to record separate branch prediction information for each
    opcode.

*/

#if defined(DYNAMIC_EXECUTION_PROFILE) || USE_COMPUTED_GOTOS
#define PREDICT(op)             if (0) goto PRED_##op
#else
#define PREDICT(op) \
    do{ \
        _Py_CODEUNIT word = *next_instr; \
        opcode = _Py_OPCODE(word); \
        if (opcode == op){ \
            oparg = _Py_OPARG(word); \
            next_instr++; \
            goto PRED_##op; \
        } \
    } while(0)
#endif
#define PREDICTED(op)           PRED_##op:


/* Stack manipulation macros */

/* The stack can grow at most MAXINT deep, as co_nlocals and
   co_stacksize are ints. */
#define STACK_LEVEL()     ((int)(stack_pointer - f->f_valuestack))
#define EMPTY()           (STACK_LEVEL() == 0)
#define TOP()             (stack_pointer[-1])
#define SECOND()          (stack_pointer[-2])
#define THIRD()           (stack_pointer[-3])
#define FOURTH()          (stack_pointer[-4])
#define PEEK(n)           (stack_pointer[-(n)])
#define SET_TOP(v)        (stack_pointer[-1] = (v))
#define SET_SECOND(v)     (stack_pointer[-2] = (v))
#define SET_THIRD(v)      (stack_pointer[-3] = (v))
#define SET_FOURTH(v)     (stack_pointer[-4] = (v))
#define SET_VALUE(n, v)   (stack_pointer[-(n)] = (v))
#define BASIC_STACKADJ(n) (stack_pointer += n)
#define BASIC_PUSH(v)     (*stack_pointer++ = (v))
#define BASIC_POP()       (*--stack_pointer)

#ifdef LLTRACE
#define PUSH(v)         { (void)(BASIC_PUSH(v), \
                          lltrace && prtrace(tstate, TOP(), "push")); \
                          assert(STACK_LEVEL() <= co->co_stacksize); }
#define POP()           ((void)(lltrace && prtrace(tstate, TOP(), "pop")), \
                         BASIC_POP())
#define STACK_GROW(n)   do { \
                          assert(n >= 0); \
                          (void)(BASIC_STACKADJ(n), \
                          lltrace && prtrace(tstate, TOP(), "stackadj")); \
                          assert(STACK_LEVEL() <= co->co_stacksize); \
                        } while (0)
#define STACK_SHRINK(n) do { \
                            assert(n >= 0); \
                            (void)(lltrace && prtrace(tstate, TOP(), "stackadj")); \
                            (void)(BASIC_STACKADJ(-n)); \
                            assert(STACK_LEVEL() <= co->co_stacksize); \
                        } while (0)
#define EXT_POP(STACK_POINTER) ((void)(lltrace && \
                                prtrace(tstate, (STACK_POINTER)[-1], "ext_pop")), \
                                *--(STACK_POINTER))
#else
#define PUSH(v)                BASIC_PUSH(v)
#define POP()                  BASIC_POP()
#define STACK_GROW(n)          BASIC_STACKADJ(n)
#define STACK_SHRINK(n)        BASIC_STACKADJ(-n)
#define EXT_POP(STACK_POINTER) (*--(STACK_POINTER))
#endif

/* Local variable macros */

#define GETLOCAL(i)     (fastlocals[i])

/* The SETLOCAL() macro must not DECREF the local variable in-place and
   then store the new value; it must copy the old value to a temporary
   value, then store the new value, and then DECREF the temporary value.
   This is because it is possible that during the DECREF the frame is
   accessed by other code (e.g. a __del__ method or gc.collect()) and the
   variable would be pointing to already-freed memory. */
#define SETLOCAL(i, value)      do { PyObject *tmp = GETLOCAL(i); \
                                     GETLOCAL(i) = value; \
                                     Py_XDECREF(tmp); } while (0)


#define UNWIND_BLOCK(b) \
    while (STACK_LEVEL() > (b)->b_level) { \
        PyObject *v = POP(); \
        Py_XDECREF(v); \
    }

#define UNWIND_EXCEPT_HANDLER(b) \
    do { \
        PyObject *type, *value, *traceback; \
        _PyErr_StackItem *exc_info; \
        assert(STACK_LEVEL() >= (b)->b_level + 3); \
        while (STACK_LEVEL() > (b)->b_level + 3) { \
            value = POP(); \
            Py_XDECREF(value); \
        } \
        exc_info = tstate->exc_info; \
        type = exc_info->exc_type; \
        value = exc_info->exc_value; \
        traceback = exc_info->exc_traceback; \
        exc_info->exc_type = POP(); \
        exc_info->exc_value = POP(); \
        exc_info->exc_traceback = POP(); \
        Py_XDECREF(type); \
        Py_XDECREF(value); \
        Py_XDECREF(traceback); \
    } while(0)

    /* macros for opcode cache */
#define OPCACHE_FETCH() \
    do { \
        unsigned char co_opt_offset = \
            co->co_opcache_map[next_instr - first_instr]; \
        assert(co_opt_offset <= co->co_opcache_size); \
        co_opcache = &co->co_opcache[co_opt_offset - 1]; \
        assert(co_opcache != NULL); \
    } while (0)

#define OPCACHE_CHECK() \
    do { \
        co_opcache = NULL; \
        if (co->co_opcache != NULL) { \
            unsigned char co_opt_offset = \
                co->co_opcache_map[next_instr - first_instr]; \
            if (co_opt_offset > 0) { \
                assert(co_opt_offset <= co->co_opcache_size); \
                co_opcache = &co->co_opcache[co_opt_offset - 1]; \
                assert(co_opcache != NULL); \
            } \
        } \
    } while (0)

#define OPCACHE_DISABLE() \
    do { \
        co_opcache = NULL; \
        if (co->co_opcache != NULL) { \
            co->co_opcache_map[next_instr - first_instr] = 0; \
        } \
    } while (0)

#if OPCACHE_STATS

#define OPCACHE_STAT_GLOBAL_HIT() \
    do { \
        if (co->co_opcache != NULL) opcache_global_hits++; \
    } while (0)

#define OPCACHE_STAT_GLOBAL_MISS() \
    do { \
        if (co->co_opcache != NULL) opcache_global_misses++; \
    } while (0)

#define OPCACHE_STAT_GLOBAL_OPT() \
    do { \
        if (co->co_opcache != NULL) opcache_global_opts++; \
    } while (0)

#define OPCACHE_INIT_IF_HIT_THRESHOLD() \
    do { \
        if (co->co_opcache_map == NULL && \
            co->co_opcache_flag >= opcache_min_runs && co->co_opcache_flag <= opcache_min_runs+OPCACHE_INC_FUNC_ENTRY) { \
            if (_PyCode_InitOpcache(co) < 0) { \
                return NULL; \
            } \
            opcache_code_objects_extra_mem += \
                PyBytes_Size(co->co_code) / sizeof(_Py_CODEUNIT) + \
                sizeof(_PyOpcache) * co->co_opcache_size; \
            opcache_code_objects++; \
        } \
    } while (0)

#else /* OPCACHE_STATS */

#define OPCACHE_STAT_GLOBAL_HIT()
#define OPCACHE_STAT_GLOBAL_MISS()
#define OPCACHE_STAT_GLOBAL_OPT()

#define OPCACHE_INIT_IF_HIT_THRESHOLD() \
    do { \
        if (co->co_opcache_map == NULL && \
            co->co_opcache_flag >= opcache_min_runs && co->co_opcache_flag <= opcache_min_runs+OPCACHE_INC_FUNC_ENTRY) { \
            if (_PyCode_InitOpcache(co) < 0) { \
                return NULL; \
            } \
        } \
    } while (0)

#endif

#define BINARY_OP_OPCACHE_PROF() \
    do { \
        if (Py_TYPE(left) == Py_TYPE(right)) { \
            _PyOpcache *co_opcache; \
            OPCACHE_CHECK(); \
            if (co_opcache && co_opcache->optimized < 10) { \
                co_opcache->u.t_refcnt.type = Py_TYPE(left); \
                co_opcache->u.t_refcnt.refcnt1_left += (Py_REFCNT(left) == 1) ? 1 : 0; \
                co_opcache->u.t_refcnt.refcnt1_right+= (Py_REFCNT(right) == 1) ? 1 : 0; \
                ++co_opcache->optimized; \
            } \
        } \
    } while (0)


// increments the number of times this loop got ececuted and if the threshold is hit JIT func and do OSR.
#define HANDLE_JUMP_BACKWARD_OSR() \
    do {  \
        ++co->co_opcache_flag;  \
        OPCACHE_INIT_IF_HIT_THRESHOLD();  \
        /* check if we should switch over to the JIT (OSR) */  \
        if (co->co_opcache_flag > jit_min_runs && can_use_jit && co->co_jit_code == NULL  \
            && !_Py_TracingPossible(ceval)) { /* don't OSR if tracing is enabled because we seem to skip a line */ \
            co->co_jit_code = jit_func(co, tstate);  \
            if (co->co_jit_code) {  \
                /* JUMPTO() did not update f->f_lasti  \
                (it still points to the JUMP_ABSOLUTE - not the destination of the jump)  \
                 update f->f_lasti manually like DISPATCH() would do because  \
                 we can only enter the machine code at jump targets. */ \
                f->f_lasti = INSTR_OFFSET() - 2; /* -2 because our JIT entry is always adding two */ \
                return _PyEval_EvalFrame_AOT_JIT(f, tstate, stack_pointer, (JitFunc)co->co_jit_code); \
            } else { \
                /* never try again to JIT compile this python function */ \
                co->co_jit_code = JIT_FUNC_FAILED; \
                can_use_jit = 0; \
            } \
        } \
    } while (0)

/* Start of code */
#if PROFILE_OPCODES
    PyObject* profile_func_entry = update_opcode_profile_func_enter(f);
#endif

    co = f->f_code;

    if (can_use_jit && co->co_jit_code == 0 && co->co_opcache_flag >= jit_min_runs /* jit after that many calls or gen yields */) {
        // JIT assumes opcache is always on
        if (co->co_opcache_map == NULL) {
            _PyCode_InitOpcache(co);
        }
#if 0
        struct timespec start, end;
        clock_gettime(CLOCK_REALTIME, &start);
        co->co_jit_code = jit_func(co, tstate);
        clock_gettime(CLOCK_REALTIME, &end);
        long time = 1000*1000 * (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1000;
        static long totaltime = 0;
        totaltime += time;
        printf("Took %ldus to jit %s (totaltime: %ld us)\n",
                time, PyUnicode_AsUTF8(co->co_name), totaltime);
#else
        co->co_jit_code = jit_func(co, tstate);
#endif
        if (co->co_jit_code) {
            return _PyEval_EvalFrame_AOT_JIT(f, tstate, stack_pointer, (JitFunc)co->co_jit_code);
        } else {
            // never try again to JIT compile this python function
            co->co_jit_code = JIT_FUNC_FAILED;
            can_use_jit = 0;
        }
    }

    names = co->co_names;
    consts = co->co_consts;
    fastlocals = f->f_localsplus;
    freevars = f->f_localsplus + co->co_nlocals;
    assert(PyBytes_Check(co->co_code));
    assert(PyBytes_GET_SIZE(co->co_code) <= INT_MAX);
    assert(PyBytes_GET_SIZE(co->co_code) % sizeof(_Py_CODEUNIT) == 0);
    assert(_Py_IS_ALIGNED(PyBytes_AS_STRING(co->co_code), sizeof(_Py_CODEUNIT)));
    first_instr = (_Py_CODEUNIT *) PyBytes_AS_STRING(co->co_code);

    /*
       f->f_lasti refers to the index of the last instruction,
       unless it's -1 in which case next_instr should be first_instr.

       YIELD_FROM sets f_lasti to itself, in order to repeatedly yield
       multiple values.

       When the PREDICT() macros are enabled, some opcode pairs follow in
       direct succession without updating f->f_lasti.  A successful
       prediction effectively links the two codes together as if they
       were a single new opcode; accordingly,f->f_lasti will point to
       the first code in the pair (for instance, GET_ITER followed by
       FOR_ITER is effectively a single opcode and f->f_lasti will point
       to the beginning of the combined pair.)
    */
    assert(f->f_lasti >= -1);
    next_instr = first_instr;
    if (f->f_lasti >= 0) { // generator resume
        assert(f->f_lasti % sizeof(_Py_CODEUNIT) == 0);
        next_instr += f->f_lasti / sizeof(_Py_CODEUNIT) + 1;
    } else { // function entry
        co->co_opcache_flag += OPCACHE_INC_FUNC_ENTRY;
        OPCACHE_INIT_IF_HIT_THRESHOLD();
    }

#ifdef LLTRACE
    lltrace = _PyDict_GetItemId(f->f_globals, &PyId___ltrace__) != NULL;
#endif

    if (throwflag) /* support for generator.throw() */
        goto error;

#ifdef Py_DEBUG
    /* PyEval_EvalFrameEx() must not be called with an exception set,
       because it can clear it (directly or indirectly) and so the
       caller loses its exception */
    assert(!_PyErr_Occurred(tstate));
#endif

main_loop:
    for (;;) {
        assert(stack_pointer >= f->f_valuestack); /* else underflow */
        assert(STACK_LEVEL() <= co->co_stacksize);  /* else overflow */
        assert(!_PyErr_Occurred(tstate));

        /* Do periodic things.  Doing this every time through
           the loop would add too much overhead, so we do it
           only every Nth instruction.  We also do it if
           ``pendingcalls_to_do'' is set, i.e. when an asynchronous
           event needs attention (e.g. a signal handler or
           async I/O handler); see Py_AddPendingCall() and
           Py_MakePendingCalls() above. */

        if (_Py_atomic_load_relaxed(eval_breaker)) {
            opcode = _Py_OPCODE(*next_instr);
            if (opcode == SETUP_FINALLY ||
                opcode == SETUP_WITH ||
                opcode == BEFORE_ASYNC_WITH ||
                opcode == YIELD_FROM) {
                /* Few cases where we skip running signal handlers and other
                   pending calls:
                   - If we're about to enter the 'with:'. It will prevent
                     emitting a resource warning in the common idiom
                     'with open(path) as file:'.
                   - If we're about to enter the 'async with:'.
                   - If we're about to enter the 'try:' of a try/finally (not
                     *very* useful, but might help in some cases and it's
                     traditional)
                   - If we're resuming a chain of nested 'yield from' or
                     'await' calls, then each frame is parked with YIELD_FROM
                     as its next opcode. If the user hit control-C we want to
                     wait until we've reached the innermost frame before
                     running the signal handler and raising KeyboardInterrupt
                     (see bpo-30039).
                */
                goto fast_next_opcode;
            }

            if (_Py_atomic_load_relaxed(&ceval->signals_pending)) {
                if (handle_signals(runtime) != 0) {
                    goto error;
                }
            }
            if (_Py_atomic_load_relaxed(&ceval->pending.calls_to_do)) {
                if (make_pending_calls(runtime) != 0) {
                    goto error;
                }
            }

            if (_Py_atomic_load_relaxed(&ceval->gil_drop_request)) {
                /* Give another thread a chance */
                if (_PyThreadState_Swap(&runtime->gilstate, NULL) != tstate) {
                    Py_FatalError("ceval: tstate mix-up");
                }
                drop_gil(ceval, tstate);

                /* Other threads may run now */

                take_gil(ceval, tstate);

                /* Check if we should make a quick exit. */
                exit_thread_if_finalizing(runtime, tstate);

                if (_PyThreadState_Swap(&runtime->gilstate, tstate) != NULL) {
                    Py_FatalError("ceval: orphan tstate");
                }
            }
            /* Check for asynchronous exceptions. */
            if (tstate->async_exc != NULL) {
                PyObject *exc = tstate->async_exc;
                tstate->async_exc = NULL;
                UNSIGNAL_ASYNC_EXC(ceval);
                _PyErr_SetNone(tstate, exc);
                Py_DECREF(exc);
                goto error;
            }
        }

    fast_next_opcode:
        f->f_lasti = INSTR_OFFSET();

        if (PyDTrace_LINE_ENABLED())
            maybe_dtrace_line(f, &instr_lb, &instr_ub, &instr_prev);

        /* line-by-line tracing support */

        if (_Py_TracingPossible(ceval) &&
            tstate->c_tracefunc != NULL && !tstate->tracing) {
            int err;
            /* see maybe_call_line_trace
               for expository comments */
            f->f_stacktop = stack_pointer;

            err = maybe_call_line_trace(tstate->c_tracefunc,
                                        tstate->c_traceobj,
                                        tstate, f,
                                        &instr_lb, &instr_ub, &instr_prev, &jit_first_trace_for_line);
            /* Reload possibly changed frame fields */
            JUMPTO(f->f_lasti);
            if (f->f_stacktop != NULL) {
                stack_pointer = f->f_stacktop;
                f->f_stacktop = NULL;
            }
            if (err)
                /* trace function raised an exception */
                goto error;
        }

        /* Extract opcode and argument */

        NEXTOPARG();
    dispatch_opcode:
#ifdef DYNAMIC_EXECUTION_PROFILE
#ifdef DXPAIRS
        dxpairs[lastopcode][opcode]++;
        lastopcode = opcode;
#endif
        dxp[opcode]++;
#endif

#ifdef LLTRACE
        /* Instruction tracing */

        if (lltrace) {
            if (HAS_ARG(opcode)) {
                printf("%d: %d, %d\n",
                       f->f_lasti, opcode, oparg);
            }
            else {
                printf("%d: %d\n",
                       f->f_lasti, opcode);
            }
        }
#endif
        switch (opcode) {

        /* BEWARE!
           It is essential that any operation that fails must goto error
           and that all operation that succeed call [FAST_]DISPATCH() ! */

        case TARGET(NOP): {
            FAST_DISPATCH();
        }

        case TARGET(LOAD_FAST): {
            PyObject *value = GETLOCAL(oparg);
            if (value == NULL) {
                format_exc_check_arg(tstate, PyExc_UnboundLocalError,
                                     UNBOUNDLOCAL_ERROR_MSG,
                                     PyTuple_GetItem(co->co_varnames, oparg));
                goto error;
            }
            Py_INCREF(value);
            PUSH(value);
            FAST_DISPATCH();
        }

        case TARGET(LOAD_CONST): {
            PREDICTED(LOAD_CONST);
            PyObject *value = GETITEM(consts, oparg);
            Py_INCREF(value);
            PUSH(value);
            FAST_DISPATCH();
        }

        case TARGET(STORE_FAST): {
            PREDICTED(STORE_FAST);
            PyObject *value = POP();
            SETLOCAL(oparg, value);
            FAST_DISPATCH();
        }

        case TARGET(POP_TOP): {
            PyObject *value = POP();
            Py_DECREF(value);
            FAST_DISPATCH();
        }

        case TARGET(ROT_TWO): {
            PyObject *top = TOP();
            PyObject *second = SECOND();
            SET_TOP(second);
            SET_SECOND(top);
            FAST_DISPATCH();
        }

        case TARGET(ROT_THREE): {
            PyObject *top = TOP();
            PyObject *second = SECOND();
            PyObject *third = THIRD();
            SET_TOP(second);
            SET_SECOND(third);
            SET_THIRD(top);
            FAST_DISPATCH();
        }

        case TARGET(ROT_FOUR): {
            PyObject *top = TOP();
            PyObject *second = SECOND();
            PyObject *third = THIRD();
            PyObject *fourth = FOURTH();
            SET_TOP(second);
            SET_SECOND(third);
            SET_THIRD(fourth);
            SET_FOURTH(top);
            FAST_DISPATCH();
        }

        case TARGET(DUP_TOP): {
            PyObject *top = TOP();
            Py_INCREF(top);
            PUSH(top);
            FAST_DISPATCH();
        }

        case TARGET(DUP_TOP_TWO): {
            PyObject *top = TOP();
            PyObject *second = SECOND();
            Py_INCREF(top);
            Py_INCREF(second);
            STACK_GROW(2);
            SET_TOP(top);
            SET_SECOND(second);
            FAST_DISPATCH();
        }

        case TARGET(UNARY_POSITIVE): {
            PyObject *value = TOP();
            PyObject *res = PyNumber_Positive(value);
            Py_DECREF(value);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(UNARY_NEGATIVE): {
            PyObject *value = TOP();
            PyObject *res = PyNumber_Negative(value);
            Py_DECREF(value);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(UNARY_NOT): {
            PyObject *value = TOP();
            int err = PyObject_IsTrue(value);
            Py_DECREF(value);
            if (err == 0) {
                Py_INCREF_IMMORTAL(Py_True);
                SET_TOP(Py_True);
                DISPATCH();
            }
            else if (err > 0) {
                Py_INCREF_IMMORTAL(Py_False);
                SET_TOP(Py_False);
                DISPATCH();
            }
            STACK_SHRINK(1);
            goto error;
        }

        case TARGET(UNARY_INVERT): {
            PyObject *value = TOP();
            PyObject *res = PyNumber_Invert(value);
            Py_DECREF(value);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(BINARY_POWER): {
            PyObject *exp = POP();
            PyObject *base = TOP();
            PyObject *res = PyNumber_Power(base, exp, Py_None);
            Py_DECREF(base);
            Py_DECREF(exp);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(BINARY_MULTIPLY): {
            PyObject *right = POP();
            PyObject *left = TOP();

            BINARY_OP_OPCACHE_PROF();

            PyObject *res = PyNumber_Multiply(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(BINARY_MATRIX_MULTIPLY): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *res = PyNumber_MatrixMultiply(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(BINARY_TRUE_DIVIDE): {
            PyObject *divisor = POP();
            PyObject *dividend = TOP();
            PyObject *quotient = PyNumber_TrueDivide(dividend, divisor);
            Py_DECREF(dividend);
            Py_DECREF(divisor);
            SET_TOP(quotient);
            if (quotient == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(BINARY_FLOOR_DIVIDE): {
            PyObject *divisor = POP();
            PyObject *dividend = TOP();
            PyObject *quotient = PyNumber_FloorDivide(dividend, divisor);
            Py_DECREF(dividend);
            Py_DECREF(divisor);
            SET_TOP(quotient);
            if (quotient == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(BINARY_MODULO): {
            PyObject *divisor = POP();
            PyObject *dividend = TOP();
            PyObject *res;
            if (PyUnicode_CheckExact(dividend) && (
                  !PyUnicode_Check(divisor) || PyUnicode_CheckExact(divisor))) {
              // fast path; string formatting, but not if the RHS is a str subclass
              // (see issue28598)
              res = PyUnicode_Format(dividend, divisor);
            } else {
              res = PyNumber_Remainder(dividend, divisor);
            }
            Py_DECREF(divisor);
            Py_DECREF(dividend);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(BINARY_ADD): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *sum;

            BINARY_OP_OPCACHE_PROF();

            /* NOTE(haypo): Please don't try to micro-optimize int+int on
               CPython using bytecode, it is simply worthless.
               See http://bugs.python.org/issue21955 and
               http://bugs.python.org/issue10044 for the discussion. In short,
               no patch shown any impact on a realistic benchmark, only a minor
               speedup on microbenchmarks. */
            if (PyUnicode_CheckExact(left) &&
                     PyUnicode_CheckExact(right)) {
                sum = unicode_concatenate(tstate, left, right, f, next_instr);
                /* unicode_concatenate consumed the ref to left */
            }
            else {
                sum = PyNumber_Add(left, right);
                Py_DECREF(left);
            }
            Py_DECREF(right);
            SET_TOP(sum);
            if (sum == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(BINARY_SUBTRACT): {
            PyObject *right = POP();
            PyObject *left = TOP();

            BINARY_OP_OPCACHE_PROF();

            PyObject *diff = PyNumber_Subtract(left, right);
            Py_DECREF(right);
            Py_DECREF(left);
            SET_TOP(diff);
            if (diff == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(BINARY_SUBSCR): {
            PyObject *sub = POP();
            PyObject *container = TOP();

            _PyOpcache *co_opcache;
            OPCACHE_CHECK();
            if (co_opcache) {
                co_opcache->u.t.type = Py_TYPE(container);
                co_opcache->optimized = 1;
            }

            PyObject *res = PyObject_GetItem(container, sub);
            Py_DECREF(container);
            Py_DECREF(sub);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(BINARY_LSHIFT): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *res = PyNumber_Lshift(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(BINARY_RSHIFT): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *res = PyNumber_Rshift(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(BINARY_AND): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *res = PyNumber_And(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(BINARY_XOR): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *res = PyNumber_Xor(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(BINARY_OR): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *res = PyNumber_Or(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(LIST_APPEND): {
            PyObject *v = POP();
            PyObject *list = PEEK(oparg);
            int err;
            err = PyList_Append(list, v);
            Py_DECREF(v);
            if (err != 0)
                goto error;
            PREDICT(JUMP_ABSOLUTE);
            DISPATCH();
        }

        case TARGET(SET_ADD): {
            PyObject *v = POP();
            PyObject *set = PEEK(oparg);
            int err;
            err = PySet_Add(set, v);
            Py_DECREF(v);
            if (err != 0)
                goto error;
            PREDICT(JUMP_ABSOLUTE);
            DISPATCH();
        }

        case TARGET(INPLACE_POWER): {
            PyObject *exp = POP();
            PyObject *base = TOP();
            PyObject *res = PyNumber_InPlacePower(base, exp, Py_None);
            Py_DECREF(base);
            Py_DECREF(exp);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(INPLACE_MULTIPLY): {
            PyObject *right = POP();
            PyObject *left = TOP();

            BINARY_OP_OPCACHE_PROF();

            PyObject *res = PyNumber_InPlaceMultiply(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(INPLACE_MATRIX_MULTIPLY): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *res = PyNumber_InPlaceMatrixMultiply(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(INPLACE_TRUE_DIVIDE): {
            PyObject *divisor = POP();
            PyObject *dividend = TOP();
            PyObject *quotient = PyNumber_InPlaceTrueDivide(dividend, divisor);
            Py_DECREF(dividend);
            Py_DECREF(divisor);
            SET_TOP(quotient);
            if (quotient == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(INPLACE_FLOOR_DIVIDE): {
            PyObject *divisor = POP();
            PyObject *dividend = TOP();
            PyObject *quotient = PyNumber_InPlaceFloorDivide(dividend, divisor);
            Py_DECREF(dividend);
            Py_DECREF(divisor);
            SET_TOP(quotient);
            if (quotient == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(INPLACE_MODULO): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *mod = PyNumber_InPlaceRemainder(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(mod);
            if (mod == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(INPLACE_ADD): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *sum;

            BINARY_OP_OPCACHE_PROF();

            if (PyUnicode_CheckExact(left) && PyUnicode_CheckExact(right)) {
                sum = unicode_concatenate(tstate, left, right, f, next_instr);
                /* unicode_concatenate consumed the ref to left */
            }
            else {
                sum = PyNumber_InPlaceAdd(left, right);
                Py_DECREF(left);
            }
            Py_DECREF(right);
            SET_TOP(sum);
            if (sum == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(INPLACE_SUBTRACT): {
            PyObject *right = POP();
            PyObject *left = TOP();

            BINARY_OP_OPCACHE_PROF();

            PyObject *diff = PyNumber_InPlaceSubtract(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(diff);
            if (diff == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(INPLACE_LSHIFT): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *res = PyNumber_InPlaceLshift(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(INPLACE_RSHIFT): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *res = PyNumber_InPlaceRshift(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(INPLACE_AND): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *res = PyNumber_InPlaceAnd(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(INPLACE_XOR): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *res = PyNumber_InPlaceXor(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(INPLACE_OR): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *res = PyNumber_InPlaceOr(left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(STORE_SUBSCR): {
            PyObject *sub = TOP();
            PyObject *container = SECOND();
            PyObject *v = THIRD();
            int err;
            STACK_SHRINK(3);

            _PyOpcache *co_opcache;
            OPCACHE_CHECK();
            if (co_opcache) {
                co_opcache->u.t.type = Py_TYPE(container);
                co_opcache->optimized = 1;
            }

            /* container[sub] = v */
            err = PyObject_SetItem(container, sub, v);
            Py_DECREF(v);
            Py_DECREF(container);
            Py_DECREF(sub);
            if (err != 0)
                goto error;
            DISPATCH();
        }

        case TARGET(DELETE_SUBSCR): {
            PyObject *sub = TOP();
            PyObject *container = SECOND();
            int err;
            STACK_SHRINK(2);
            /* del container[sub] */
            err = PyObject_DelItem(container, sub);
            Py_DECREF(container);
            Py_DECREF(sub);
            if (err != 0)
                goto error;
            DISPATCH();
        }

        case TARGET(PRINT_EXPR): {
            _Py_IDENTIFIER(displayhook);
            PyObject *value = POP();
            PyObject *hook = _PySys_GetObjectId(&PyId_displayhook);
            PyObject *res;
            if (hook == NULL) {
                _PyErr_SetString(tstate, PyExc_RuntimeError,
                                 "lost sys.displayhook");
                Py_DECREF(value);
                goto error;
            }
            res = PyObject_CallFunctionObjArgs(hook, value, NULL);
            Py_DECREF(value);
            if (res == NULL)
                goto error;
            Py_DECREF(res);
            DISPATCH();
        }

        case TARGET(RAISE_VARARGS): {
            PyObject *cause = NULL, *exc = NULL;
            switch (oparg) {
            case 2:
                cause = POP(); /* cause */
                /* fall through */
            case 1:
                exc = POP(); /* exc */
                /* fall through */
            case 0:
                if (do_raise(tstate, exc, cause)) {
                    goto exception_unwind;
                }
                break;
            default:
                _PyErr_SetString(tstate, PyExc_SystemError,
                                 "bad RAISE_VARARGS oparg");
                break;
            }
            goto error;
        }

        case TARGET(RETURN_VALUE): {
            retval = POP();
            assert(f->f_iblock == 0);
            goto exit_returning;
        }

        case TARGET(GET_AITER): {
            unaryfunc getter = NULL;
            PyObject *iter = NULL;
            PyObject *obj = TOP();
            PyTypeObject *type = Py_TYPE(obj);

            if (type->tp_as_async != NULL) {
                getter = type->tp_as_async->am_aiter;
            }

            if (getter != NULL) {
                iter = (*getter)(obj);
                Py_DECREF(obj);
                if (iter == NULL) {
                    SET_TOP(NULL);
                    goto error;
                }
            }
            else {
                SET_TOP(NULL);
                _PyErr_Format(tstate, PyExc_TypeError,
                              "'async for' requires an object with "
                              "__aiter__ method, got %.100s",
                              type->tp_name);
                Py_DECREF(obj);
                goto error;
            }

            if (Py_TYPE(iter)->tp_as_async == NULL ||
                    Py_TYPE(iter)->tp_as_async->am_anext == NULL) {

                SET_TOP(NULL);
                _PyErr_Format(tstate, PyExc_TypeError,
                              "'async for' received an object from __aiter__ "
                              "that does not implement __anext__: %.100s",
                              Py_TYPE(iter)->tp_name);
                Py_DECREF(iter);
                goto error;
            }

            SET_TOP(iter);
            DISPATCH();
        }

        case TARGET(GET_ANEXT): {
            unaryfunc getter = NULL;
            PyObject *next_iter = NULL;
            PyObject *awaitable = NULL;
            PyObject *aiter = TOP();
            PyTypeObject *type = Py_TYPE(aiter);

            if (PyAsyncGen_CheckExact(aiter)) {
                awaitable = type->tp_as_async->am_anext(aiter);
                if (awaitable == NULL) {
                    goto error;
                }
            } else {
                if (type->tp_as_async != NULL){
                    getter = type->tp_as_async->am_anext;
                }

                if (getter != NULL) {
                    next_iter = (*getter)(aiter);
                    if (next_iter == NULL) {
                        goto error;
                    }
                }
                else {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                  "'async for' requires an iterator with "
                                  "__anext__ method, got %.100s",
                                  type->tp_name);
                    goto error;
                }

                awaitable = _PyCoro_GetAwaitableIter(next_iter);
                if (awaitable == NULL) {
                    _PyErr_FormatFromCause(
                        PyExc_TypeError,
                        "'async for' received an invalid object "
                        "from __anext__: %.100s",
                        Py_TYPE(next_iter)->tp_name);

                    Py_DECREF(next_iter);
                    goto error;
                } else {
                    Py_DECREF(next_iter);
                }
            }

            PUSH(awaitable);
            PREDICT(LOAD_CONST);
            DISPATCH();
        }

        case TARGET(GET_AWAITABLE): {
            PREDICTED(GET_AWAITABLE);
            PyObject *iterable = TOP();
            PyObject *iter = _PyCoro_GetAwaitableIter(iterable);

            if (iter == NULL) {
                format_awaitable_error(tstate, Py_TYPE(iterable),
                                       _Py_OPCODE(next_instr[-2]));
            }

            Py_DECREF(iterable);

            if (iter != NULL && PyCoro_CheckExact(iter)) {
                PyObject *yf = _PyGen_yf((PyGenObject*)iter);
                if (yf != NULL) {
                    /* `iter` is a coroutine object that is being
                       awaited, `yf` is a pointer to the current awaitable
                       being awaited on. */
                    Py_DECREF(yf);
                    Py_CLEAR(iter);
                    _PyErr_SetString(tstate, PyExc_RuntimeError,
                                     "coroutine is being awaited already");
                    /* The code below jumps to `error` if `iter` is NULL. */
                }
            }

            SET_TOP(iter); /* Even if it's NULL */

            if (iter == NULL) {
                goto error;
            }

            PREDICT(LOAD_CONST);
            DISPATCH();
        }

        case TARGET(YIELD_FROM): {
            PyObject *v = POP();
            PyObject *receiver = TOP();
            int err;
            if (PyGen_CheckExact(receiver) || PyCoro_CheckExact(receiver)) {
                retval = _PyGen_Send((PyGenObject *)receiver, v);
            } else {
                _Py_IDENTIFIER(send);
                if (v == Py_None)
                    retval = Py_TYPE(receiver)->tp_iternext(receiver);
                else
                    retval = _PyObject_CallMethodIdObjArgs(receiver, &PyId_send, v, NULL);
            }
            Py_DECREF(v);
            if (retval == NULL) {
                PyObject *val;
                if (tstate->c_tracefunc != NULL
                        && _PyErr_ExceptionMatches(tstate, PyExc_StopIteration))
                    call_exc_trace(tstate->c_tracefunc, tstate->c_traceobj, tstate, f);
                err = _PyGen_FetchStopIterationValue(&val);
                if (err < 0)
                    goto error;
                Py_DECREF(receiver);
                SET_TOP(val);
                DISPATCH();
            }
            /* receiver remains on stack, retval is value to be yielded */
            f->f_stacktop = stack_pointer;
            /* and repeat... */
            assert(f->f_lasti >= (int)sizeof(_Py_CODEUNIT));
            f->f_lasti -= sizeof(_Py_CODEUNIT);
            goto exit_yielding;
        }

        case TARGET(YIELD_VALUE): {
            retval = POP();

            if (co->co_flags & CO_ASYNC_GENERATOR) {
                PyObject *w = _PyAsyncGenValueWrapperNew(retval);
                Py_DECREF(retval);
                if (w == NULL) {
                    retval = NULL;
                    goto error;
                }
                retval = w;
            }

            f->f_stacktop = stack_pointer;
            goto exit_yielding;
        }

        case TARGET(POP_EXCEPT): {
            PyObject *type, *value, *traceback;
            _PyErr_StackItem *exc_info;
            PyTryBlock *b = PyFrame_BlockPop(f);
            if (b->b_type != EXCEPT_HANDLER) {
                _PyErr_SetString(tstate, PyExc_SystemError,
                                 "popped block is not an except handler");
                goto error;
            }
            assert(STACK_LEVEL() >= (b)->b_level + 3 &&
                   STACK_LEVEL() <= (b)->b_level + 4);
            exc_info = tstate->exc_info;
            type = exc_info->exc_type;
            value = exc_info->exc_value;
            traceback = exc_info->exc_traceback;
            exc_info->exc_type = POP();
            exc_info->exc_value = POP();
            exc_info->exc_traceback = POP();
            Py_XDECREF(type);
            Py_XDECREF(value);
            Py_XDECREF(traceback);
            DISPATCH();
        }

        case TARGET(POP_BLOCK): {
            PREDICTED(POP_BLOCK);
            PyFrame_BlockPop(f);
            DISPATCH();
        }

        case TARGET(POP_FINALLY): {
            /* If oparg is 0 at the top of the stack are 1 or 6 values:
               Either:
                - TOP = NULL or an integer
               or:
                - (TOP, SECOND, THIRD) = exc_info()
                - (FOURTH, FITH, SIXTH) = previous exception for EXCEPT_HANDLER

               If oparg is 1 the value for 'return' was additionally pushed
               at the top of the stack.
            */
            PyObject *res = NULL;
            if (oparg) {
                res = POP();
            }
            PyObject *exc = POP();
            if (exc == NULL || PyLong_CheckExact(exc)) {
                Py_XDECREF(exc);
            }
            else {
                Py_DECREF(exc);
                Py_DECREF(POP());
                Py_DECREF(POP());

                PyObject *type, *value, *traceback;
                _PyErr_StackItem *exc_info;
                PyTryBlock *b = PyFrame_BlockPop(f);
                if (b->b_type != EXCEPT_HANDLER) {
                    _PyErr_SetString(tstate, PyExc_SystemError,
                                     "popped block is not an except handler");
                    Py_XDECREF(res);
                    goto error;
                }
                assert(STACK_LEVEL() == (b)->b_level + 3);
                exc_info = tstate->exc_info;
                type = exc_info->exc_type;
                value = exc_info->exc_value;
                traceback = exc_info->exc_traceback;
                exc_info->exc_type = POP();
                exc_info->exc_value = POP();
                exc_info->exc_traceback = POP();
                Py_XDECREF(type);
                Py_XDECREF(value);
                Py_XDECREF(traceback);
            }
            if (oparg) {
                PUSH(res);
            }
            DISPATCH();
        }

        case TARGET(CALL_FINALLY): {
            PyObject *ret = PyLong_FromLong(INSTR_OFFSET());
            if (ret == NULL) {
                goto error;
            }
            PUSH(ret);
            JUMPBY(oparg);
            FAST_DISPATCH();
        }

        case TARGET(BEGIN_FINALLY): {
            /* Push NULL onto the stack for using it in END_FINALLY,
               POP_FINALLY, WITH_CLEANUP_START and WITH_CLEANUP_FINISH.
             */
            PUSH(NULL);
            FAST_DISPATCH();
        }

        case TARGET(END_FINALLY): {
            PREDICTED(END_FINALLY);
            /* At the top of the stack are 1 or 6 values:
               Either:
                - TOP = NULL or an integer
               or:
                - (TOP, SECOND, THIRD) = exc_info()
                - (FOURTH, FITH, SIXTH) = previous exception for EXCEPT_HANDLER
            */
            PyObject *exc = POP();
            if (exc == NULL) {
                FAST_DISPATCH();
            }
            else if (PyLong_CheckExact(exc)) {
                int ret = _PyLong_AsInt(exc);
                Py_DECREF(exc);
                if (ret == -1 && _PyErr_Occurred(tstate)) {
                    goto error;
                }
                JUMPTO(ret);
                FAST_DISPATCH();
            }
            else {
                assert(PyExceptionClass_Check(exc));
                PyObject *val = POP();
                PyObject *tb = POP();
                _PyErr_Restore(tstate, exc, val, tb);
                goto exception_unwind;
            }
        }

        case TARGET(END_ASYNC_FOR): {
            PyObject *exc = POP();
            assert(PyExceptionClass_Check(exc));
            if (PyErr_GivenExceptionMatches(exc, PyExc_StopAsyncIteration)) {
                PyTryBlock *b = PyFrame_BlockPop(f);
                assert(b->b_type == EXCEPT_HANDLER);
                Py_DECREF(exc);
                UNWIND_EXCEPT_HANDLER(b);
                Py_DECREF(POP());
                JUMPBY(oparg);
                FAST_DISPATCH();
            }
            else {
                PyObject *val = POP();
                PyObject *tb = POP();
                _PyErr_Restore(tstate, exc, val, tb);
                goto exception_unwind;
            }
        }

        case TARGET(LOAD_BUILD_CLASS): {
            _Py_IDENTIFIER(__build_class__);

            PyObject *bc;
            if (PyDict_CheckExact(f->f_builtins)) {
                bc = _PyDict_GetItemIdWithError(f->f_builtins, &PyId___build_class__);
                if (bc == NULL) {
                    if (!_PyErr_Occurred(tstate)) {
                        _PyErr_SetString(tstate, PyExc_NameError,
                                         "__build_class__ not found");
                    }
                    goto error;
                }
                Py_INCREF(bc);
            }
            else {
                PyObject *build_class_str = _PyUnicode_FromId(&PyId___build_class__);
                if (build_class_str == NULL)
                    goto error;
                bc = PyObject_GetItem(f->f_builtins, build_class_str);
                if (bc == NULL) {
                    if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError))
                        _PyErr_SetString(tstate, PyExc_NameError,
                                         "__build_class__ not found");
                    goto error;
                }
            }
            PUSH(bc);
            DISPATCH();
        }

        case TARGET(STORE_NAME): {
            PyObject *name = GETITEM(names, oparg);
            PyObject *v = POP();
            PyObject *ns = f->f_locals;
            int err;
            if (ns == NULL) {
                _PyErr_Format(tstate, PyExc_SystemError,
                              "no locals found when storing %R", name);
                Py_DECREF(v);
                goto error;
            }
            if (PyDict_CheckExact(ns))
                err = PyDict_SetItem(ns, name, v);
            else
                err = PyObject_SetItem(ns, name, v);
            Py_DECREF(v);
            if (err != 0)
                goto error;
            DISPATCH();
        }

        case TARGET(DELETE_NAME): {
            PyObject *name = GETITEM(names, oparg);
            PyObject *ns = f->f_locals;
            int err;
            if (ns == NULL) {
                _PyErr_Format(tstate, PyExc_SystemError,
                              "no locals when deleting %R", name);
                goto error;
            }
            err = PyObject_DelItem(ns, name);
            if (err != 0) {
                format_exc_check_arg(tstate, PyExc_NameError,
                                     NAME_ERROR_MSG,
                                     name);
                goto error;
            }
            DISPATCH();
        }

        case TARGET(UNPACK_SEQUENCE): {
            PREDICTED(UNPACK_SEQUENCE);
            PyObject *seq = POP(), *item, **items;
            if (PyTuple_CheckExact(seq) &&
                PyTuple_GET_SIZE(seq) == oparg) {
                items = ((PyTupleObject *)seq)->ob_item;
                while (oparg--) {
                    item = items[oparg];
                    Py_INCREF(item);
                    PUSH(item);
                }
            } else if (PyList_CheckExact(seq) &&
                       PyList_GET_SIZE(seq) == oparg) {
                items = ((PyListObject *)seq)->ob_item;
                while (oparg--) {
                    item = items[oparg];
                    Py_INCREF(item);
                    PUSH(item);
                }
            } else if (unpack_iterable(tstate, seq, oparg, -1,
                                       stack_pointer + oparg)) {
                STACK_GROW(oparg);
            } else {
                /* unpack_iterable() raised an exception */
                Py_DECREF(seq);
                goto error;
            }
            Py_DECREF(seq);
            DISPATCH();
        }

        case TARGET(UNPACK_EX): {
            int totalargs = 1 + (oparg & 0xFF) + (oparg >> 8);
            PyObject *seq = POP();

            if (unpack_iterable(tstate, seq, oparg & 0xFF, oparg >> 8,
                                stack_pointer + totalargs)) {
                stack_pointer += totalargs;
            } else {
                Py_DECREF(seq);
                goto error;
            }
            Py_DECREF(seq);
            DISPATCH();
        }

        case TARGET(STORE_ATTR): {
            PyObject *name = GETITEM(names, oparg);
            PyObject *owner = TOP();
            PyObject *v = SECOND();
            int err;

            _PyOpcache *co_opcache;
            OPCACHE_CHECK();
            if (USE_STORE_ATTR_CACHE && co_opcache && likely(v)) {
                if (likely(storeAttrCache(owner, name, v, co_opcache, &err) == 0)) {
                    STACK_SHRINK(2);
                    goto sa_common;
                }
                ++co_opcache->num_failed;
            }
            STACK_SHRINK(2);
            err = PyObject_SetAttr(owner, name, v);

#if OPCACHE_STATS
            if (co_opcache)
                storeattr_misses++;
            else
                storeattr_noopcache++;
#endif

            if (USE_STORE_ATTR_CACHE && co_opcache && err == 0) {
                setupStoreAttrCache(owner, name, co_opcache);
            }

sa_common:
            Py_DECREF(v);
            Py_DECREF(owner);
            if (err != 0)
                goto error;
            DISPATCH();
        }

        case TARGET(DELETE_ATTR): {
            PyObject *name = GETITEM(names, oparg);
            PyObject *owner = POP();
            int err;
            err = PyObject_SetAttr(owner, name, (PyObject *)NULL);
            Py_DECREF(owner);
            if (err != 0)
                goto error;
            DISPATCH();
        }

        case TARGET(STORE_GLOBAL): {
            PyObject *name = GETITEM(names, oparg);
            PyObject *v = POP();
            int err;
            err = PyDict_SetItem(f->f_globals, name, v);
            Py_DECREF(v);
            if (err != 0)
                goto error;
            DISPATCH();
        }

        case TARGET(DELETE_GLOBAL): {
            PyObject *name = GETITEM(names, oparg);
            int err;
            err = PyDict_DelItem(f->f_globals, name);
            if (err != 0) {
                if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                    format_exc_check_arg(tstate, PyExc_NameError,
                                         NAME_ERROR_MSG, name);
                }
                goto error;
            }
            DISPATCH();
        }

        case TARGET(LOAD_NAME): {
            PyObject *name = GETITEM(names, oparg);
            PyObject *locals = f->f_locals;
            PyObject *v;
            if (locals == NULL) {
                _PyErr_Format(tstate, PyExc_SystemError,
                              "no locals when loading %R", name);
                goto error;
            }
            if (PyDict_CheckExact(locals)) {
                v = PyDict_GetItemWithError(locals, name);
                if (v != NULL) {
                    Py_INCREF(v);
                }
                else if (_PyErr_Occurred(tstate)) {
                    goto error;
                }
            }
            else {
                v = PyObject_GetItem(locals, name);
                if (v == NULL) {
                    if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError))
                        goto error;
                    _PyErr_Clear(tstate);
                }
            }
            if (v == NULL) {
                v = PyDict_GetItemWithError(f->f_globals, name);
                if (v != NULL) {
                    Py_INCREF(v);
                }
                else if (_PyErr_Occurred(tstate)) {
                    goto error;
                }
                else {
                    if (PyDict_CheckExact(f->f_builtins)) {
                        v = PyDict_GetItemWithError(f->f_builtins, name);
                        if (v == NULL) {
                            if (!_PyErr_Occurred(tstate)) {
                                format_exc_check_arg(
                                        tstate, PyExc_NameError,
                                        NAME_ERROR_MSG, name);
                            }
                            goto error;
                        }
                        Py_INCREF(v);
                    }
                    else {
                        v = PyObject_GetItem(f->f_builtins, name);
                        if (v == NULL) {
                            if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                                format_exc_check_arg(
                                            tstate, PyExc_NameError,
                                            NAME_ERROR_MSG, name);
                            }
                            goto error;
                        }
                    }
                }
            }
            PUSH(v);
            DISPATCH();
        }

        case TARGET(LOAD_GLOBAL): {
            PyObject *name;
            PyObject *v;
            if (PyDict_CheckExact(f->f_globals)
                && PyDict_CheckExact(f->f_builtins))
            {
                _PyOpcache *co_opcache;
                OPCACHE_CHECK();
                if (co_opcache != NULL && co_opcache->optimized > 0) {
                    _PyOpcache_LoadGlobal *lg = &co_opcache->u.lg;

                    if (lg->globals_ver ==
                            ((PyDictObject *)f->f_globals)->ma_version_tag
                        && (lg->builtins_ver == LOADGLOBAL_WAS_GLOBAL ||
                            lg->builtins_ver == ((PyDictObject *)f->f_builtins)->ma_version_tag))
                    {
#if OPCACHE_STATS
                        loadglobal_hits++;
#endif
                        co_opcache->num_failed = 0;
                        PyObject *ptr = lg->ptr;
                        OPCACHE_STAT_GLOBAL_HIT();
                        assert(ptr != NULL);
                        Py_INCREF(ptr);
                        PUSH(ptr);
                        DISPATCH();
                    }
#if OPCACHE_STATS
                    loadglobal_misses++;
#endif
                    co_opcache->num_failed++;
                }
#if OPCACHE_STATS
                else
                    loadglobal_noopcache++;
#endif

                name = GETITEM(names, oparg);
                int wasglobal;
                v = _PyDict_LoadGlobalEx((PyDictObject *)f->f_globals,
                                       (PyDictObject *)f->f_builtins,
                                       name, &wasglobal);
                if (v == NULL) {
                    if (!_PyErr_OCCURRED()) {
                        /* _PyDict_LoadGlobal() returns NULL without raising
                         * an exception if the key doesn't exist */
                        format_exc_check_arg(tstate, PyExc_NameError,
                                             NAME_ERROR_MSG, name);
                    }
                    goto error;
                }

                if (co_opcache != NULL) {
                    _PyOpcache_LoadGlobal *lg = &co_opcache->u.lg;

                    if (co_opcache->optimized == 0) {
                        /* Wasn't optimized before. */
                        OPCACHE_STAT_GLOBAL_OPT();
                    } else {
                        OPCACHE_STAT_GLOBAL_MISS();
                    }

                    co_opcache->optimized = 1;
                    lg->globals_ver =
                        ((PyDictObject *)f->f_globals)->ma_version_tag;
                    if (wasglobal)
                        lg->builtins_ver = LOADGLOBAL_WAS_GLOBAL;
                    else
                        lg->builtins_ver =
                            ((PyDictObject *)f->f_builtins)->ma_version_tag;
                    lg->ptr = v; /* borrowed */
                }

                Py_INCREF(v);
            }
            else {
                /* Slow-path if globals or builtins is not a dict */
#if OPCACHE_STATS
                loadglobal_uncached++;
#endif

                /* namespace 1: globals */
                name = GETITEM(names, oparg);
                v = PyObject_GetItem(f->f_globals, name);
                if (v == NULL) {
                    if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                        goto error;
                    }
                    _PyErr_Clear(tstate);

                    /* namespace 2: builtins */
                    v = PyObject_GetItem(f->f_builtins, name);
                    if (v == NULL) {
                        if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                            format_exc_check_arg(
                                        tstate, PyExc_NameError,
                                        NAME_ERROR_MSG, name);
                        }
                        goto error;
                    }
                }
            }
            PUSH(v);
            DISPATCH();
        }

        case TARGET(DELETE_FAST): {
            PyObject *v = GETLOCAL(oparg);
            if (v != NULL) {
                SETLOCAL(oparg, NULL);
                DISPATCH();
            }
            format_exc_check_arg(
                tstate, PyExc_UnboundLocalError,
                UNBOUNDLOCAL_ERROR_MSG,
                PyTuple_GetItem(co->co_varnames, oparg)
                );
            goto error;
        }

        case TARGET(DELETE_DEREF): {
            PyObject *cell = freevars[oparg];
            PyObject *oldobj = PyCell_GET(cell);
            if (oldobj != NULL) {
                PyCell_SET(cell, NULL);
                Py_DECREF(oldobj);
                DISPATCH();
            }
            format_exc_unbound(tstate, co, oparg);
            goto error;
        }

        case TARGET(LOAD_CLOSURE): {
            PyObject *cell = freevars[oparg];
            Py_INCREF(cell);
            PUSH(cell);
            DISPATCH();
        }

        case TARGET(LOAD_CLASSDEREF): {
            PyObject *name, *value, *locals = f->f_locals;
            Py_ssize_t idx;
            assert(locals);
            assert(oparg >= PyTuple_GET_SIZE(co->co_cellvars));
            idx = oparg - PyTuple_GET_SIZE(co->co_cellvars);
            assert(idx >= 0 && idx < PyTuple_GET_SIZE(co->co_freevars));
            name = PyTuple_GET_ITEM(co->co_freevars, idx);
            if (PyDict_CheckExact(locals)) {
                value = PyDict_GetItemWithError(locals, name);
                if (value != NULL) {
                    Py_INCREF(value);
                }
                else if (_PyErr_Occurred(tstate)) {
                    goto error;
                }
            }
            else {
                value = PyObject_GetItem(locals, name);
                if (value == NULL) {
                    if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                        goto error;
                    }
                    _PyErr_Clear(tstate);
                }
            }
            if (!value) {
                PyObject *cell = freevars[oparg];
                value = PyCell_GET(cell);
                if (value == NULL) {
                    format_exc_unbound(tstate, co, oparg);
                    goto error;
                }
                Py_INCREF(value);
            }
            PUSH(value);
            DISPATCH();
        }

        case TARGET(LOAD_DEREF): {
            PyObject *cell = freevars[oparg];
            PyObject *value = PyCell_GET(cell);
            if (value == NULL) {
                format_exc_unbound(tstate, co, oparg);
                goto error;
            }
            Py_INCREF(value);
            PUSH(value);
            DISPATCH();
        }

        case TARGET(STORE_DEREF): {
            PyObject *v = POP();
            PyObject *cell = freevars[oparg];
            PyObject *oldobj = PyCell_GET(cell);
            PyCell_SET(cell, v);
            Py_XDECREF(oldobj);
            DISPATCH();
        }

        case TARGET(BUILD_STRING): {
            PyObject *str;
            PyObject *empty = PyUnicode_New(0, 0);
            if (empty == NULL) {
                goto error;
            }
            str = _PyUnicode_JoinArray(empty, stack_pointer - oparg, oparg);
            Py_DECREF(empty);
            if (str == NULL)
                goto error;
            while (--oparg >= 0) {
                PyObject *item = POP();
                Py_DECREF(item);
            }
            PUSH(str);
            DISPATCH();
        }

        case TARGET(BUILD_TUPLE): {
            PyObject *tup = PyTuple_New_Nonzeroed(oparg);
            if (tup == NULL)
                goto error;
            while (--oparg >= 0) {
                PyObject *item = POP();
                PyTuple_SET_ITEM(tup, oparg, item);
            }
            PUSH(tup);
            DISPATCH();
        }

        case TARGET(BUILD_LIST): {
            PyObject *list =  PyList_New(oparg);
            if (list == NULL)
                goto error;
            while (--oparg >= 0) {
                PyObject *item = POP();
                PyList_SET_ITEM(list, oparg, item);
            }
            PUSH(list);
            DISPATCH();
        }

        case TARGET(BUILD_TUPLE_UNPACK_WITH_CALL):
        case TARGET(BUILD_TUPLE_UNPACK):
        case TARGET(BUILD_LIST_UNPACK): {
            int convert_to_tuple = opcode != BUILD_LIST_UNPACK;
            Py_ssize_t i;
            PyObject *sum = PyList_New(0);
            PyObject *return_value;

            if (sum == NULL)
                goto error;

            for (i = oparg; i > 0; i--) {
                PyObject *none_val;

                none_val = _PyList_Extend((PyListObject *)sum, PEEK(i));
                if (none_val == NULL) {
                    if (opcode == BUILD_TUPLE_UNPACK_WITH_CALL &&
                        _PyErr_ExceptionMatches(tstate, PyExc_TypeError))
                    {
                        check_args_iterable(tstate, PEEK(1 + oparg), PEEK(i));
                    }
                    Py_DECREF(sum);
                    goto error;
                }
                Py_DECREF(none_val);
            }

            if (convert_to_tuple) {
                return_value = PyList_AsTuple(sum);
                Py_DECREF(sum);
                if (return_value == NULL)
                    goto error;
            }
            else {
                return_value = sum;
            }

            while (oparg--)
                Py_DECREF(POP());
            PUSH(return_value);
            DISPATCH();
        }

        case TARGET(BUILD_SET): {
            PyObject *set = PySet_New(NULL);
            int err = 0;
            int i;
            if (set == NULL)
                goto error;
            for (i = oparg; i > 0; i--) {
                PyObject *item = PEEK(i);
                if (err == 0)
                    err = PySet_Add(set, item);
                Py_DECREF(item);
            }
            STACK_SHRINK(oparg);
            if (err != 0) {
                Py_DECREF(set);
                goto error;
            }
            PUSH(set);
            DISPATCH();
        }

        case TARGET(BUILD_SET_UNPACK): {
            Py_ssize_t i;
            PyObject *sum = PySet_New(NULL);
            if (sum == NULL)
                goto error;

            for (i = oparg; i > 0; i--) {
                if (_PySet_Update(sum, PEEK(i)) < 0) {
                    Py_DECREF(sum);
                    goto error;
                }
            }

            while (oparg--)
                Py_DECREF(POP());
            PUSH(sum);
            DISPATCH();
        }

        case TARGET(BUILD_MAP): {
            Py_ssize_t i;
            PyObject *map = _PyDict_NewPresized((Py_ssize_t)oparg);
            if (map == NULL)
                goto error;
            for (i = oparg; i > 0; i--) {
                int err;
                PyObject *key = PEEK(2*i);
                PyObject *value = PEEK(2*i - 1);
                err = PyDict_SetItem(map, key, value);
                if (err != 0) {
                    Py_DECREF(map);
                    goto error;
                }
            }

            while (oparg--) {
                Py_DECREF(POP());
                Py_DECREF(POP());
            }
            PUSH(map);
            DISPATCH();
        }

        case TARGET(SETUP_ANNOTATIONS): {
            _Py_IDENTIFIER(__annotations__);
            int err;
            PyObject *ann_dict;
            if (f->f_locals == NULL) {
                _PyErr_Format(tstate, PyExc_SystemError,
                              "no locals found when setting up annotations");
                goto error;
            }
            /* check if __annotations__ in locals()... */
            if (PyDict_CheckExact(f->f_locals)) {
                ann_dict = _PyDict_GetItemIdWithError(f->f_locals,
                                             &PyId___annotations__);
                if (ann_dict == NULL) {
                    if (_PyErr_Occurred(tstate)) {
                        goto error;
                    }
                    /* ...if not, create a new one */
                    ann_dict = PyDict_New();
                    if (ann_dict == NULL) {
                        goto error;
                    }
                    err = _PyDict_SetItemId(f->f_locals,
                                            &PyId___annotations__, ann_dict);
                    Py_DECREF(ann_dict);
                    if (err != 0) {
                        goto error;
                    }
                }
            }
            else {
                /* do the same if locals() is not a dict */
                PyObject *ann_str = _PyUnicode_FromId(&PyId___annotations__);
                if (ann_str == NULL) {
                    goto error;
                }
                ann_dict = PyObject_GetItem(f->f_locals, ann_str);
                if (ann_dict == NULL) {
                    if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                        goto error;
                    }
                    _PyErr_Clear(tstate);
                    ann_dict = PyDict_New();
                    if (ann_dict == NULL) {
                        goto error;
                    }
                    err = PyObject_SetItem(f->f_locals, ann_str, ann_dict);
                    Py_DECREF(ann_dict);
                    if (err != 0) {
                        goto error;
                    }
                }
                else {
                    Py_DECREF(ann_dict);
                }
            }
            DISPATCH();
        }

        case TARGET(BUILD_CONST_KEY_MAP): {
            Py_ssize_t i;
            PyObject *map;
            PyObject *keys = TOP();
            if (!PyTuple_CheckExact(keys) ||
                PyTuple_GET_SIZE(keys) != (Py_ssize_t)oparg) {
                _PyErr_SetString(tstate, PyExc_SystemError,
                                 "bad BUILD_CONST_KEY_MAP keys argument");
                goto error;
            }
            map = _PyDict_NewPresized((Py_ssize_t)oparg);
            if (map == NULL) {
                goto error;
            }
            for (i = oparg; i > 0; i--) {
                int err;
                PyObject *key = PyTuple_GET_ITEM(keys, oparg - i);
                PyObject *value = PEEK(i + 1);
                err = PyDict_SetItem(map, key, value);
                if (err != 0) {
                    Py_DECREF(map);
                    goto error;
                }
            }

            Py_DECREF(POP());
            while (oparg--) {
                Py_DECREF(POP());
            }
            PUSH(map);
            DISPATCH();
        }

        case TARGET(BUILD_MAP_UNPACK): {
            Py_ssize_t i;
            PyObject *sum = PyDict_New();
            if (sum == NULL)
                goto error;

            for (i = oparg; i > 0; i--) {
                PyObject *arg = PEEK(i);
                if (PyDict_Update(sum, arg) < 0) {
                    if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
                        _PyErr_Format(tstate, PyExc_TypeError,
                                      "'%.200s' object is not a mapping",
                                      arg->ob_type->tp_name);
                    }
                    Py_DECREF(sum);
                    goto error;
                }
            }

            while (oparg--)
                Py_DECREF(POP());
            PUSH(sum);
            DISPATCH();
        }

        case TARGET(BUILD_MAP_UNPACK_WITH_CALL): {
            Py_ssize_t i;
            PyObject *sum = PyDict_New();
            if (sum == NULL)
                goto error;

            for (i = oparg; i > 0; i--) {
                PyObject *arg = PEEK(i);
                if (_PyDict_MergeEx(sum, arg, 2) < 0) {
                    Py_DECREF(sum);
                    format_kwargs_error(tstate, PEEK(2 + oparg), arg);
                    goto error;
                }
            }

            while (oparg--)
                Py_DECREF(POP());
            PUSH(sum);
            DISPATCH();
        }

        case TARGET(MAP_ADD): {
            PyObject *value = TOP();
            PyObject *key = SECOND();
            PyObject *map;
            int err;
            STACK_SHRINK(2);
            map = PEEK(oparg);                      /* dict */
            assert(PyDict_CheckExact(map));
            err = PyDict_SetItem(map, key, value);  /* map[key] = value */
            Py_DECREF(value);
            Py_DECREF(key);
            if (err != 0)
                goto error;
            PREDICT(JUMP_ABSOLUTE);
            DISPATCH();
        }

        case TARGET(LOAD_ATTR): {
            PyObject *name = GETITEM(names, oparg);
            PyObject *owner = TOP();
            PyObject *res;

            _PyOpcache *co_opcache;
            OPCACHE_CHECK();
            if (USE_LOAD_ATTR_CACHE && co_opcache) {
                if (likely(loadAttrCache(owner, name, co_opcache, &res, NULL) == 0))
                    goto la_common;
                ++co_opcache->num_failed;
            }


            res = PyObject_GetAttr(owner, name);

#if OPCACHE_STATS
            if (co_opcache)
                loadattr_misses++;
            else
                loadattr_noopcache++;
#endif

            if (USE_LOAD_ATTR_CACHE && co_opcache && res) {
                setupLoadAttrCache(owner, name, co_opcache, res, 0 /*= not LOAD_METHOD*/, 1 /*inside interpreter*/);
            }
la_common:
            Py_DECREF(owner);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(COMPARE_OP): {
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *res = cmp_outcome(tstate, oparg, left, right);
            Py_DECREF(left);
            Py_DECREF(right);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            PREDICT(POP_JUMP_IF_FALSE);
            PREDICT(POP_JUMP_IF_TRUE);
            DISPATCH();
        }

        case TARGET(IMPORT_NAME): {
            PyObject *name = GETITEM(names, oparg);
            PyObject *fromlist = POP();
            PyObject *level = TOP();
            PyObject *res;
            res = import_name(tstate, f, name, fromlist, level);
            Py_DECREF(level);
            Py_DECREF(fromlist);
            SET_TOP(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(IMPORT_STAR): {
            PyObject *from = POP(), *locals;
            int err;
            if (PyFrame_FastToLocalsWithError(f) < 0) {
                Py_DECREF(from);
                goto error;
            }

            locals = f->f_locals;
            if (locals == NULL) {
                _PyErr_SetString(tstate, PyExc_SystemError,
                                 "no locals found during 'import *'");
                Py_DECREF(from);
                goto error;
            }
            err = import_all_from(tstate, locals, from);
            PyFrame_LocalsToFast(f, 0);
            Py_DECREF(from);
            if (err != 0)
                goto error;
            DISPATCH();
        }

        case TARGET(IMPORT_FROM): {
            PyObject *name = GETITEM(names, oparg);
            PyObject *from = TOP();
            PyObject *res;
            res = import_from(tstate, from, name);
            PUSH(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(JUMP_FORWARD): {
            JUMPBY(oparg);
            FAST_DISPATCH();
        }

        case TARGET(POP_JUMP_IF_FALSE): {
            PREDICTED(POP_JUMP_IF_FALSE);
            PyObject *cond = POP();
            int err;
            if (cond == Py_True) {
                Py_DECREF_IMMORTAL(cond);
                FAST_DISPATCH();
            }
            if (cond == Py_False) {
                Py_DECREF_IMMORTAL(cond);
                JUMPTO_WITH_OSR(oparg);
                FAST_DISPATCH();
            }
            err = PyObject_IsTrue(cond);
            Py_DECREF(cond);
            if (err > 0)
                ;
            else if (err == 0)
                JUMPTO_WITH_OSR(oparg);
            else
                goto error;
            DISPATCH();
        }

        case TARGET(POP_JUMP_IF_TRUE): {
            PREDICTED(POP_JUMP_IF_TRUE);
            PyObject *cond = POP();
            int err;
            if (cond == Py_False) {
                Py_DECREF_IMMORTAL(cond);
                FAST_DISPATCH();
            }
            if (cond == Py_True) {
                Py_DECREF_IMMORTAL(cond);
                JUMPTO_WITH_OSR(oparg);
                FAST_DISPATCH();
            }
            err = PyObject_IsTrue(cond);
            Py_DECREF(cond);
            if (err > 0) {
                JUMPTO_WITH_OSR(oparg);
            }
            else if (err == 0)
                ;
            else
                goto error;
            DISPATCH();
        }

        case TARGET(JUMP_IF_FALSE_OR_POP): {
            PyObject *cond = TOP();
            int err;
            if (cond == Py_True) {
                STACK_SHRINK(1);
                Py_DECREF_IMMORTAL(cond);
                FAST_DISPATCH();
            }
            if (cond == Py_False) {
                JUMPTO_WITH_OSR(oparg);
                FAST_DISPATCH();
            }
            err = PyObject_IsTrue(cond);
            if (err > 0) {
                STACK_SHRINK(1);
                Py_DECREF(cond);
            }
            else if (err == 0)
                JUMPTO_WITH_OSR(oparg);
            else
                goto error;
            DISPATCH();
        }

        case TARGET(JUMP_IF_TRUE_OR_POP): {
            PyObject *cond = TOP();
            int err;
            if (cond == Py_False) {
                STACK_SHRINK(1);
                Py_DECREF_IMMORTAL(cond);
                FAST_DISPATCH();
            }
            if (cond == Py_True) {
                JUMPTO_WITH_OSR(oparg);
                FAST_DISPATCH();
            }
            err = PyObject_IsTrue(cond);
            if (err > 0) {
                JUMPTO_WITH_OSR(oparg);
            }
            else if (err == 0) {
                STACK_SHRINK(1);
                Py_DECREF(cond);
            }
            else
                goto error;
            DISPATCH();
        }

        case TARGET(JUMP_ABSOLUTE): {
            PREDICTED(JUMP_ABSOLUTE);
            JUMPTO(oparg);

            HANDLE_JUMP_BACKWARD_OSR();

#if FAST_LOOPS
            /* Enabling this path speeds-up all while and for-loops by bypassing
               the per-loop checks for signals.  By default, this should be turned-off
               because it prevents detection of a control-break in tight loops like
               "while 1: pass".  Compile with this option turned-on when you need
               the speed-up and do not need break checking inside tight loops (ones
               that contain only instructions ending with FAST_DISPATCH).
            */
            FAST_DISPATCH();
#else
            DISPATCH();
#endif
        }

        case TARGET(GET_ITER): {
            /* before: [obj]; after [getiter(obj)] */
            PyObject *iterable = TOP();
            PyObject *iter = PyObject_GetIter(iterable);
            Py_DECREF(iterable);
            SET_TOP(iter);
            if (iter == NULL)
                goto error;
            PREDICT(FOR_ITER);
            PREDICT(CALL_FUNCTION);
            DISPATCH();
        }

        case TARGET(GET_YIELD_FROM_ITER): {
            /* before: [obj]; after [getiter(obj)] */
            PyObject *iterable = TOP();
            PyObject *iter;
            if (PyCoro_CheckExact(iterable)) {
                /* `iterable` is a coroutine */
                if (!(co->co_flags & (CO_COROUTINE | CO_ITERABLE_COROUTINE))) {
                    /* and it is used in a 'yield from' expression of a
                       regular generator. */
                    Py_DECREF(iterable);
                    SET_TOP(NULL);
                    _PyErr_SetString(tstate, PyExc_TypeError,
                                     "cannot 'yield from' a coroutine object "
                                     "in a non-coroutine generator");
                    goto error;
                }
            }
            else if (!PyGen_CheckExact(iterable)) {
                /* `iterable` is not a generator. */
                iter = PyObject_GetIter(iterable);
                Py_DECREF(iterable);
                SET_TOP(iter);
                if (iter == NULL)
                    goto error;
            }
            PREDICT(LOAD_CONST);
            DISPATCH();
        }

        case TARGET(FOR_ITER): {
            PREDICTED(FOR_ITER);
            /* before: [iter]; after: [iter, iter()] *or* [] */
            PyObject *iter = TOP();
            PyObject *next = (*iter->ob_type->tp_iternext)(iter);
            if (next != NULL) {
                PUSH(next);
                PREDICT(STORE_FAST);
                PREDICT(UNPACK_SEQUENCE);
                DISPATCH();
            }
            if (_PyErr_Occurred(tstate)) {
                if (!_PyErr_ExceptionMatches(tstate, PyExc_StopIteration)) {
                    goto error;
                }
                else if (tstate->c_tracefunc != NULL) {
                    call_exc_trace(tstate->c_tracefunc, tstate->c_traceobj, tstate, f);
                }
                _PyErr_Clear(tstate);
            }
            /* iterator ended normally */
            STACK_SHRINK(1);
            Py_DECREF(iter);
            JUMPBY(oparg);
            PREDICT(POP_BLOCK);
            DISPATCH();
        }

        case TARGET(SETUP_FINALLY): {
            /* NOTE: If you add any new block-setup opcodes that
               are not try/except/finally handlers, you may need
               to update the PyGen_NeedsFinalizing() function.
               */

            PyFrame_BlockSetup(f, SETUP_FINALLY, INSTR_OFFSET() + oparg,
                               STACK_LEVEL());
            DISPATCH();
        }

        case TARGET(BEFORE_ASYNC_WITH): {
            _Py_IDENTIFIER(__aexit__);
            _Py_IDENTIFIER(__aenter__);

            PyObject *mgr = TOP();
            PyObject *exit = special_lookup(tstate, mgr, &PyId___aexit__),
                     *enter;
            PyObject *res;
            if (exit == NULL)
                goto error;
            SET_TOP(exit);
            enter = special_lookup(tstate, mgr, &PyId___aenter__);
            Py_DECREF(mgr);
            if (enter == NULL)
                goto error;
            res = _PyObject_CallNoArg(enter);
            Py_DECREF(enter);
            if (res == NULL)
                goto error;
            PUSH(res);
            PREDICT(GET_AWAITABLE);
            DISPATCH();
        }

        case TARGET(SETUP_ASYNC_WITH): {
            PyObject *res = POP();
            /* Setup the finally block before pushing the result
               of __aenter__ on the stack. */
            PyFrame_BlockSetup(f, SETUP_FINALLY, INSTR_OFFSET() + oparg,
                               STACK_LEVEL());
            PUSH(res);
            DISPATCH();
        }

        case TARGET(SETUP_WITH): {
            _Py_IDENTIFIER(__exit__);
            _Py_IDENTIFIER(__enter__);
            PyObject *mgr = TOP();
            PyObject *enter = special_lookup(tstate, mgr, &PyId___enter__);
            PyObject *res;
            if (enter == NULL) {
                goto error;
            }
            PyObject *exit = special_lookup(tstate, mgr, &PyId___exit__);
            if (exit == NULL) {
                Py_DECREF(enter);
                goto error;
            }
            SET_TOP(exit);
            Py_DECREF(mgr);
            res = _PyObject_CallNoArg(enter);
            Py_DECREF(enter);
            if (res == NULL)
                goto error;
            /* Setup the finally block before pushing the result
               of __enter__ on the stack. */
            PyFrame_BlockSetup(f, SETUP_FINALLY, INSTR_OFFSET() + oparg,
                               STACK_LEVEL());

            PUSH(res);
            DISPATCH();
        }

        case TARGET(WITH_CLEANUP_START): {
            /* At the top of the stack are 1 or 6 values indicating
               how/why we entered the finally clause:
               - TOP = NULL
               - (TOP, SECOND, THIRD) = exc_info()
                 (FOURTH, FITH, SIXTH) = previous exception for EXCEPT_HANDLER
               Below them is EXIT, the context.__exit__ or context.__aexit__
               bound method.
               In the first case, we must call
                 EXIT(None, None, None)
               otherwise we must call
                 EXIT(TOP, SECOND, THIRD)

               In the first case, we remove EXIT from the
               stack, leaving TOP, and push TOP on the stack.
               Otherwise we shift the bottom 3 values of the
               stack down, replace the empty spot with NULL, and push
               None on the stack.

               Finally we push the result of the call.
            */
            PyObject *stack[3];
            PyObject *exit_func;
            PyObject *exc, *val, *tb, *res;

            val = tb = Py_None;
            exc = TOP();
            if (exc == NULL) {
                STACK_SHRINK(1);
                exit_func = TOP();
                SET_TOP(exc);
                exc = Py_None;
            }
            else {
                assert(PyExceptionClass_Check(exc));
                PyObject *tp2, *exc2, *tb2;
                PyTryBlock *block;
                val = SECOND();
                tb = THIRD();
                tp2 = FOURTH();
                exc2 = PEEK(5);
                tb2 = PEEK(6);
                exit_func = PEEK(7);
                SET_VALUE(7, tb2);
                SET_VALUE(6, exc2);
                SET_VALUE(5, tp2);
                /* UNWIND_EXCEPT_HANDLER will pop this off. */
                SET_FOURTH(NULL);
                /* We just shifted the stack down, so we have
                   to tell the except handler block that the
                   values are lower than it expects. */
                assert(f->f_iblock > 0);
                block = &f->f_blockstack[f->f_iblock - 1];
                assert(block->b_type == EXCEPT_HANDLER);
                assert(block->b_level > 0);
                block->b_level--;
            }

            stack[0] = exc;
            stack[1] = val;
            stack[2] = tb;
            res = _PyObject_FastCall(exit_func, stack, 3);
            Py_DECREF(exit_func);
            if (res == NULL)
                goto error;

            Py_INCREF(exc); /* Duplicating the exception on the stack */
            PUSH(exc);
            PUSH(res);
            PREDICT(WITH_CLEANUP_FINISH);
            DISPATCH();
        }

        case TARGET(WITH_CLEANUP_FINISH): {
            PREDICTED(WITH_CLEANUP_FINISH);
            /* TOP = the result of calling the context.__exit__ bound method
               SECOND = either None or exception type

               If SECOND is None below is NULL or the return address,
               otherwise below are 7 values representing an exception.
            */
            PyObject *res = POP();
            PyObject *exc = POP();
            int err;

            if (exc != Py_None)
                err = PyObject_IsTrue(res);
            else
                err = 0;

            Py_DECREF(res);
            Py_DECREF(exc);

            if (err < 0)
                goto error;
            else if (err > 0) {
                /* There was an exception and a True return.
                 * We must manually unwind the EXCEPT_HANDLER block
                 * which was created when the exception was caught,
                 * otherwise the stack will be in an inconsistent state.
                 */
                PyTryBlock *b = PyFrame_BlockPop(f);
                assert(b->b_type == EXCEPT_HANDLER);
                UNWIND_EXCEPT_HANDLER(b);
                PUSH(NULL);
            }
            PREDICT(END_FINALLY);
            DISPATCH();
        }

        case TARGET(LOAD_METHOD): {
            /* Designed to work in tandem with CALL_METHOD. */
            PyObject *name = GETITEM(names, oparg);
            PyObject *obj = TOP();
            PyObject *meth = NULL;

            _PyOpcache *co_opcache;
            OPCACHE_CHECK();

            int is_method;
            if (USE_LOAD_METHOD_CACHE && co_opcache) {
                if (likely(loadAttrCache(obj, name, co_opcache, &meth, &is_method) == 0)) {
                    if (meth == NULL) {
                        goto error;
                    }
                    if (is_method) {
                        SET_TOP(meth);
                        PUSH(obj);
                    } else {
                        SET_TOP(NULL);
                        Py_DECREF(obj);
                        PUSH(meth);
                    }
                    goto lm_before_dispatch;
                }
                ++co_opcache->num_failed;
            }
            meth = NULL;

            int meth_found = _PyObject_GetMethod(obj, name, &meth);

#if OPCACHE_STATS
            if (co_opcache)
                loadmethod_misses++;
            else
                loadmethod_noopcache++;
#endif

            if (meth == NULL) {
                /* Most likely attribute wasn't found. */
                goto error;
            }

            if (USE_LOAD_METHOD_CACHE && co_opcache) {
                setupLoadAttrCache(obj, name, co_opcache, meth, 1 /*= LOAD_METHOD*/, 1 /*inside interpreter*/);
            }

            if (meth_found) {
                /* We can bypass temporary bound method object.
                   meth is unbound method and obj is self.

                   meth | self | arg1 | ... | argN
                 */
                SET_TOP(meth);
                PUSH(obj);  // self
            }
            else {
                /* meth is not an unbound method (but a regular attr, or
                   something was returned by a descriptor protocol).  Set
                   the second element of the stack to NULL, to signal
                   CALL_METHOD that it's not a method call.

                   NULL | meth | arg1 | ... | argN
                */
                SET_TOP(NULL);
                Py_DECREF(obj);
                PUSH(meth);
            }
lm_before_dispatch:
            DISPATCH();
        }

        case TARGET(CALL_METHOD): {
            /* Designed to work in tamdem with LOAD_METHOD. */
            PyObject **sp, *res, *meth;

            sp = stack_pointer;

            meth = PEEK(oparg + 2);
            if (meth == NULL) {
                /* `meth` is NULL when LOAD_METHOD thinks that it's not
                   a method call.

                   Stack layout:

                       ... | NULL | callable | arg1 | ... | argN
                                                            ^- TOP()
                                               ^- (-oparg)
                                    ^- (-oparg-1)
                             ^- (-oparg-2)

                   `callable` will be POPed by call_function.
                   NULL will will be POPed manually later.
                */
                res = call_function_ceval(tstate, &sp, oparg, NULL);
                stack_pointer = sp;
                (void)POP(); /* POP the NULL. */
            }
            else {
                /* This is a method call.  Stack layout:

                     ... | method | self | arg1 | ... | argN
                                                        ^- TOP()
                                           ^- (-oparg)
                                    ^- (-oparg-1)
                           ^- (-oparg-2)

                  `self` and `method` will be POPed by call_function.
                  We'll be passing `oparg + 1` to call_function, to
                  make it accept the `self` as a first argument.
                */
                res = call_function_ceval(tstate, &sp, oparg + 1, NULL);
                stack_pointer = sp;
            }

            PUSH(res);
            if (res == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(CALL_FUNCTION): {
            PREDICTED(CALL_FUNCTION);
            PyObject **sp, *res;
            sp = stack_pointer;
            res = call_function_ceval(tstate, &sp, oparg, NULL);
            stack_pointer = sp;
            PUSH(res);
            if (res == NULL) {
                goto error;
            }
            DISPATCH();
        }

        case TARGET(CALL_FUNCTION_KW): {
            PyObject **sp, *res, *names;

            names = POP();
            assert(PyTuple_CheckExact(names) && PyTuple_GET_SIZE(names) <= oparg);
            sp = stack_pointer;
            res = call_function_ceval(tstate, &sp, oparg, names);
            stack_pointer = sp;
            PUSH(res);
            Py_DECREF(names);

            if (res == NULL) {
                goto error;
            }
            DISPATCH();
        }

        case TARGET(CALL_FUNCTION_EX): {
            PyObject *func, *callargs, *kwargs = NULL, *result;
            if (oparg & 0x01) {
                kwargs = POP();
                if (!PyDict_CheckExact(kwargs)) {
                    PyObject *d = PyDict_New();
                    if (d == NULL)
                        goto error;
                    if (_PyDict_MergeEx(d, kwargs, 2) < 0) {
                        Py_DECREF(d);
                        format_kwargs_error(tstate, SECOND(), kwargs);
                        Py_DECREF(kwargs);
                        goto error;
                    }
                    Py_DECREF(kwargs);
                    kwargs = d;
                }
                assert(PyDict_CheckExact(kwargs));
            }
            callargs = POP();
            func = TOP();
            if (!PyTuple_CheckExact(callargs)) {
                if (check_args_iterable(tstate, func, callargs) < 0) {
                    Py_DECREF(callargs);
                    goto error;
                }
                Py_SETREF(callargs, PySequence_Tuple(callargs));
                if (callargs == NULL) {
                    goto error;
                }
            }
            assert(PyTuple_CheckExact(callargs));

            result = do_call_core(tstate, func, callargs, kwargs);
            Py_DECREF(func);
            Py_DECREF(callargs);
            Py_XDECREF(kwargs);

            SET_TOP(result);
            if (result == NULL) {
                goto error;
            }
            DISPATCH();
        }

        case TARGET(MAKE_FUNCTION): {
            PyObject *qualname = POP();
            PyObject *codeobj = POP();
            PyFunctionObject *func = (PyFunctionObject *)
                PyFunction_NewWithQualName(codeobj, f->f_globals, qualname);

            Py_DECREF(codeobj);
            Py_DECREF(qualname);
            if (func == NULL) {
                goto error;
            }

            if (oparg & 0x08) {
                assert(PyTuple_CheckExact(TOP()));
                func ->func_closure = POP();
            }
            if (oparg & 0x04) {
                assert(PyDict_CheckExact(TOP()));
                func->func_annotations = POP();
            }
            if (oparg & 0x02) {
                assert(PyDict_CheckExact(TOP()));
                func->func_kwdefaults = POP();
            }
            if (oparg & 0x01) {
                assert(PyTuple_CheckExact(TOP()));
                func->func_defaults = POP();
            }

            PUSH((PyObject *)func);
            DISPATCH();
        }

        case TARGET(BUILD_SLICE): {
            PyObject *start, *stop, *step, *slice;
            if (oparg == 3)
                step = POP();
            else
                step = NULL;
            stop = POP();
            start = TOP();
            slice = PySlice_New(start, stop, step);
            Py_DECREF(start);
            Py_DECREF(stop);
            Py_XDECREF(step);
            SET_TOP(slice);
            if (slice == NULL)
                goto error;
            DISPATCH();
        }

        case TARGET(FORMAT_VALUE): {
            /* Handles f-string value formatting. */
            PyObject *result;
            PyObject *fmt_spec;
            PyObject *value;
            PyObject *(*conv_fn)(PyObject *);
            int which_conversion = oparg & FVC_MASK;
            int have_fmt_spec = (oparg & FVS_MASK) == FVS_HAVE_SPEC;

            fmt_spec = have_fmt_spec ? POP() : NULL;
            value = POP();

            /* See if any conversion is specified. */
            switch (which_conversion) {
            case FVC_NONE:  conv_fn = NULL;           break;
            case FVC_STR:   conv_fn = PyObject_Str;   break;
            case FVC_REPR:  conv_fn = PyObject_Repr;  break;
            case FVC_ASCII: conv_fn = PyObject_ASCII; break;
            default:
                _PyErr_Format(tstate, PyExc_SystemError,
                              "unexpected conversion flag %d",
                              which_conversion);
                goto error;
            }

            /* If there's a conversion function, call it and replace
               value with that result. Otherwise, just use value,
               without conversion. */
            if (conv_fn != NULL) {
                result = conv_fn(value);
                Py_DECREF(value);
                if (result == NULL) {
                    Py_XDECREF(fmt_spec);
                    goto error;
                }
                value = result;
            }

            /* If value is a unicode object, and there's no fmt_spec,
               then we know the result of format(value) is value
               itself. In that case, skip calling format(). I plan to
               move this optimization in to PyObject_Format()
               itself. */
            if (PyUnicode_CheckExact(value) && fmt_spec == NULL) {
                /* Do nothing, just transfer ownership to result. */
                result = value;
            } else {
                /* Actually call format(). */
                result = PyObject_Format(value, fmt_spec);
                Py_DECREF(value);
                Py_XDECREF(fmt_spec);
                if (result == NULL) {
                    goto error;
                }
            }

            PUSH(result);
            DISPATCH();
        }

        case TARGET(EXTENDED_ARG): {
            int oldoparg = oparg;
            NEXTOPARG();
            oparg |= oldoparg << 8;
            goto dispatch_opcode;
        }

#if USE_COMPUTED_GOTOS
        _unknown_opcode:
#endif
        default:
            fprintf(stderr,
                "XXX lineno: %d, opcode: %d\n",
                PyFrame_GetLineNumber(f),
                opcode);
            _PyErr_SetString(tstate, PyExc_SystemError, "unknown opcode");
            goto error;

        } /* switch */

        /* This should never be reached. Every opcode should end with DISPATCH()
           or goto error. */
        Py_UNREACHABLE();

error:
        /* Double-check exception status. */
#ifdef NDEBUG
        if (!_PyErr_Occurred(tstate)) {
            _PyErr_SetString(tstate, PyExc_SystemError,
                             "error return without exception set");
        }
#else
        assert(_PyErr_Occurred(tstate));
#endif

        /* Log traceback info. */
        PyTraceBack_Here(f);

        if (tstate->c_tracefunc != NULL)
            call_exc_trace(tstate->c_tracefunc, tstate->c_traceobj,
                           tstate, f);

exception_unwind:
        /* Unwind stacks if an exception occurred */
        while (f->f_iblock > 0) {
            /* Pop the current block. */
            PyTryBlock *b = &f->f_blockstack[--f->f_iblock];

            if (b->b_type == EXCEPT_HANDLER) {
                UNWIND_EXCEPT_HANDLER(b);
                continue;
            }
            UNWIND_BLOCK(b);
            if (b->b_type == SETUP_FINALLY) {
                PyObject *exc, *val, *tb;
                int handler = b->b_handler;
                _PyErr_StackItem *exc_info = tstate->exc_info;
                /* Beware, this invalidates all b->b_* fields */
                PyFrame_BlockSetup(f, EXCEPT_HANDLER, -1, STACK_LEVEL());
                PUSH(exc_info->exc_traceback);
                PUSH(exc_info->exc_value);
                if (exc_info->exc_type != NULL) {
                    PUSH(exc_info->exc_type);
                }
                else {
                    Py_INCREF_IMMORTAL(Py_None);
                    PUSH(Py_None);
                }
                _PyErr_Fetch(tstate, &exc, &val, &tb);
                /* Make the raw exception data
                   available to the handler,
                   so a program can emulate the
                   Python main loop. */
                _PyErr_NormalizeException(tstate, &exc, &val, &tb);
                if (tb != NULL)
                    PyException_SetTraceback(val, tb);
                else
                    PyException_SetTraceback(val, Py_None);
                Py_INCREF(exc);
                exc_info->exc_type = exc;
                Py_INCREF(val);
                exc_info->exc_value = val;
                exc_info->exc_traceback = tb;
                if (tb == NULL) {
                    tb = Py_None;
                    Py_INCREF_IMMORTAL(tb);
                } else {
                    Py_INCREF(tb);
                }
                PUSH(tb);
                PUSH(val);
                PUSH(exc);

                JUMPTO(handler);
                /* Resume normal execution */
                goto main_loop;
            }
        } /* unwind stack */

        /* End the loop as we still have an error */
        break;
    } /* main loop */

    assert(retval == NULL);
    assert(_PyErr_Occurred(tstate));

exit_returning:

    /* Pop remaining stack entries. */
    while (!EMPTY()) {
        PyObject *o = POP();
        Py_XDECREF(o);
    }

exit_yielding:
    if (tstate->use_tracing) {
        if (tstate->c_tracefunc) {
            if (call_trace_protected(tstate->c_tracefunc, tstate->c_traceobj,
                                     tstate, f, PyTrace_RETURN, retval)) {
                Py_CLEAR(retval);
            }
        }
        if (tstate->c_profilefunc) {
            if (call_trace_protected(tstate->c_profilefunc, tstate->c_profileobj,
                                     tstate, f, PyTrace_RETURN, retval)) {
                Py_CLEAR(retval);
            }
        }
    }

    return retval;
}

static PyObject* _Py_HOT_FUNCTION
_PyEval_EvalFrame_AOT_JIT(PyFrameObject *f, PyThreadState * const tstate, PyObject** stack_pointer, JitFunc jit_code)
{
    PyObject* retval = NULL;

continue_jit:
    {
        JitRetVal ret = jit_code(f, tstate, stack_pointer);
        stack_pointer = ret.stack_pointer;
        int lower_bits = ret.ret_val & 3;
        if (lower_bits == 0) {
            retval = (PyObject*)ret.ret_val;
            if (retval)
                goto exit_returning;
            goto error;
        } else if (lower_bits == 1)
            goto exception_unwind;
        else if (lower_bits == 2) {
            retval = (PyObject*)(ret.ret_val & ~3);
            goto exit_yielding;
        } else { // lower_bits == 3
            // this is a deopt

            // we have to adjust back the last bytecode because the interpreter
            // will start at the next bytecode after f_lasti (so would skip one)
            if (f->f_lasti == 0)
                f->f_lasti = -1; // special case: we deopted on the first opcode, set it to -1 which is the func entry
            else
                f->f_lasti -= 2; // normal case: decrement one opcode

            int jit_first_trace_for_line = (PyObject*)(ret.ret_val & ~3) ? 1 : 0;
            return _PyEval_EvalFrame_AOT_Interpreter(f, 0 /* throwflag */, tstate, stack_pointer, 0 /*= can't use jit */, jit_first_trace_for_line);
        }
    }

error:
    /* Double-check exception status. */
#ifdef NDEBUG
    if (!_PyErr_Occurred(tstate)) {
        _PyErr_SetString(tstate, PyExc_SystemError,
                            "error return without exception set");
    }
#else
    assert(_PyErr_Occurred(tstate));
#endif

    /* Log traceback info. */
    PyTraceBack_Here(f);

    if (tstate->c_tracefunc != NULL)
        call_exc_trace(tstate->c_tracefunc, tstate->c_traceobj,
                        tstate, f);

exception_unwind:
    /* Unwind stacks if an exception occurred */
    while (f->f_iblock > 0) {
        /* Pop the current block. */
        PyTryBlock *b = &f->f_blockstack[--f->f_iblock];

        if (b->b_type == EXCEPT_HANDLER) {
            UNWIND_EXCEPT_HANDLER(b);
            continue;
        }
        UNWIND_BLOCK(b);
        if (b->b_type == SETUP_FINALLY) {
            PyObject *exc, *val, *tb;
            int handler = b->b_handler;
            _PyErr_StackItem *exc_info = tstate->exc_info;
            /* Beware, this invalidates all b->b_* fields */
            PyFrame_BlockSetup(f, EXCEPT_HANDLER, -1, STACK_LEVEL());
            PUSH(exc_info->exc_traceback);
            PUSH(exc_info->exc_value);
            if (exc_info->exc_type != NULL) {
                PUSH(exc_info->exc_type);
            }
            else {
                Py_INCREF_IMMORTAL(Py_None);
                PUSH(Py_None);
            }
            _PyErr_Fetch(tstate, &exc, &val, &tb);
            /* Make the raw exception data
                available to the handler,
                so a program can emulate the
                Python main loop. */
            _PyErr_NormalizeException(tstate, &exc, &val, &tb);
            if (tb != NULL)
                PyException_SetTraceback(val, tb);
            else
                PyException_SetTraceback(val, Py_None);
            Py_INCREF(exc);
            exc_info->exc_type = exc;
            Py_INCREF(val);
            exc_info->exc_value = val;
            exc_info->exc_traceback = tb;
            if (tb == NULL) {
                tb = Py_None;
                Py_INCREF_IMMORTAL(tb);
            } else {
                Py_INCREF(tb);
            }
            PUSH(tb);
            PUSH(val);
            PUSH(exc);

            f->f_lasti = handler - 2;
            /* Resume normal execution */
            goto continue_jit;
        }
    } /* unwind stack */



    assert(retval == NULL);
    assert(_PyErr_Occurred(tstate));

exit_returning:

    /* Pop remaining stack entries. */
    while (!EMPTY()) {
        PyObject *o = POP();
        Py_XDECREF(o);
    }

exit_yielding:
    if (tstate->use_tracing) {
        if (tstate->c_tracefunc) {
            if (call_trace_protected(tstate->c_tracefunc, tstate->c_traceobj,
                                     tstate, f, PyTrace_RETURN, retval)) {
                Py_CLEAR(retval);
            }
        }
        if (tstate->c_profilefunc) {
            if (call_trace_protected(tstate->c_profilefunc, tstate->c_profileobj,
                                     tstate, f, PyTrace_RETURN, retval)) {
                Py_CLEAR(retval);
            }
        }
    }

    return retval;
}

// Entry point when executing a python function.
// We check if we can use a JIT compiled version or have to use the Interpreter
PyObject* _Py_HOT_FUNCTION
_PyEval_EvalFrameDefault(PyFrameObject *f, int throwflag)
{
    PyObject* retval = NULL;
    _PyRuntimeState * const runtime = &_PyRuntime;
    PyThreadState * const tstate = _PyRuntimeState_GetThreadState(runtime);

    /* push frame */
    if (Py_EnterRecursiveCall(""))
        return NULL;

    tstate->frame = f;

    if (tstate->use_tracing) {
        if (tstate->c_tracefunc != NULL) {
            /* tstate->c_tracefunc, if defined, is a
               function that will be called on *every* entry
               to a code block.  Its return value, if not
               None, is a function that will be called at
               the start of each executed line of code.
               (Actually, the function must return itself
               in order to continue tracing.)  The trace
               functions are called with three arguments:
               a pointer to the current frame, a string
               indicating why the function is called, and
               an argument which depends on the situation.
               The global trace function is also called
               whenever an exception is detected. */
            if (call_trace_protected(tstate->c_tracefunc,
                                     tstate->c_traceobj,
                                     tstate, f, PyTrace_CALL, Py_None)) {
                /* Trace function raised an error */
                goto exit_eval_frame;
            }
        }
        if (tstate->c_profilefunc != NULL) {
            /* Similar for c_profilefunc, except it needn't
               return itself and isn't called for "line" events */
            if (call_trace_protected(tstate->c_profilefunc,
                                     tstate->c_profileobj,
                                     tstate, f, PyTrace_CALL, Py_None)) {
                /* Profile function raised an error */
                goto exit_eval_frame;
            }
        }
    }

    if (PyDTrace_FUNCTION_ENTRY_ENABLED())
        dtrace_function_entry(f);

    PyObject **stack_pointer = f->f_stacktop;  /* Next free slot in value stack */
    assert(stack_pointer != NULL);
    f->f_stacktop = NULL;       /* remains NULL unless yield suspends frame */
    f->f_executing = 1;

    JitFunc jit_code = f->f_code->co_jit_code;

    // The jit assumes that globals and builtins are dicts so that it doesn't have to check them.
    // It looks like they are not changeable for a given frame, so we only have to check once
    // at the beginning, but they're not fixed for a code object so we can't just check at jit time.
    // Also don't enter the jit if the throwflag is set which skips the main code path and goes to error path.
    int can_use_jit = PyDict_CheckExact(f->f_globals) && PyDict_CheckExact(f->f_builtins) && !throwflag;
    if (jit_code == JIT_FUNC_FAILED) {
        can_use_jit = 0;
    }
    if (jit_code != NULL && can_use_jit) {
        retval = _PyEval_EvalFrame_AOT_JIT(f, tstate, stack_pointer, jit_code);
    } else {
        retval = _PyEval_EvalFrame_AOT_Interpreter(f, throwflag, tstate, stack_pointer, can_use_jit, 0);
    }

exit_eval_frame:
    if (PyDTrace_FUNCTION_RETURN_ENABLED())
        dtrace_function_return(f);
    Py_LeaveRecursiveCall();
    f->f_executing = 0;
    tstate->frame = f->f_back;

    return _Py_CheckFunctionResult(NULL, retval, "PyEval_EvalFrameEx");
}

static void
format_missing(PyThreadState *tstate, const char *kind,
               PyCodeObject *co, PyObject *names)
{
    int err;
    Py_ssize_t len = PyList_GET_SIZE(names);
    PyObject *name_str, *comma, *tail, *tmp;

    assert(PyList_CheckExact(names));
    assert(len >= 1);
    /* Deal with the joys of natural language. */
    switch (len) {
    case 1:
        name_str = PyList_GET_ITEM(names, 0);
        Py_INCREF(name_str);
        break;
    case 2:
        name_str = PyUnicode_FromFormat("%U and %U",
                                        PyList_GET_ITEM(names, len - 2),
                                        PyList_GET_ITEM(names, len - 1));
        break;
    default:
        tail = PyUnicode_FromFormat(", %U, and %U",
                                    PyList_GET_ITEM(names, len - 2),
                                    PyList_GET_ITEM(names, len - 1));
        if (tail == NULL)
            return;
        /* Chop off the last two objects in the list. This shouldn't actually
           fail, but we can't be too careful. */
        err = PyList_SetSlice(names, len - 2, len, NULL);
        if (err == -1) {
            Py_DECREF(tail);
            return;
        }
        /* Stitch everything up into a nice comma-separated list. */
        comma = PyUnicode_FromString(", ");
        if (comma == NULL) {
            Py_DECREF(tail);
            return;
        }
        tmp = PyUnicode_Join(comma, names);
        Py_DECREF(comma);
        if (tmp == NULL) {
            Py_DECREF(tail);
            return;
        }
        name_str = PyUnicode_Concat(tmp, tail);
        Py_DECREF(tmp);
        Py_DECREF(tail);
        break;
    }
    if (name_str == NULL)
        return;
    _PyErr_Format(tstate, PyExc_TypeError,
                  "%U() missing %i required %s argument%s: %U",
                  co->co_name,
                  len,
                  kind,
                  len == 1 ? "" : "s",
                  name_str);
    Py_DECREF(name_str);
}

static void
missing_arguments(PyThreadState *tstate, PyCodeObject *co,
                  Py_ssize_t missing, Py_ssize_t defcount,
                  PyObject **fastlocals)
{
    Py_ssize_t i, j = 0;
    Py_ssize_t start, end;
    int positional = (defcount != -1);
    const char *kind = positional ? "positional" : "keyword-only";
    PyObject *missing_names;

    /* Compute the names of the arguments that are missing. */
    missing_names = PyList_New(missing);
    if (missing_names == NULL)
        return;
    if (positional) {
        start = 0;
        end = co->co_argcount - defcount;
    }
    else {
        start = co->co_argcount;
        end = start + co->co_kwonlyargcount;
    }
    for (i = start; i < end; i++) {
        if (GETLOCAL(i) == NULL) {
            PyObject *raw = PyTuple_GET_ITEM(co->co_varnames, i);
            PyObject *name = PyObject_Repr(raw);
            if (name == NULL) {
                Py_DECREF(missing_names);
                return;
            }
            PyList_SET_ITEM(missing_names, j++, name);
        }
    }
    assert(j == missing);
    format_missing(tstate, kind, co, missing_names);
    Py_DECREF(missing_names);
}

static void
too_many_positional(PyThreadState *tstate, PyCodeObject *co,
                    Py_ssize_t given, Py_ssize_t defcount,
                    PyObject **fastlocals)
{
    int plural;
    Py_ssize_t kwonly_given = 0;
    Py_ssize_t i;
    PyObject *sig, *kwonly_sig;
    Py_ssize_t co_argcount = co->co_argcount;

    assert((co->co_flags & CO_VARARGS) == 0);
    /* Count missing keyword-only args. */
    for (i = co_argcount; i < co_argcount + co->co_kwonlyargcount; i++) {
        if (GETLOCAL(i) != NULL) {
            kwonly_given++;
        }
    }
    if (defcount) {
        Py_ssize_t atleast = co_argcount - defcount;
        plural = 1;
        sig = PyUnicode_FromFormat("from %zd to %zd", atleast, co_argcount);
    }
    else {
        plural = (co_argcount != 1);
        sig = PyUnicode_FromFormat("%zd", co_argcount);
    }
    if (sig == NULL)
        return;
    if (kwonly_given) {
        const char *format = " positional argument%s (and %zd keyword-only argument%s)";
        kwonly_sig = PyUnicode_FromFormat(format,
                                          given != 1 ? "s" : "",
                                          kwonly_given,
                                          kwonly_given != 1 ? "s" : "");
        if (kwonly_sig == NULL) {
            Py_DECREF(sig);
            return;
        }
    }
    else {
        /* This will not fail. */
        kwonly_sig = PyUnicode_FromString("");
        assert(kwonly_sig != NULL);
    }
    _PyErr_Format(tstate, PyExc_TypeError,
                  "%U() takes %U positional argument%s but %zd%U %s given",
                  co->co_name,
                  sig,
                  plural ? "s" : "",
                  given,
                  kwonly_sig,
                  given == 1 && !kwonly_given ? "was" : "were");
    Py_DECREF(sig);
    Py_DECREF(kwonly_sig);
}

static int
positional_only_passed_as_keyword(PyThreadState *tstate, PyCodeObject *co,
                                  Py_ssize_t kwcount, PyObject* const* kwnames)
{
    int posonly_conflicts = 0;
    PyObject* posonly_names = PyList_New(0);

    for(int k=0; k < co->co_posonlyargcount; k++){
        PyObject* posonly_name = PyTuple_GET_ITEM(co->co_varnames, k);

        for (int k2=0; k2<kwcount; k2++){
            /* Compare the pointers first and fallback to PyObject_RichCompareBool*/
            PyObject* kwname = kwnames[k2];
            if (kwname == posonly_name){
                if(PyList_Append(posonly_names, kwname) != 0) {
                    goto fail;
                }
                posonly_conflicts++;
                continue;
            }

            int cmp = PyObject_RichCompareBool(posonly_name, kwname, Py_EQ);

            if ( cmp > 0) {
                if(PyList_Append(posonly_names, kwname) != 0) {
                    goto fail;
                }
                posonly_conflicts++;
            } else if (cmp < 0) {
                goto fail;
            }

        }
    }
    if (posonly_conflicts) {
        PyObject* comma = PyUnicode_FromString(", ");
        if (comma == NULL) {
            goto fail;
        }
        PyObject* error_names = PyUnicode_Join(comma, posonly_names);
        Py_DECREF(comma);
        if (error_names == NULL) {
            goto fail;
        }
        _PyErr_Format(tstate, PyExc_TypeError,
                      "%U() got some positional-only arguments passed"
                      " as keyword arguments: '%U'",
                      co->co_name, error_names);
        Py_DECREF(error_names);
        goto fail;
    }

    Py_DECREF(posonly_names);
    return 0;

fail:
    Py_XDECREF(posonly_names);
    return 1;

}

/* This is gonna seem *real weird*, but if you put some other code between
   PyEval_EvalFrame() and _PyEval_EvalFrameDefault() you will need to adjust
   the test in the if statements in Misc/gdbinit (pystack and pystackv). */
#if 0
PyObject *
_PyEval_EvalCodeWithName(PyObject *_co, PyObject *globals, PyObject *locals,
           PyObject *const *args, Py_ssize_t argcount,
           PyObject *const *kwnames, PyObject *const *kwargs,
           Py_ssize_t kwcount, int kwstep,
           PyObject *const *defs, Py_ssize_t defcount,
           PyObject *kwdefs, PyObject *closure,
           PyObject *name, PyObject *qualname)
{
    PyCodeObject* co = (PyCodeObject*)_co;
    PyFrameObject *f;
    PyObject *retval = NULL;
    PyObject **fastlocals, **freevars;
    PyObject *x, *u;
    const Py_ssize_t total_args = co->co_argcount + co->co_kwonlyargcount;
    Py_ssize_t i, j, n;
    PyObject *kwdict;

    PyThreadState *tstate = _PyThreadState_GET();
    assert(tstate != NULL);

    if (globals == NULL) {
        _PyErr_SetString(tstate, PyExc_SystemError,
                         "PyEval_EvalCodeEx: NULL globals");
        return NULL;
    }

    /* Create the frame */
    f = _PyFrame_New_NoTrack(tstate, co, globals, locals);
    if (f == NULL) {
        return NULL;
    }
    fastlocals = f->f_localsplus;
    freevars = f->f_localsplus + co->co_nlocals;

    /* Create a dictionary for keyword parameters (**kwags) */
    if (co->co_flags & CO_VARKEYWORDS) {
        kwdict = PyDict_New();
        if (kwdict == NULL)
            goto fail;
        i = total_args;
        if (co->co_flags & CO_VARARGS) {
            i++;
        }
        SETLOCAL(i, kwdict);
    }
    else {
        kwdict = NULL;
    }

    /* Copy all positional arguments into local variables */
    if (argcount > co->co_argcount) {
        n = co->co_argcount;
    }
    else {
        n = argcount;
    }
    for (j = 0; j < n; j++) {
        x = args[j];
        Py_INCREF(x);
        SETLOCAL(j, x);
    }

    /* Pack other positional arguments into the *args argument */
    if (co->co_flags & CO_VARARGS) {
        u = _PyTuple_FromArray(args + n, argcount - n);
        if (u == NULL) {
            goto fail;
        }
        SETLOCAL(total_args, u);
    }

    /* Handle keyword arguments passed as two strided arrays */
    kwcount *= kwstep;
    for (i = 0; i < kwcount; i += kwstep) {
        PyObject **co_varnames;
        PyObject *keyword = kwnames[i];
        PyObject *value = kwargs[i];
        Py_ssize_t j;

        if (keyword == NULL || !PyUnicode_Check(keyword)) {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "%U() keywords must be strings",
                          co->co_name);
            goto fail;
        }

        /* Speed hack: do raw pointer compares. As names are
           normally interned this should almost always hit. */
        co_varnames = ((PyTupleObject *)(co->co_varnames))->ob_item;
        for (j = co->co_posonlyargcount; j < total_args; j++) {
            PyObject *name = co_varnames[j];
            if (name == keyword) {
                goto kw_found;
            }
        }

        /* Slow fallback, just in case */
        for (j = co->co_posonlyargcount; j < total_args; j++) {
            PyObject *name = co_varnames[j];
            int cmp = PyObject_RichCompareBool( keyword, name, Py_EQ);
            if (cmp > 0) {
                goto kw_found;
            }
            else if (cmp < 0) {
                goto fail;
            }
        }

        assert(j >= total_args);
        if (kwdict == NULL) {

            if (co->co_posonlyargcount
                && positional_only_passed_as_keyword(tstate, co,
                                                     kwcount, kwnames))
            {
                goto fail;
            }

            _PyErr_Format(tstate, PyExc_TypeError,
                          "%U() got an unexpected keyword argument '%S'",
                          co->co_name, keyword);
            goto fail;
        }

        if (PyDict_SetItem(kwdict, keyword, value) == -1) {
            goto fail;
        }
        continue;

      kw_found:
        if (GETLOCAL(j) != NULL) {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "%U() got multiple values for argument '%S'",
                          co->co_name, keyword);
            goto fail;
        }
        Py_INCREF(value);
        SETLOCAL(j, value);
    }

    /* Check the number of positional arguments */
    if ((argcount > co->co_argcount) && !(co->co_flags & CO_VARARGS)) {
        too_many_positional(tstate, co, argcount, defcount, fastlocals);
        goto fail;
    }

    /* Add missing positional arguments (copy default values from defs) */
    if (argcount < co->co_argcount) {
        Py_ssize_t m = co->co_argcount - defcount;
        Py_ssize_t missing = 0;
        for (i = argcount; i < m; i++) {
            if (GETLOCAL(i) == NULL) {
                missing++;
            }
        }
        if (missing) {
            missing_arguments(tstate, co, missing, defcount, fastlocals);
            goto fail;
        }
        if (n > m)
            i = n - m;
        else
            i = 0;
        for (; i < defcount; i++) {
            if (GETLOCAL(m+i) == NULL) {
                PyObject *def = defs[i];
                Py_INCREF(def);
                SETLOCAL(m+i, def);
            }
        }
    }

    /* Add missing keyword arguments (copy default values from kwdefs) */
    if (co->co_kwonlyargcount > 0) {
        Py_ssize_t missing = 0;
        for (i = co->co_argcount; i < total_args; i++) {
            PyObject *name;
            if (GETLOCAL(i) != NULL)
                continue;
            name = PyTuple_GET_ITEM(co->co_varnames, i);
            if (kwdefs != NULL) {
                PyObject *def = PyDict_GetItemWithError(kwdefs, name);
                if (def) {
                    Py_INCREF(def);
                    SETLOCAL(i, def);
                    continue;
                }
                else if (_PyErr_Occurred(tstate)) {
                    goto fail;
                }
            }
            missing++;
        }
        if (missing) {
            missing_arguments(tstate, co, missing, -1, fastlocals);
            goto fail;
        }
    }

    /* Allocate and initialize storage for cell vars, and copy free
       vars into frame. */
    for (i = 0; i < PyTuple_GET_SIZE(co->co_cellvars); ++i) {
        PyObject *c;
        Py_ssize_t arg;
        /* Possibly account for the cell variable being an argument. */
        if (co->co_cell2arg != NULL &&
            (arg = co->co_cell2arg[i]) != CO_CELL_NOT_AN_ARG) {
            c = PyCell_New(GETLOCAL(arg));
            /* Clear the local copy. */
            SETLOCAL(arg, NULL);
        }
        else {
            c = PyCell_New(NULL);
        }
        if (c == NULL)
            goto fail;
        SETLOCAL(co->co_nlocals + i, c);
    }

    /* Copy closure variables to free variables */
    for (i = 0; i < PyTuple_GET_SIZE(co->co_freevars); ++i) {
        PyObject *o = PyTuple_GET_ITEM(closure, i);
        Py_INCREF(o);
        freevars[PyTuple_GET_SIZE(co->co_cellvars) + i] = o;
    }

    /* Handle generator/coroutine/asynchronous generator */
    if (co->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)) {
        PyObject *gen;
        int is_coro = co->co_flags & CO_COROUTINE;

        /* Don't need to keep the reference to f_back, it will be set
         * when the generator is resumed. */
        Py_CLEAR(f->f_back);

        /* Create a new generator that owns the ready to run frame
         * and return that as the value. */
        if (is_coro) {
            gen = PyCoro_New(f, name, qualname);
        } else if (co->co_flags & CO_ASYNC_GENERATOR) {
            gen = PyAsyncGen_New(f, name, qualname);
        } else {
            gen = PyGen_NewWithQualName(f, name, qualname);
        }
        if (gen == NULL) {
            return NULL;
        }

        _PyObject_GC_TRACK(f);

        return gen;
    }

    retval = PyEval_EvalFrameEx(f,0);

fail: /* Jump here from prelude on failure */

    /* decref'ing the frame can cause __del__ methods to get invoked,
       which can call back into Python.  While we're done with the
       current Python frame (f), the associated C stack is still in use,
       so recursion_depth must be boosted for the duration.
    */
    assert(tstate != NULL);
    if (Py_REFCNT(f) > 1) {
        Py_DECREF(f);
        _PyObject_GC_TRACK(f);
    }
    else {
#if !PYSTON_SPEEDUPS
        ++tstate->recursion_depth;
#endif
        Py_DECREF(f);
#if !PYSTON_SPEEDUPS
        --tstate->recursion_depth;
#endif
    }
    return retval;
}

PyObject *
PyEval_EvalCodeEx(PyObject *_co, PyObject *globals, PyObject *locals,
                  PyObject *const *args, int argcount,
                  PyObject *const *kws, int kwcount,
                  PyObject *const *defs, int defcount,
                  PyObject *kwdefs, PyObject *closure)
{
    return _PyEval_EvalCodeWithName(_co, globals, locals,
                                    args, argcount,
                                    kws, kws != NULL ? kws + 1 : NULL,
                                    kwcount, 2,
                                    defs, defcount,
                                    kwdefs, closure,
                                    NULL, NULL);
}
#endif
/*static*/ PyObject *
special_lookup(PyThreadState *tstate, PyObject *o, _Py_Identifier *id)
{
    PyObject *res;
    res = _PyObject_LookupSpecial(o, id);
    if (res == NULL && !_PyErr_Occurred(tstate)) {
        _PyErr_SetObject(tstate, PyExc_AttributeError, id->object);
        return NULL;
    }
    return res;
}


/* Logic for the raise statement (too complicated for inlining).
   This *consumes* a reference count to each of its arguments. */
/*static*/ int
do_raise(PyThreadState *tstate, PyObject *exc, PyObject *cause)
{
    PyObject *type = NULL, *value = NULL;

    if (exc == NULL) {
        /* Reraise */
        _PyErr_StackItem *exc_info = _PyErr_GetTopmostException(tstate);
        PyObject *tb;
        type = exc_info->exc_type;
        value = exc_info->exc_value;
        tb = exc_info->exc_traceback;
        if (type == Py_None || type == NULL) {
            _PyErr_SetString(tstate, PyExc_RuntimeError,
                             "No active exception to reraise");
            return 0;
        }
        Py_XINCREF(type);
        Py_XINCREF(value);
        Py_XINCREF(tb);
        _PyErr_Restore(tstate, type, value, tb);
        return 1;
    }

    /* We support the following forms of raise:
       raise
       raise <instance>
       raise <type> */

    if (PyExceptionClass_Check(exc)) {
        type = exc;
        value = _PyObject_CallNoArg(exc);
        if (value == NULL)
            goto raise_error;
        if (!PyExceptionInstance_Check(value)) {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "calling %R should have returned an instance of "
                          "BaseException, not %R",
                          type, Py_TYPE(value));
             goto raise_error;
        }
    }
    else if (PyExceptionInstance_Check(exc)) {
        value = exc;
        type = PyExceptionInstance_Class(exc);
        Py_INCREF(type);
    }
    else {
        /* Not something you can raise.  You get an exception
           anyway, just not what you specified :-) */
        Py_DECREF(exc);
        _PyErr_SetString(tstate, PyExc_TypeError,
                         "exceptions must derive from BaseException");
        goto raise_error;
    }

    assert(type != NULL);
    assert(value != NULL);

    if (cause) {
        PyObject *fixed_cause;
        if (PyExceptionClass_Check(cause)) {
            fixed_cause = _PyObject_CallNoArg(cause);
            if (fixed_cause == NULL)
                goto raise_error;
            Py_DECREF(cause);
        }
        else if (PyExceptionInstance_Check(cause)) {
            fixed_cause = cause;
        }
        else if (cause == Py_None) {
            Py_DECREF_IMMORTAL(cause);
            fixed_cause = NULL;
        }
        else {
            _PyErr_SetString(tstate, PyExc_TypeError,
                             "exception causes must derive from "
                             "BaseException");
            goto raise_error;
        }
        PyException_SetCause(value, fixed_cause);
    }

    _PyErr_SetObject(tstate, type, value);
    /* PyErr_SetObject incref's its arguments */
    Py_DECREF(value);
    Py_DECREF(type);
    return 0;

raise_error:
    Py_XDECREF(value);
    Py_XDECREF(type);
    Py_XDECREF(cause);
    return 0;
}

/* Iterate v argcnt times and store the results on the stack (via decreasing
   sp).  Return 1 for success, 0 if error.

   If argcntafter == -1, do a simple unpack. If it is >= 0, do an unpack
   with a variable target.
*/

/*static*/ int
unpack_iterable(PyThreadState *tstate, PyObject *v,
                int argcnt, int argcntafter, PyObject **sp)
{
    int i = 0, j = 0;
    Py_ssize_t ll = 0;
    PyObject *it;  /* iter(v) */
    PyObject *w;
    PyObject *l = NULL; /* variable list */

    assert(v != NULL);

    it = PyObject_GetIter(v);
    if (it == NULL) {
        if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError) &&
            v->ob_type->tp_iter == NULL && !PySequence_Check(v))
        {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "cannot unpack non-iterable %.200s object",
                          v->ob_type->tp_name);
        }
        return 0;
    }

    for (; i < argcnt; i++) {
        w = PyIter_Next(it);
        if (w == NULL) {
            /* Iterator done, via error or exhaustion. */
            if (!_PyErr_Occurred(tstate)) {
                if (argcntafter == -1) {
                    _PyErr_Format(tstate, PyExc_ValueError,
                                  "not enough values to unpack "
                                  "(expected %d, got %d)",
                                  argcnt, i);
                }
                else {
                    _PyErr_Format(tstate, PyExc_ValueError,
                                  "not enough values to unpack "
                                  "(expected at least %d, got %d)",
                                  argcnt + argcntafter, i);
                }
            }
            goto Error;
        }
        *--sp = w;
    }

    if (argcntafter == -1) {
        /* We better have exhausted the iterator now. */
        w = PyIter_Next(it);
        if (w == NULL) {
            if (_PyErr_Occurred(tstate))
                goto Error;
            Py_DECREF(it);
            return 1;
        }
        Py_DECREF(w);
        _PyErr_Format(tstate, PyExc_ValueError,
                      "too many values to unpack (expected %d)",
                      argcnt);
        goto Error;
    }

    l = PySequence_List(it);
    if (l == NULL)
        goto Error;
    *--sp = l;
    i++;

    ll = PyList_GET_SIZE(l);
    if (ll < argcntafter) {
        _PyErr_Format(tstate, PyExc_ValueError,
            "not enough values to unpack (expected at least %d, got %zd)",
            argcnt + argcntafter, argcnt + ll);
        goto Error;
    }

    /* Pop the "after-variable" args off the list. */
    for (j = argcntafter; j > 0; j--, i++) {
        *--sp = PyList_GET_ITEM(l, ll - j);
    }
    /* Resize the list. */
    Py_SIZE(l) = ll - argcntafter;
    Py_DECREF(it);
    return 1;

Error:
    for (; i > 0; i--, sp++)
        Py_DECREF(*sp);
    Py_XDECREF(it);
    return 0;
}


#ifdef LLTRACE
static int
prtrace(PyThreadState *tstate, PyObject *v, const char *str)
{
    printf("%s ", str);
    if (PyObject_Print(v, stdout, 0) != 0) {
        /* Don't know what else to do */
        _PyErr_Clear(tstate);
    }
    printf("\n");
    return 1;
}
#endif

/*static*/ void
call_exc_trace(Py_tracefunc func, PyObject *self,
               PyThreadState *tstate, PyFrameObject *f)
{
    PyObject *type, *value, *traceback, *orig_traceback, *arg;
    int err;
    _PyErr_Fetch(tstate, &type, &value, &orig_traceback);
    if (value == NULL) {
        value = Py_None;
        Py_INCREF_IMMORTAL(value);
    }
    _PyErr_NormalizeException(tstate, &type, &value, &orig_traceback);
    traceback = (orig_traceback != NULL) ? orig_traceback : Py_None;
    arg = PyTuple_Pack3(type, value, traceback);
    if (arg == NULL) {
        _PyErr_Restore(tstate, type, value, orig_traceback);
        return;
    }
    err = call_trace(func, self, tstate, f, PyTrace_EXCEPTION, arg);
    Py_DECREF(arg);
    if (err == 0) {
        _PyErr_Restore(tstate, type, value, orig_traceback);
    }
    else {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(orig_traceback);
    }
}

static int
call_trace_protected(Py_tracefunc func, PyObject *obj,
                     PyThreadState *tstate, PyFrameObject *frame,
                     int what, PyObject *arg)
{
    PyObject *type, *value, *traceback;
    int err;
    _PyErr_Fetch(tstate, &type, &value, &traceback);
    err = call_trace(func, obj, tstate, frame, what, arg);
    if (err == 0)
    {
        _PyErr_Restore(tstate, type, value, traceback);
        return 0;
    }
    else {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);
        return -1;
    }
}

static int
call_trace(Py_tracefunc func, PyObject *obj,
           PyThreadState *tstate, PyFrameObject *frame,
           int what, PyObject *arg)
{
    int result;
    if (tstate->tracing)
        return 0;
    tstate->tracing++;
    tstate->use_tracing = 0;
    result = func(obj, frame, what, arg);
    tstate->use_tracing = ((tstate->c_tracefunc != NULL)
                           || (tstate->c_profilefunc != NULL));
    tstate->tracing--;
    return result;
}
#if 0
PyObject *
_PyEval_CallTracing(PyObject *func, PyObject *args)
{
    PyThreadState *tstate = _PyThreadState_GET();
    int save_tracing = tstate->tracing;
    int save_use_tracing = tstate->use_tracing;
    PyObject *result;

    tstate->tracing = 0;
    tstate->use_tracing = ((tstate->c_tracefunc != NULL)
                           || (tstate->c_profilefunc != NULL));
    result = PyObject_Call(func, args, NULL);
    tstate->tracing = save_tracing;
    tstate->use_tracing = save_use_tracing;
    return result;
}
#endif
/* See Objects/lnotab_notes.txt for a description of how tracing works. */
static int
maybe_call_line_trace(Py_tracefunc func, PyObject *obj,
                      PyThreadState *tstate, PyFrameObject *frame,
                      int *instr_lb, int *instr_ub, int *instr_prev, int *jit_first_trace_for_line)
{
    int result = 0;
    int line = frame->f_lineno;

    /* If the last instruction executed isn't in the current
       instruction window, reset the window.
    */
    if (frame->f_lasti < *instr_lb || frame->f_lasti >= *instr_ub) {
        PyAddrPair bounds;
        line = _PyCode_CheckLineNumber(frame->f_code, frame->f_lasti,
                                       &bounds);
        *instr_lb = bounds.ap_lower;
        *instr_ub = bounds.ap_upper;
    }
    /* If the last instruction falls at the start of a line or if it
       represents a jump backwards, update the frame's line number and
       then call the trace function if we're tracing source lines.
    */
    if (*jit_first_trace_for_line || (frame->f_lasti == *instr_lb || frame->f_lasti < *instr_prev)) {
        *jit_first_trace_for_line = 0;
        frame->f_lineno = line;
        if (frame->f_trace_lines) {
            result = call_trace(func, obj, tstate, frame, PyTrace_LINE, Py_None);
        }
    }
    /* Always emit an opcode event if we're tracing all opcodes. */
    if (frame->f_trace_opcodes) {
        result = call_trace(func, obj, tstate, frame, PyTrace_OPCODE, Py_None);
    }
    *instr_prev = frame->f_lasti;
    return result;
}
#if 0
void
PyEval_SetProfile(Py_tracefunc func, PyObject *arg)
{
    if (PySys_Audit("sys.setprofile", NULL) < 0) {
        _PyErr_WriteUnraisableMsg("in PyEval_SetProfile", NULL);
        return;
    }

    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *temp = tstate->c_profileobj;
    Py_XINCREF(arg);
    tstate->c_profilefunc = NULL;
    tstate->c_profileobj = NULL;
    /* Must make sure that tracing is not ignored if 'temp' is freed */
    tstate->use_tracing = tstate->c_tracefunc != NULL;
    Py_XDECREF(temp);
    tstate->c_profilefunc = func;
    tstate->c_profileobj = arg;
    /* Flag that tracing or profiling is turned on */
    tstate->use_tracing = (func != NULL) || (tstate->c_tracefunc != NULL);
}

void
PyEval_SetTrace(Py_tracefunc func, PyObject *arg)
{
    if (PySys_Audit("sys.settrace", NULL) < 0) {
        _PyErr_WriteUnraisableMsg("in PyEval_SetTrace", NULL);
        return;
    }

    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *tstate = _PyRuntimeState_GetThreadState(runtime);
    PyObject *temp = tstate->c_traceobj;
    runtime->ceval.tracing_possible += (func != NULL) - (tstate->c_tracefunc != NULL);
    Py_XINCREF(arg);
    tstate->c_tracefunc = NULL;
    tstate->c_traceobj = NULL;
    /* Must make sure that profiling is not ignored if 'temp' is freed */
    tstate->use_tracing = tstate->c_profilefunc != NULL;
    Py_XDECREF(temp);
    tstate->c_tracefunc = func;
    tstate->c_traceobj = arg;
    /* Flag that tracing or profiling is turned on */
    tstate->use_tracing = ((func != NULL)
                           || (tstate->c_profilefunc != NULL));
}

void
_PyEval_SetCoroutineOriginTrackingDepth(int new_depth)
{
    assert(new_depth >= 0);
    PyThreadState *tstate = _PyThreadState_GET();
    tstate->coroutine_origin_tracking_depth = new_depth;
}

int
_PyEval_GetCoroutineOriginTrackingDepth(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->coroutine_origin_tracking_depth;
}

PyObject *
_PyEval_GetAsyncGenFirstiter(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->async_gen_firstiter;
}

PyObject *
_PyEval_GetAsyncGenFinalizer(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->async_gen_finalizer;
}
#endif
static PyFrameObject *
_PyEval_GetFrame(PyThreadState *tstate)
{
    return _PyRuntime.gilstate.getframe(tstate);
}
#if 0
PyFrameObject *
PyEval_GetFrame(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return _PyEval_GetFrame(tstate);
}

PyObject *
PyEval_GetBuiltins(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyFrameObject *current_frame = _PyEval_GetFrame(tstate);
    if (current_frame == NULL)
        return tstate->interp->builtins;
    else
        return current_frame->f_builtins;
}

/* Convenience function to get a builtin from its name */
PyObject *
_PyEval_GetBuiltinId(_Py_Identifier *name)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *attr = _PyDict_GetItemIdWithError(PyEval_GetBuiltins(), name);
    if (attr) {
        Py_INCREF(attr);
    }
    else if (!_PyErr_Occurred(tstate)) {
        _PyErr_SetObject(tstate, PyExc_AttributeError, _PyUnicode_FromId(name));
    }
    return attr;
}

PyObject *
PyEval_GetLocals(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyFrameObject *current_frame = _PyEval_GetFrame(tstate);
    if (current_frame == NULL) {
        _PyErr_SetString(tstate, PyExc_SystemError, "frame does not exist");
        return NULL;
    }

    if (PyFrame_FastToLocalsWithError(current_frame) < 0) {
        return NULL;
    }

    assert(current_frame->f_locals != NULL);
    return current_frame->f_locals;
}

PyObject *
PyEval_GetGlobals(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyFrameObject *current_frame = _PyEval_GetFrame(tstate);
    if (current_frame == NULL) {
        return NULL;
    }

    assert(current_frame->f_globals != NULL);
    return current_frame->f_globals;
}

int
PyEval_MergeCompilerFlags(PyCompilerFlags *cf)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyFrameObject *current_frame = _PyEval_GetFrame(tstate);
    int result = cf->cf_flags != 0;

    if (current_frame != NULL) {
        const int codeflags = current_frame->f_code->co_flags;
        const int compilerflags = codeflags & PyCF_MASK;
        if (compilerflags) {
            result = 1;
            cf->cf_flags |= compilerflags;
        }
#if 0 /* future keyword */
        if (codeflags & CO_GENERATOR_ALLOWED) {
            result = 1;
            cf->cf_flags |= CO_GENERATOR_ALLOWED;
        }
#endif
    }
    return result;
}


const char *
PyEval_GetFuncName(PyObject *func)
{
    if (PyMethod_Check(func))
        return PyEval_GetFuncName(PyMethod_GET_FUNCTION(func));
    else if (PyFunction_Check(func))
        return PyUnicode_AsUTF8(((PyFunctionObject*)func)->func_name);
    else if (PyCFunction_Check(func))
        return ((PyCFunctionObject*)func)->m_ml->ml_name;
    else
        return func->ob_type->tp_name;
}

const char *
PyEval_GetFuncDesc(PyObject *func)
{
    if (PyMethod_Check(func))
        return "()";
    else if (PyFunction_Check(func))
        return "()";
    else if (PyCFunction_Check(func))
        return "()";
    else
        return " object";
}
#endif

#define C_TRACE(x, call) \
if (tstate->use_tracing && tstate->c_profilefunc) { \
    if (call_trace(tstate->c_profilefunc, tstate->c_profileobj, \
        tstate, tstate->frame, \
        PyTrace_C_CALL, func)) { \
        x = NULL; \
    } \
    else { \
        x = call; \
        if (tstate->c_profilefunc != NULL) { \
            if (x == NULL) { \
                call_trace_protected(tstate->c_profilefunc, \
                    tstate->c_profileobj, \
                    tstate, tstate->frame, \
                    PyTrace_C_EXCEPTION, func); \
                /* XXX should pass (type, value, tb) */ \
            } else { \
                if (call_trace(tstate->c_profilefunc, \
                    tstate->c_profileobj, \
                    tstate, tstate->frame, \
                    PyTrace_C_RETURN, func)) { \
                    Py_DECREF(x); \
                    x = NULL; \
                } \
            } \
        } \
    } \
} else { \
    x = call; \
    }

static PyObject *
trace_call_function(PyThreadState *tstate,
                    PyObject *func,
                    PyObject **args, Py_ssize_t nargs,
                    PyObject *kwnames)
{
    PyObject *x;
    if (PyCFunction_Check(func)) {
        C_TRACE(x, _PyObject_Vectorcall(func, args, nargs, kwnames));
        return x;
    }
    else if (Py_TYPE(func) == &PyMethodDescr_Type && nargs > 0) {
        /* We need to create a temporary bound method as argument
           for profiling.

           If nargs == 0, then this cannot work because we have no
           "self". In any case, the call itself would raise
           TypeError (foo needs an argument), so we just skip
           profiling. */
        PyObject *self = args[0];
        func = Py_TYPE(func)->tp_descr_get(func, self, (PyObject*)Py_TYPE(self));
        if (func == NULL) {
            return NULL;
        }
        C_TRACE(x, _PyObject_Vectorcall(func,
                                        args+1, nargs-1,
                                        kwnames));
        Py_DECREF(func);
        return x;
    }
    return _PyObject_Vectorcall(func, args, nargs | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
}

/* Issue #29227: Inline call_function() into _PyEval_EvalFrameDefault()
   to reduce the stack consumption. */
// already non statically defined inside ceval.c
#if 0
Py_LOCAL_INLINE(PyObject *) _Py_HOT_FUNCTION
call_function_ceval(PyThreadState *tstate, PyObject ***pp_stack, Py_ssize_t oparg, PyObject *kwnames)
{
    PyObject **pfunc = (*pp_stack) - oparg - 1;
    PyObject *func = *pfunc;
    PyObject *x, *w;
    Py_ssize_t nkwargs = (kwnames == NULL) ? 0 : PyTuple_GET_SIZE(kwnames);
    Py_ssize_t nargs = oparg - nkwargs;
    PyObject **stack = (*pp_stack) - nargs - nkwargs;

    if (tstate->use_tracing) {
        x = trace_call_function(tstate, func, stack, nargs, kwnames);
    }
    else {
        x = _PyObject_Vectorcall(func, stack, nargs | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
    }

    assert((x != NULL) ^ (_PyErr_Occurred(tstate) != NULL));

    /* Clear the stack of the function object. */
    while ((*pp_stack) > pfunc) {
        w = EXT_POP(*pp_stack);
        Py_DECREF(w);
    }

    return x;
}
#endif

/*static*/ PyObject *
do_call_core(PyThreadState *tstate, PyObject *func, PyObject *callargs, PyObject *kwdict)
{
    PyObject *result;

    if (PyCFunction_Check(func)) {
        C_TRACE(result, PyCFunction_Call(func, callargs, kwdict));
        return result;
    }
    else if (Py_TYPE(func) == &PyMethodDescr_Type) {
        Py_ssize_t nargs = PyTuple_GET_SIZE(callargs);
        if (nargs > 0 && tstate->use_tracing) {
            /* We need to create a temporary bound method as argument
               for profiling.

               If nargs == 0, then this cannot work because we have no
               "self". In any case, the call itself would raise
               TypeError (foo needs an argument), so we just skip
               profiling. */
            PyObject *self = PyTuple_GET_ITEM(callargs, 0);
            func = Py_TYPE(func)->tp_descr_get(func, self, (PyObject*)Py_TYPE(self));
            if (func == NULL) {
                return NULL;
            }

            C_TRACE(result, _PyObject_FastCallDict(func,
                                                   &_PyTuple_ITEMS(callargs)[1],
                                                   nargs - 1,
                                                   kwdict));
            Py_DECREF(func);
            return result;
        }
    }
    return PyObject_Call(func, callargs, kwdict);
}
#if 0
/* Extract a slice index from a PyLong or an object with the
   nb_index slot defined, and store in *pi.
   Silently reduce values larger than PY_SSIZE_T_MAX to PY_SSIZE_T_MAX,
   and silently boost values less than PY_SSIZE_T_MIN to PY_SSIZE_T_MIN.
   Return 0 on error, 1 on success.
*/
int
_PyEval_SliceIndex(PyObject *v, Py_ssize_t *pi)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (v != Py_None) {
        Py_ssize_t x;
        if (PyIndex_Check(v)) {
            x = PyNumber_AsSsize_t(v, NULL);
            if (x == -1 && _PyErr_Occurred(tstate))
                return 0;
        }
        else {
            _PyErr_SetString(tstate, PyExc_TypeError,
                             "slice indices must be integers or "
                             "None or have an __index__ method");
            return 0;
        }
        *pi = x;
    }
    return 1;
}

int
_PyEval_SliceIndexNotNone(PyObject *v, Py_ssize_t *pi)
{
    PyThreadState *tstate = _PyThreadState_GET();
    Py_ssize_t x;
    if (PyIndex_Check(v)) {
        x = PyNumber_AsSsize_t(v, NULL);
        if (x == -1 && _PyErr_Occurred(tstate))
            return 0;
    }
    else {
        _PyErr_SetString(tstate, PyExc_TypeError,
                         "slice indices must be integers or "
                         "have an __index__ method");
        return 0;
    }
    *pi = x;
    return 1;
}
#endif

#define CANNOT_CATCH_MSG "catching classes that do not inherit from "\
                         "BaseException is not allowed"
// already non statically defined inside ceval.c
#if 0
static PyObject *
cmp_outcome(PyThreadState *tstate, int op, PyObject *v, PyObject *w)
{
    int res = 0;
    switch (op) {
    case PyCmp_IS:
        res = (v == w);
        break;
    case PyCmp_IS_NOT:
        res = (v != w);
        break;
    case PyCmp_IN:
        res = PySequence_Contains(w, v);
        if (res < 0)
            return NULL;
        break;
    case PyCmp_NOT_IN:
        res = PySequence_Contains(w, v);
        if (res < 0)
            return NULL;
        res = !res;
        break;
    case PyCmp_EXC_MATCH:
        if (PyTuple_Check(w)) {
            Py_ssize_t i, length;
            length = PyTuple_Size(w);
            for (i = 0; i < length; i += 1) {
                PyObject *exc = PyTuple_GET_ITEM(w, i);
                if (!PyExceptionClass_Check(exc)) {
                    _PyErr_SetString(tstate, PyExc_TypeError,
                                     CANNOT_CATCH_MSG);
                    return NULL;
                }
            }
        }
        else {
            if (!PyExceptionClass_Check(w)) {
                _PyErr_SetString(tstate, PyExc_TypeError,
                                 CANNOT_CATCH_MSG);
                return NULL;
            }
        }
        res = PyErr_GivenExceptionMatches(v, w);
        break;
    default:
        return PyObject_RichCompare(v, w, op);
    }
    v = res ? Py_True : Py_False;
    Py_INCREF(v);
    return v;
}
#endif

/*static*/ PyObject *
import_name(PyThreadState *tstate, PyFrameObject *f,
            PyObject *name, PyObject *fromlist, PyObject *level)
{
    _Py_IDENTIFIER(__import__);
    PyObject *import_func, *res;
    PyObject* stack[5];

    import_func = _PyDict_GetItemIdWithError(f->f_builtins, &PyId___import__);
    if (import_func == NULL) {
        if (!_PyErr_Occurred(tstate)) {
            _PyErr_SetString(tstate, PyExc_ImportError, "__import__ not found");
        }
        return NULL;
    }

    /* Fast path for not overloaded __import__. */
    if (import_func == tstate->interp->import_func) {
        int ilevel = _PyLong_AsInt(level);
        if (ilevel == -1 && _PyErr_Occurred(tstate)) {
            return NULL;
        }
        res = PyImport_ImportModuleLevelObject(
                        name,
                        f->f_globals,
                        f->f_locals == NULL ? Py_None : f->f_locals,
                        fromlist,
                        ilevel);
        return res;
    }

    Py_INCREF(import_func);

    stack[0] = name;
    stack[1] = f->f_globals;
    stack[2] = f->f_locals == NULL ? Py_None : f->f_locals;
    stack[3] = fromlist;
    stack[4] = level;
    res = _PyObject_FastCall(import_func, stack, 5);
    Py_DECREF(import_func);
    return res;
}

/*static*/ PyObject *
import_from(PyThreadState *tstate, PyObject *v, PyObject *name)
{
    PyObject *x;
    _Py_IDENTIFIER(__name__);
    PyObject *fullmodname, *pkgname, *pkgpath, *pkgname_or_unknown, *errmsg;

    if (_PyObject_LookupAttr(v, name, &x) != 0) {
        return x;
    }
    /* Issue #17636: in case this failed because of a circular relative
       import, try to fallback on reading the module directly from
       sys.modules. */
    pkgname = _PyObject_GetAttrId(v, &PyId___name__);
    if (pkgname == NULL) {
        goto error;
    }
    if (!PyUnicode_Check(pkgname)) {
        Py_CLEAR(pkgname);
        goto error;
    }
    fullmodname = PyUnicode_FromFormat("%U.%U", pkgname, name);
    if (fullmodname == NULL) {
        Py_DECREF(pkgname);
        return NULL;
    }
    x = PyImport_GetModule(fullmodname);
    Py_DECREF(fullmodname);
    if (x == NULL && !_PyErr_Occurred(tstate)) {
        goto error;
    }
    Py_DECREF(pkgname);
    return x;
 error:
    pkgpath = PyModule_GetFilenameObject(v);
    if (pkgname == NULL) {
        pkgname_or_unknown = PyUnicode_FromString("<unknown module name>");
        if (pkgname_or_unknown == NULL) {
            Py_XDECREF(pkgpath);
            return NULL;
        }
    } else {
        pkgname_or_unknown = pkgname;
    }

    if (pkgpath == NULL || !PyUnicode_Check(pkgpath)) {
        _PyErr_Clear(tstate);
        errmsg = PyUnicode_FromFormat(
            "cannot import name %R from %R (unknown location)",
            name, pkgname_or_unknown
        );
        /* NULL checks for errmsg and pkgname done by PyErr_SetImportError. */
        PyErr_SetImportError(errmsg, pkgname, NULL);
    }
    else {
        _Py_IDENTIFIER(__spec__);
        PyObject *spec = _PyObject_GetAttrId(v, &PyId___spec__);
        const char *fmt =
            _PyModuleSpec_IsInitializing(spec) ?
            "cannot import name %R from partially initialized module %R "
            "(most likely due to a circular import) (%S)" :
            "cannot import name %R from %R (%S)";
        Py_XDECREF(spec);

        errmsg = PyUnicode_FromFormat(fmt, name, pkgname_or_unknown, pkgpath);
        /* NULL checks for errmsg and pkgname done by PyErr_SetImportError. */
        PyErr_SetImportError(errmsg, pkgname, pkgpath);
    }

    Py_XDECREF(errmsg);
    Py_XDECREF(pkgname_or_unknown);
    Py_XDECREF(pkgpath);
    return NULL;
}

/*static*/ int
import_all_from(PyThreadState *tstate, PyObject *locals, PyObject *v)
{
    _Py_IDENTIFIER(__all__);
    _Py_IDENTIFIER(__dict__);
    _Py_IDENTIFIER(__name__);
    PyObject *all, *dict, *name, *value;
    int skip_leading_underscores = 0;
    int pos, err;

    if (_PyObject_LookupAttrId(v, &PyId___all__, &all) < 0) {
        return -1; /* Unexpected error */
    }
    if (all == NULL) {
        if (_PyObject_LookupAttrId(v, &PyId___dict__, &dict) < 0) {
            return -1;
        }
        if (dict == NULL) {
            _PyErr_SetString(tstate, PyExc_ImportError,
                    "from-import-* object has no __dict__ and no __all__");
            return -1;
        }
        all = PyMapping_Keys(dict);
        Py_DECREF(dict);
        if (all == NULL)
            return -1;
        skip_leading_underscores = 1;
    }

    for (pos = 0, err = 0; ; pos++) {
        name = PySequence_GetItem(all, pos);
        if (name == NULL) {
            if (!_PyErr_ExceptionMatches(tstate, PyExc_IndexError)) {
                err = -1;
            }
            else {
                _PyErr_Clear(tstate);
            }
            break;
        }
        if (!PyUnicode_Check(name)) {
            PyObject *modname = _PyObject_GetAttrId(v, &PyId___name__);
            if (modname == NULL) {
                Py_DECREF(name);
                err = -1;
                break;
            }
            if (!PyUnicode_Check(modname)) {
                _PyErr_Format(tstate, PyExc_TypeError,
                              "module __name__ must be a string, not %.100s",
                              Py_TYPE(modname)->tp_name);
            }
            else {
                _PyErr_Format(tstate, PyExc_TypeError,
                              "%s in %U.%s must be str, not %.100s",
                              skip_leading_underscores ? "Key" : "Item",
                              modname,
                              skip_leading_underscores ? "__dict__" : "__all__",
                              Py_TYPE(name)->tp_name);
            }
            Py_DECREF(modname);
            Py_DECREF(name);
            err = -1;
            break;
        }
        if (skip_leading_underscores) {
            if (PyUnicode_READY(name) == -1) {
                Py_DECREF(name);
                err = -1;
                break;
            }
            if (PyUnicode_READ_CHAR(name, 0) == '_') {
                Py_DECREF(name);
                continue;
            }
        }
        value = PyObject_GetAttr(v, name);
        if (value == NULL)
            err = -1;
        else if (PyDict_CheckExact(locals))
            err = PyDict_SetItem(locals, name, value);
        else
            err = PyObject_SetItem(locals, name, value);
        Py_DECREF(name);
        Py_XDECREF(value);
        if (err != 0)
            break;
    }
    Py_DECREF(all);
    return err;
}

/*static*/ int
check_args_iterable(PyThreadState *tstate, PyObject *func, PyObject *args)
{
    if (args->ob_type->tp_iter == NULL && !PySequence_Check(args)) {
        _PyErr_Format(tstate, PyExc_TypeError,
                      "%.200s%.200s argument after * "
                      "must be an iterable, not %.200s",
                      PyEval_GetFuncName(func),
                      PyEval_GetFuncDesc(func),
                      args->ob_type->tp_name);
        return -1;
    }
    return 0;
}

/*static*/ void
format_kwargs_error(PyThreadState *tstate, PyObject *func, PyObject *kwargs)
{
    /* _PyDict_MergeEx raises attribute
     * error (percolated from an attempt
     * to get 'keys' attribute) instead of
     * a type error if its second argument
     * is not a mapping.
     */
    if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
        _PyErr_Format(tstate, PyExc_TypeError,
                      "%.200s%.200s argument after ** "
                      "must be a mapping, not %.200s",
                      PyEval_GetFuncName(func),
                      PyEval_GetFuncDesc(func),
                      kwargs->ob_type->tp_name);
    }
    else if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
        PyObject *exc, *val, *tb;
        _PyErr_Fetch(tstate, &exc, &val, &tb);
        if (val && PyTuple_Check(val) && PyTuple_GET_SIZE(val) == 1) {
            PyObject *key = PyTuple_GET_ITEM(val, 0);
            if (!PyUnicode_Check(key)) {
                _PyErr_Format(tstate, PyExc_TypeError,
                              "%.200s%.200s keywords must be strings",
                              PyEval_GetFuncName(func),
                              PyEval_GetFuncDesc(func));
            }
            else {
                _PyErr_Format(tstate, PyExc_TypeError,
                              "%.200s%.200s got multiple "
                              "values for keyword argument '%U'",
                              PyEval_GetFuncName(func),
                              PyEval_GetFuncDesc(func),
                              key);
            }
            Py_XDECREF(exc);
            Py_XDECREF(val);
            Py_XDECREF(tb);
        }
        else {
            _PyErr_Restore(tstate, exc, val, tb);
        }
    }
}

/*static*/ void
format_exc_check_arg(PyThreadState *tstate, PyObject *exc,
                     const char *format_str, PyObject *obj)
{
    const char *obj_str;

    if (!obj)
        return;

    obj_str = PyUnicode_AsUTF8(obj);
    if (!obj_str)
        return;

    _PyErr_Format(tstate, exc, format_str, obj_str);
}

/*static*/ void
format_exc_unbound(PyThreadState *tstate, PyCodeObject *co, int oparg)
{
    PyObject *name;
    /* Don't stomp existing exception */
    if (_PyErr_Occurred(tstate))
        return;
    if (oparg < PyTuple_GET_SIZE(co->co_cellvars)) {
        name = PyTuple_GET_ITEM(co->co_cellvars,
                                oparg);
        format_exc_check_arg(tstate,
            PyExc_UnboundLocalError,
            UNBOUNDLOCAL_ERROR_MSG,
            name);
    } else {
        name = PyTuple_GET_ITEM(co->co_freevars, oparg -
                                PyTuple_GET_SIZE(co->co_cellvars));
        format_exc_check_arg(tstate, PyExc_NameError,
                             UNBOUNDFREE_ERROR_MSG, name);
    }
}

/*static*/ void
format_awaitable_error(PyThreadState *tstate, PyTypeObject *type, int prevopcode)
{
    if (type->tp_as_async == NULL || type->tp_as_async->am_await == NULL) {
        if (prevopcode == BEFORE_ASYNC_WITH) {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "'async with' received an object from __aenter__ "
                          "that does not implement __await__: %.100s",
                          type->tp_name);
        }
        else if (prevopcode == WITH_CLEANUP_START) {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "'async with' received an object from __aexit__ "
                          "that does not implement __await__: %.100s",
                          type->tp_name);
        }
    }
}

static PyObject *
unicode_concatenate(PyThreadState *tstate, PyObject *v, PyObject *w,
                    PyFrameObject *f, const _Py_CODEUNIT *next_instr)
{
    // this is just a dummy for the use of NEXTOPARG() will not get used
    PyObject *res;
    if (Py_REFCNT(v) == 2) {
        /* In the common case, there are 2 references to the value
         * stored in 'variable' when the += is performed: one on the
         * value stack (in 'v') and one still stored in the
         * 'variable'.  We try to delete the variable now to reduce
         * the refcnt to 1.
         */
        int opcode, oparg;
        NEXTOPARG();
        switch (opcode) {
        case STORE_FAST:
        {
            PyObject **fastlocals = f->f_localsplus;
            if (GETLOCAL(oparg) == v)
                SETLOCAL(oparg, NULL);
            break;
        }
        case STORE_DEREF:
        {
            PyObject **freevars = (f->f_localsplus +
                                   f->f_code->co_nlocals);
            PyObject *c = freevars[oparg];
            if (PyCell_GET(c) ==  v) {
                PyCell_SET(c, NULL);
                Py_DECREF(v);
            }
            break;
        }
        case STORE_NAME:
        {
            PyObject *names = f->f_code->co_names;
            PyObject *name = GETITEM(names, oparg);
            PyObject *locals = f->f_locals;
            if (locals && PyDict_CheckExact(locals)) {
                PyObject *w = PyDict_GetItemWithError(locals, name);
                if ((w == v && PyDict_DelItem(locals, name) != 0) ||
                    (w == NULL && _PyErr_Occurred(tstate)))
                {
                    Py_DECREF(v);
                    return NULL;
                }
            }
            break;
        }
        }
    }
    res = v;
    PyUnicode_Append(&res, w);
    return res;
}

#ifdef DYNAMIC_EXECUTION_PROFILE

static PyObject *
getarray(long a[256])
{
    int i;
    PyObject *l = PyList_New(256);
    if (l == NULL) return NULL;
    for (i = 0; i < 256; i++) {
        PyObject *x = PyLong_FromLong(a[i]);
        if (x == NULL) {
            Py_DECREF(l);
            return NULL;
        }
        PyList_SET_ITEM(l, i, x);
    }
    for (i = 0; i < 256; i++)
        a[i] = 0;
    return l;
}
#if 0
PyObject *
_Py_GetDXProfile(PyObject *self, PyObject *args)
{
#ifndef DXPAIRS
    return getarray(dxp);
#else
    int i;
    PyObject *l = PyList_New(257);
    if (l == NULL) return NULL;
    for (i = 0; i < 257; i++) {
        PyObject *x = getarray(dxpairs[i]);
        if (x == NULL) {
            Py_DECREF(l);
            return NULL;
        }
        PyList_SET_ITEM(l, i, x);
    }
    return l;
#endif
}
#endif
#endif

#if 0
Py_ssize_t
_PyEval_RequestCodeExtraIndex(freefunc free)
{
    PyInterpreterState *interp = _PyInterpreterState_GET_UNSAFE();
    Py_ssize_t new_index;

    if (interp->co_extra_user_count == MAX_CO_EXTRA_USERS - 1) {
        return -1;
    }
    new_index = interp->co_extra_user_count++;
    interp->co_extra_freefuncs[new_index] = free;
    return new_index;
}
#endif

static void
dtrace_function_entry(PyFrameObject *f)
{
    const char *filename;
    const char *funcname;
    int lineno;

    filename = PyUnicode_AsUTF8(f->f_code->co_filename);
    funcname = PyUnicode_AsUTF8(f->f_code->co_name);
    lineno = PyCode_Addr2Line(f->f_code, f->f_lasti);

    PyDTrace_FUNCTION_ENTRY(filename, funcname, lineno);
}

static void
dtrace_function_return(PyFrameObject *f)
{
    const char *filename;
    const char *funcname;
    int lineno;

    filename = PyUnicode_AsUTF8(f->f_code->co_filename);
    funcname = PyUnicode_AsUTF8(f->f_code->co_name);
    lineno = PyCode_Addr2Line(f->f_code, f->f_lasti);

    PyDTrace_FUNCTION_RETURN(filename, funcname, lineno);
}

/* DTrace equivalent of maybe_call_line_trace. */
static void
maybe_dtrace_line(PyFrameObject *frame,
                  int *instr_lb, int *instr_ub, int *instr_prev)
{
    int line = frame->f_lineno;
    const char *co_filename, *co_name;

    /* If the last instruction executed isn't in the current
       instruction window, reset the window.
    */
    if (frame->f_lasti < *instr_lb || frame->f_lasti >= *instr_ub) {
        PyAddrPair bounds;
        line = _PyCode_CheckLineNumber(frame->f_code, frame->f_lasti,
                                       &bounds);
        *instr_lb = bounds.ap_lower;
        *instr_ub = bounds.ap_upper;
    }
    /* If the last instruction falls at the start of a line or if
       it represents a jump backwards, update the frame's line
       number and call the trace function. */
    if (frame->f_lasti == *instr_lb || frame->f_lasti < *instr_prev) {
        frame->f_lineno = line;
        co_filename = PyUnicode_AsUTF8(frame->f_code->co_filename);
        if (!co_filename)
            co_filename = "?";
        co_name = PyUnicode_AsUTF8(frame->f_code->co_name);
        if (!co_name)
            co_name = "?";
        PyDTrace_LINE(co_filename, co_name, line);
    }
    *instr_prev = frame->f_lasti;
}


static PyObject *
aot_ceval_test(PyObject *self, PyObject *args)
{
    Py_RETURN_NONE;
}

static void showStats(const char* name, long hits, long misses, long uncached, long warmup) {
    long total = hits + misses + uncached + warmup;
    printf("%ld %s: %.1f%% hits %.1f%% misses %.1f%% uncached %.1f%% warmup\n", total, name,
           100.0 * hits / total, 100.0 * misses / total,
           100.0 * uncached / total, 100.0 * warmup / total);
}
void aot_exit()
{
#if OPCACHE_STATS
    char* env = getenv("SHOW_OPCACHE_STATS");
    if (env && atoi(env)) {
        showStats("LOAD_ATTR", loadattr_hits, loadattr_misses, loadattr_uncached, loadattr_noopcache);
        showStats("STORE_ATTR", storeattr_hits, storeattr_misses, storeattr_uncached, storeattr_noopcache);
        showStats("LOAD_METHOD", loadmethod_hits, loadmethod_misses, loadmethod_uncached, loadmethod_noopcache);
        showStats("LOAD_GLOBAL", loadglobal_hits, loadglobal_misses, loadglobal_uncached, loadglobal_noopcache);
    }
#endif

#if PROFILE_OPCODES
    if (opcode_profile_enabled) {
        // make sure we are not measuring this script
        opcode_profile_enabled = 0;

        const char* pycode =                                                        \
            "DISPLAY_TOP_N_FUNCS = 5\n"                                             \
            "DISPLAY_TOP_N_OPS = 10\n"                                              \
            "profile = list(opcode_profile.items())\n"                              \
            "profile.sort(key=lambda x: x[1]['num_func_called'], reverse=True)\n"   \
            "num_displayed = 0\n"                                                   \
            "for (name, d) in profile:\n"                                           \
            "    if name.startswith('<frozen importlib._bootstrap'):\n"             \
            "       continue\n"                                                     \
            "    times_called = d['num_func_called']\n"                             \
            "    del d['num_func_called']\n"                                        \
            "    print(f'func {name} got {times_called} times called')\n"           \
            "    op_profile = list(d.items())\n"                                    \
            "    op_profile.sort(key=lambda x: x[1], reverse=True)\n"               \
            "    for (op, num) in op_profile[:DISPLAY_TOP_N_OPS]:\n"                \
            "        print(f'  {op:50} {num:>10}')\n"                               \
            "    num_displayed += 1\n"                                              \
            "    if num_displayed >= DISPLAY_TOP_N_FUNCS:\n"                        \
            "        break";

        PyObject* d = PyDict_New();
        PyDict_SetItemString(d, "opcode_profile", opcode_profile_dict);
        PyObject* ret = PyRun_String(pycode, Py_file_input, d, d);
        Py_DECREF(d);
        Py_DECREF(ret);
        Py_DECREF(opcode_profile_dict);
        opcode_profile_dict = NULL;
    }
#endif

    jit_finish();
}
void aot_ceval_opcode_profile(){}

static PyMethodDef aot_cevalMethods[] = {
    {"test",  aot_ceval_test, METH_VARARGS, "Run test"},
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

static struct PyModuleDef aot_cevalmodule = {
    PyModuleDef_HEAD_INIT,
    "aot_ceval",   /* name of module */
    NULL, /* module documentation, may be NULL */
    -1,       /* size of per-interpreter state of the module,
                 or -1 if the module keeps state in global variables. */
    aot_cevalMethods
};

PyMODINIT_FUNC
PyInit_aot_ceval(void)
{
    PyObject *m;

    m = PyModule_Create(&aot_cevalmodule);
    if (m == NULL)
        return NULL;

    jit_start();


#if PROFILE_OPCODES
    opcode_profile_enabled = getenv("PRINT_OP_PROF") != NULL;
#endif
    char* val = getenv("JIT_MIN_RUNS");
    if (val) {
        jit_min_runs = atoll(val);
        // adjust opcache thresholds too because the JIT can only emit efficient
        // code for the caches if the opcache got used a few times.
        if (jit_min_runs / 2 < opcache_min_runs)
            opcache_min_runs = jit_min_runs / 2;
    }
    val = getenv("OPCACHE_MIN_RUNS");
    if (val) {
        opcache_min_runs = atoll(val);
    }

    return m;
}

