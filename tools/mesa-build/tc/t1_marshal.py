# T1 (notes/perf-threaded-context.md): winsys runtime-callback marshalling + threaded-context wait hooks.
# usage: python t1_marshal.py <mesa root>   (applied once; exported as a tools/mesa-build/patches entry afterwards)
import sys
root = sys.argv[1]


def patch(path, pairs):
    p = root + '/' + path
    s = open(p, newline='').read()
    nl = '\r\n' if '\r\n' in s else '\n'
    for old, new, count in pairs:
        old = old.replace('\n', nl)
        new = new.replace('\n', nl)
        n = s.count(old)
        assert n >= 1 and (count == 0 or n == count), (path, old[:60], n)
        s = s.replace(old, new) if count == 0 else s.replace(old, new, 1)
    open(p, 'w', newline='').write(s)


W = 'include/win32-compat/malikm_drm.c'
MK_HEAD = r'''static SRWLOCK wddm_lock;

/* ---- exynos-woa threaded context (notes/perf-threaded-context.md T1): runtime callbacks only on the DDI thread ----
 * MS Learn pfnSetAsyncCallbacksCb: the D3D10+ runtimes set it to NULL, and "Drivers can call the runtime's callback
 * functions only from a worker thread if the runtime previously set the pfnSetAsyncCallbacksCb member of
 * D3DDDI_DEVICECALLBACKS to non-NULL". Under Gallium's threaded context Panfrost runs on a worker thread, so a w_*
 * call made there (when a runtime device exists) is handed to the thread inside the DDI: calls with a result post a
 * request and wait, destroy/unlock (no result) are queued in order. The DDI thread runs them (malikm_ddi_pump) at
 * every DDI entry, while it waits for wddm_lock and inside the threaded context's waits; it drains the queue before
 * any callback of its own, so the runtime sees the calls in program order, one thread at a time.
 * Inert until malikm_marshal_enable(): without a worker every call is direct, as before. */
enum mk_op { MK_ESCAPE, MK_CREATE, MK_RESIDENT, MK_LOCK, MK_RENDER, MK_DESTROY, MK_UNLOCK, MK_NOPS };
struct mk_req {
   enum mk_op op;
   void *p;
   UINT u1, u2;
   D3DKMT_HANDLE h;
   NTSTATUS st;
   void *ret;
   volatile LONG done;
};
static __declspec(thread) int mk_worker;       /* this thread is a threaded-context driver thread */
static volatile LONG mk_on;
static struct mk_req *volatile mk_box;         /* the one pending request with a result */
static HANDLE mk_done_ev;
static SRWLOCK mk_call_lock = SRWLOCK_INIT;    /* one posting driver thread at a time */
static SRWLOCK mk_defer_lock = SRWLOCK_INIT;
static struct mk_req *mk_defer;
static volatile LONG mk_defer_n;
static LONG mk_defer_cap;
static volatile LONG mk_stats[MK_NOPS];
void malikm_ddi_pump(void);

static void
wddm_lock_acquire(void)
{
   if (!mk_on || mk_worker) {
      AcquireSRWLockExclusive(&wddm_lock);
      return;
   }
   /* the driver thread may hold the lock while it waits for this thread to run its callback */
   for (unsigned n = 0; !TryAcquireSRWLockExclusive(&wddm_lock); n++) {
      malikm_ddi_pump();
      if (n < 64)
         YieldProcessor();
      else
         SwitchToThread();
   }
}
/* ---- end of the T1 head; every wddm_lock acquisition below uses wddm_lock_acquire ---- */
'''
patch(W, [('static SRWLOCK wddm_lock;\n', MK_HEAD, 1)])

# every later wddm_lock acquisition goes through the pumping helper
p = root + '/' + W
s = open(p, newline='').read()
cut = s.index('/* ---- end of the T1 head;')
pre, post = s[:cut], s[cut:]
n = post.count('AcquireSRWLockExclusive(&wddm_lock);')
post = post.replace('AcquireSRWLockExclusive(&wddm_lock);', 'wddm_lock_acquire();')
open(p, 'w', newline='').write(pre + post)
print('wddm_lock acquisitions routed:', n)

# raw functions
patch(W, [
    ('static NTSTATUS\nw_escape(void *data, UINT size)\n{', 'static NTSTATUS\nw_escape_raw(void *data, UINT size)\n{', 1),
    ('static NTSTATUS\nw_create(D3DDDI_ALLOCATIONINFO *ai)\n{', 'static NTSTATUS\nw_create_raw(D3DDDI_ALLOCATIONINFO *ai)\n{', 1),
    ('static void\nw_destroy(D3DKMT_HANDLE h)\n{', 'static void\nw_destroy_raw(D3DKMT_HANDLE h)\n{', 1),
    ('static void\nw_resident(D3DKMT_HANDLE h)\n{', 'static void\nw_resident_raw(D3DKMT_HANDLE h)\n{', 1),
    ('static void *\nw_lock(D3DKMT_HANDLE h)\n{', 'static void *\nw_lock_raw(D3DKMT_HANDLE h)\n{', 1),
    ('static void\nw_unlock(D3DKMT_HANDLE h)\n{', 'static void\nw_unlock_raw(D3DKMT_HANDLE h)\n{', 1),
    ('static NTSTATUS\nw_render(UINT commandLength, UINT allocations)\n{',
     'static NTSTATUS\nw_render_raw(UINT commandLength, UINT allocations)\n{', 1),
])

WRAPPERS = r'''
/* ---- exynos-woa T1: the marshalling wrappers (see wddm_lock_acquire) ---- */
static void
mk_exec(struct mk_req *r)
{
   switch (r->op) {
   case MK_ESCAPE:   r->st = w_escape_raw(r->p, r->u1); break;
   case MK_CREATE:   r->st = w_create_raw(r->p); break;
   case MK_RESIDENT: w_resident_raw(r->h); break;
   case MK_LOCK:     r->ret = w_lock_raw(r->h); break;
   case MK_RENDER:   r->st = w_render_raw(r->u1, r->u2); break;
   case MK_DESTROY:  w_destroy_raw(r->h); break;
   case MK_UNLOCK:   w_unlock_raw(r->h); break;
   default: break;
   }
}

static void
mk_drain(void)
{
   if (!mk_defer_n)
      return;
   AcquireSRWLockExclusive(&mk_defer_lock);
   for (LONG i = 0; i < mk_defer_n; i++)
      mk_exec(&mk_defer[i]);
   mk_defer_n = 0;
   ReleaseSRWLockExclusive(&mk_defer_lock);
}

void
malikm_ddi_pump(void)
{
   if (mk_worker || (!mk_box && !mk_defer_n))
      return;
   mk_drain();
   struct mk_req *r = InterlockedExchangePointer((PVOID volatile *)&mk_box, NULL);
   if (r) {
      mk_exec(r);
      InterlockedExchange(&r->done, 1);
      SetEvent(mk_done_ev);
   }
}

/* on a driver thread with a runtime device: hand the call over */
static int
mk_marshal(void)
{
   return mk_worker && mk_on && ddi.on;
}

/* a direct call on a DDI thread: the driver thread's queued destroy/unlock go first */
static void
mk_direct(void)
{
   if (mk_on && !mk_worker)
      mk_drain();
}

static void
mk_defer_op(enum mk_op op, D3DKMT_HANDLE h)
{
   AcquireSRWLockExclusive(&mk_defer_lock);
   if (mk_defer_n == mk_defer_cap) {
      LONG cap = mk_defer_cap ? mk_defer_cap * 2 : 256;
      struct mk_req *n = realloc(mk_defer, cap * sizeof(*n));
      if (!n) {
         ReleaseSRWLockExclusive(&mk_defer_lock);
         malikm_tlog("tc: deferred callback queue full (%ld) - op %d handle 0x%x dropped", (long)mk_defer_n, op, h);
         return;
      }
      mk_defer = n;
      mk_defer_cap = cap;
   }
   memset(&mk_defer[mk_defer_n], 0, sizeof(mk_defer[0]));
   mk_defer[mk_defer_n].op = op;
   mk_defer[mk_defer_n].h = h;
   mk_defer_n++;
   ReleaseSRWLockExclusive(&mk_defer_lock);
   InterlockedIncrement(&mk_stats[op]);
}

static void
mk_call(struct mk_req *r)
{
   AcquireSRWLockExclusive(&mk_call_lock);
   r->done = 0;
   InterlockedIncrement(&mk_stats[r->op]);
   InterlockedExchangePointer((PVOID volatile *)&mk_box, r);
   DWORD t0 = GetTickCount();
   int logged = 0;
   while (!r->done) {
      WaitForSingleObject(mk_done_ev, 500);
      if (!r->done && !logged && GetTickCount() - t0 > 2000) {
         malikm_tlog("tc: driver thread waits > 2 s for the DDI thread to run callback op %d", r->op);
         logged = 1;
      }
   }
   ReleaseSRWLockExclusive(&mk_call_lock);
}

static NTSTATUS
w_escape(void *data, UINT size)
{
   if (mk_marshal()) {
      struct mk_req r = { MK_ESCAPE, data, size };
      mk_call(&r);
      return r.st;
   }
   mk_direct();
   return w_escape_raw(data, size);
}

static NTSTATUS
w_create(D3DDDI_ALLOCATIONINFO *ai)
{
   if (mk_marshal()) {
      struct mk_req r = { MK_CREATE, ai };
      mk_call(&r);
      return r.st;
   }
   mk_direct();
   return w_create_raw(ai);
}

static void
w_resident(D3DKMT_HANDLE h)
{
   if (mk_marshal()) {
      struct mk_req r = { MK_RESIDENT };
      r.h = h;
      mk_call(&r);
      return;
   }
   mk_direct();
   w_resident_raw(h);
}

static void *
w_lock(D3DKMT_HANDLE h)
{
   if (mk_marshal()) {
      struct mk_req r = { MK_LOCK };
      r.h = h;
      mk_call(&r);
      return r.ret;
   }
   mk_direct();
   return w_lock_raw(h);
}

static NTSTATUS
w_render(UINT commandLength, UINT allocations)
{
   if (mk_marshal()) {
      struct mk_req r = { MK_RENDER, NULL, commandLength, allocations };
      mk_call(&r);
      return r.st;
   }
   mk_direct();
   return w_render_raw(commandLength, allocations);
}

static void
w_destroy(D3DKMT_HANDLE h)
{
   if (mk_marshal()) {
      mk_defer_op(MK_DESTROY, h);
      return;
   }
   mk_direct();
   w_destroy_raw(h);
}

static void
w_unlock(D3DKMT_HANDLE h)
{
   if (mk_marshal()) {
      mk_defer_op(MK_UNLOCK, h);
      return;
   }
   mk_direct();
   w_unlock_raw(h);
}

/* exported for d3d10umd: enable once the threaded context exists (before its driver thread runs) */
void
malikm_marshal_enable(void)
{
   if (!mk_done_ev)
      mk_done_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
   if (mk_done_ev)
      InterlockedExchange(&mk_on, 1);
}

void
malikm_mark_worker_thread(void)
{
   mk_worker = 1;
}

/* "tc callbacks: escape N create N ..." since the last call (counts reset) */
void
malikm_marshal_report(char *buf, size_t size)
{
   static const char *names[MK_NOPS] = { "escape", "create", "resident", "lock", "render", "destroy", "unlock" };
   int o = snprintf(buf, size, "tc callbacks:");
   for (int i = 0; i < MK_NOPS && o > 0 && (size_t)o < size; i++)
      o += snprintf(buf + o, size - o, " %s %ld", names[i], (long)InterlockedExchange(&mk_stats[i], 0));
}

/* Everything the KMD was given has completed: its fences agree and nothing
 * is queued or running. For when there is no event to wait on. */
static int
wddm_poll_idle(void)
'''
patch(W, [('\n/* Everything the KMD was given has completed: its fences agree and nothing\n'
           ' * is queued or running. For when there is no event to wait on. */\nstatic int\nwddm_poll_idle(void)\n',
           WRAPPERS, 1)])

# threaded context: driver-thread mark + pumping waits
T = 'src/gallium/auxiliary/util/u_threaded_context.c'
TC_HOOKS = '''static void
tc_batch_execute(void *job, UNUSED void *gdata, int thread_index);

/* exynos-woa (notes/perf-threaded-context.md T1): the Windows UMD may not call D3D runtime callbacks from the
 * driver thread; its winsys hands them to the application thread, which has to service them while it waits for
 * the driver thread. tc_driver_thread_hook marks the queue thread, tc_wait_hook services pending callbacks; both
 * NULL = upstream behaviour. */
void (*tc_driver_thread_hook)(void);
void (*tc_wait_hook)(void);

static void
tc_fence_wait(struct util_queue_fence *fence)
{
   if (!tc_wait_hook) {
      util_queue_fence_wait(fence);
      return;
   }
   for (;;) {
      tc_wait_hook();
      if (util_queue_fence_wait_timeout(fence, os_time_get_absolute_timeout(1000000)))
         return;
   }
}

static void
tc_batch_execute_queued(void *job, void *gdata, int thread_index)
{
   if (tc_driver_thread_hook)
      tc_driver_thread_hook();
   tc_batch_execute(job, gdata, thread_index);
}
'''
patch(T, [
    ('static void\ntc_batch_execute(void *job, UNUSED void *gdata, int thread_index);\n', TC_HOOKS, 1),
    ('      /* always wait on the batch to finish since this will otherwise overwrite thread data */\n'
     '      util_queue_fence_wait(&batch->fence);',
     '      /* always wait on the batch to finish since this will otherwise overwrite thread data */\n'
     '      tc_fence_wait(&batch->fence);', 1),
    ('   if (!util_queue_fence_is_signalled(&last->fence)) {\n      util_queue_fence_wait(&last->fence);\n      synced = true;',
     '   if (!util_queue_fence_is_signalled(&last->fence)) {\n      tc_fence_wait(&last->fence);\n      synced = true;', 1),
    ('   tc_update_batch_generation(tc, next);\n   util_queue_add_job(&tc->queue, next, &next->fence, tc_batch_execute,\n'
     '                      NULL, 0);',
     '   tc_update_batch_generation(tc, next);\n'
     '   /* exynos-woa T1: util_queue_add_job blocks (without servicing callbacks) once TC_MAX_BATCHES - 2 jobs are\n'
     '    * queued; the oldest one that can still be queued is two slots ahead - wait for it here instead. */\n'
     '   if (tc_wait_hook)\n'
     '      tc_fence_wait(&tc->batch_slots[(tc->next + 2) % TC_MAX_BATCHES].fence);\n'
     '   util_queue_add_job(&tc->queue, next, &next->fence, tc_batch_execute_queued,\n'
     '                      NULL, 0);', 1),
    ('   while (1) {\n      util_queue_fence_wait(&info->ready);', '   while (1) {\n      tc_fence_wait(&info->ready);', 1),
])
print('ok')
