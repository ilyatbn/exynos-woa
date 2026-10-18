# T4 (notes/perf-threaded-context.md): Panfrost wraps itself in Gallium's threaded context when asked
# (PIPE_CONTEXT_PREFER_THREADED); d3d10umd asks for it, connects the winsys marshalling (T1) and pumps pending
# driver-thread callbacks at every outermost DDI entry. MALIUMD_NO_TC=1 = diagnosis switch only (AGENTS rule 12).
# usage: python t4_enable.py <mesa root>
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
        assert n >= 1 and (count == 0 or n == count), (path, old[:70], n)
        s = s.replace(old, new) if count == 0 else s.replace(old, new, 1)
    open(p, 'w', newline='').write(s)


D = 'src/gallium/drivers/panfrost/'
patch(D + 'pan_screen.h', [
    ('struct panfrost_screen {\n   struct pipe_screen base;\n   struct panfrost_device dev;\n',
     'struct panfrost_screen {\n   struct pipe_screen base;\n   struct panfrost_device dev;\n'
     '   /* exynos-woa TC T4: the threaded context\'s transfer slabs (threaded_context_create) */\n'
     '   struct slab_parent_pool transfer_pool;\n', 1),
])
p = root + '/' + D + 'pan_screen.h'
s = open(p, newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
if '#include "util/slab.h"' not in s:
    i = s.index('#include')
    s = s[:i] + '#include "util/slab.h"' + nl + s[i:]
    open(p, 'w', newline='').write(s)

patch(D + 'pan_screen.c', [
    ('   struct panfrost_screen *screen = rzalloc(NULL, struct panfrost_screen);\n\n   if (!screen)\n      return NULL;\n',
     '   struct panfrost_screen *screen = rzalloc(NULL, struct panfrost_screen);\n\n   if (!screen)\n      return NULL;\n\n'
     '   /* exynos-woa TC T4 */\n'
     '   slab_create_parent(&screen->transfer_pool, sizeof(struct panfrost_transfer), 16);\n', 1),
    ('   panfrost_resource_screen_destroy(pscreen);\n   panfrost_pool_cleanup(&screen->mempools.bin);\n',
     '   panfrost_resource_screen_destroy(pscreen);\n'
     '   slab_destroy_parent(&screen->transfer_pool);   /* exynos-woa TC T4 */\n'
     '   panfrost_pool_cleanup(&screen->mempools.bin);\n', 1),
])
patch(D + 'pan_context.c', [
    ('   ret = pan_screen(screen)->vtbl.context_init(ctx);\n\n   if (ret)\n      goto failed;\n\n   return gallium;\n',
     '   ret = pan_screen(screen)->vtbl.context_init(ctx);\n\n   if (ret)\n      goto failed;\n\n'
     '   /* exynos-woa TC T4 (notes/perf-threaded-context.md): every call goes through Gallium\'s threaded context and\n'
     '    * Panfrost runs on its driver thread. Synchronous flushes (no create_fence callback); no is_resource_busy, so\n'
     '    * every invalidation of a busy buffer gets new storage (panfrost_replace_buffer_storage). */\n'
     '   if (flags & PIPE_CONTEXT_PREFER_THREADED) {\n'
     '      struct pipe_context *tc = threaded_context_create(gallium, &pan_screen(screen)->transfer_pool,\n'
     '                                                        panfrost_replace_buffer_storage, NULL, NULL);\n'
     '      if (tc && tc != gallium)\n'
     '         ctx->tc_pipe = tc;\n'
     '      return tc;\n'
     '   }\n\n'
     '   return gallium;\n', 1),
])

F = 'src/gallium/frontends/d3d10umd/'
patch(F + 'Device.cpp', [
    ('extern "C" void malikm_tlog(const char *fmt, ...);   /* exynos-woa: malikm_drm.c */\n',
     'extern "C" void malikm_tlog(const char *fmt, ...);   /* exynos-woa: malikm_drm.c */\n'
     '/* exynos-woa TC T4 (notes/perf-threaded-context.md): threaded context hooks (u_threaded_context.c) and the\n'
     ' * winsys runtime-callback marshalling (malikm_drm.c T1) */\n'
     'extern "C" void (*tc_driver_thread_hook)(void);\n'
     'extern "C" void (*tc_wait_hook)(void);\n'
     'extern "C" void malikm_mark_worker_thread(void);\n'
     'extern "C" void malikm_ddi_pump(void);\n'
     'extern "C" void malikm_marshal_enable(void);\n', 1),
    ('   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);\n',
     '   /* exynos-woa TC T4: Panfrost on a driver thread (Gallium threaded context). The runtime callbacks it needs\n'
     '    * there run on this DDI thread (MS Learn pfnSetAsyncCallbacksCb: D3D10+ runtimes allow none from a worker).\n'
     '    * MALIUMD_NO_TC=1 turns it off - diagnosis only (AGENTS rule 12). */\n'
     '   unsigned ctx_flags = 0;\n'
     '   if (!debug_get_bool_option("MALIUMD_NO_TC", false)) {\n'
     '      tc_driver_thread_hook = malikm_mark_worker_thread;\n'
     '      tc_wait_hook = malikm_ddi_pump;\n'
     '      malikm_marshal_enable();\n'
     '      ctx_flags = PIPE_CONTEXT_PREFER_THREADED;\n'
     '   }\n'
     '   struct pipe_context *pipe = screen->context_create(screen, NULL, ctx_flags);\n'
     '   malikm_tlog("device: context %p threaded %d", (void *)pipe, pipe && pipe->priv ? 1 : 0);\n', 1),
])
patch(F + 'Debug.cpp', [
    ('extern "C" void malikm_tlog(const char *fmt, ...);\n',
     'extern "C" void malikm_tlog(const char *fmt, ...);\n'
     'extern "C" void malikm_ddi_pump(void);   /* exynos-woa TC T4: malikm_drm.c T1 */\n', 1),
    ('   if (umd_entry_depth++ == 0) {\n      LARGE_INTEGER q;\n',
     '   if (umd_entry_depth == 0)\n'
     '      malikm_ddi_pump();   /* exynos-woa TC T4: run what the driver thread handed over (cheap when idle) */\n'
     '   if (umd_entry_depth++ == 0) {\n      LARGE_INTEGER q;\n', 1),
])
patch(F + 'DxgiFns.cpp', [
    ('extern "C" void malikm_ioctl_report(char *buf, size_t size, int n);   /* exynos-woa G21: malikm_drm.c */\n',
     'extern "C" void malikm_ioctl_report(char *buf, size_t size, int n);   /* exynos-woa G21: malikm_drm.c */\n'
     'extern "C" void malikm_marshal_report(char *buf, size_t size);   /* exynos-woa TC T4: malikm_drm.c T1 */\n', 1),
    ('            malikm_ioctl_report(top, sizeof top, 8);\n            malikm_tlog("app ioctl top %u: %s", n, top);\n',
     '            malikm_ioctl_report(top, sizeof top, 8);\n            malikm_tlog("app ioctl top %u: %s", n, top);\n'
     '            malikm_marshal_report(top, sizeof top);\n            malikm_tlog("app %s", top);\n', 1),
])
print('ok')
