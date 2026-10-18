# T5a (notes/perf-threaded-context.md): panfrost_tc_sync (T2b) must not sync the threaded context from inside the
# threaded context's own batch execution. u_blitter (ResourceCopy blits, clears) calls ctx->base.create_sampler_view
# on the driver context while a batch executes; the T2b sync then re-entered _tc_sync and executed the running batch
# a second time (x86 tc2: a queued buffer unmap ran twice -> use-after-free in u_transfer_helper_transfer_unmap); on
# the driver thread it would deadlock. Fix: a thread-local "executing a batch" depth in u_threaded_context.c,
# threaded_context_executing(); panfrost_tc_sync syncs only outside it.
# usage: python t5_reentry.py <mesa root>
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


T = 'src/gallium/auxiliary/util/u_threaded_context.c'
patch(T, [
    ('void (*tc_driver_thread_hook)(void);\nvoid (*tc_wait_hook)(void);\n',
     'void (*tc_driver_thread_hook)(void);\nvoid (*tc_wait_hook)(void);\n\n'
     '/* exynos-woa TC T5a: > 0 while this thread executes threaded-context calls (driver thread, or the application\n'
     ' * thread executing unflushed calls in _tc_sync) - the driver must not sync the threaded context from there */\n'
     'static _Thread_local int tc_executing_depth;\n\n'
     'bool\nthreaded_context_executing(void)\n{\n   return tc_executing_depth > 0;\n}\n', 1),
    ('   if (batch->tc->options.parse_renderpass_info) {\n      batch_execute(batch, pipe, true);\n',
     '   tc_executing_depth++;   /* exynos-woa TC T5a */\n'
     '   if (batch->tc->options.parse_renderpass_info) {\n      batch_execute(batch, pipe, true);\n', 1),
    ('   } else {\n      batch_execute(batch, pipe, false);\n   }\n',
     '   } else {\n      batch_execute(batch, pipe, false);\n   }\n'
     '   tc_executing_depth--;   /* exynos-woa TC T5a */\n', 1),
])
patch('src/gallium/auxiliary/util/u_threaded_context.h', [
    ('struct pipe_context *threaded_context_unwrap_sync(struct pipe_context *pipe);\n',
     'struct pipe_context *threaded_context_unwrap_sync(struct pipe_context *pipe);\n'
     '/* exynos-woa TC T5a: true while the calling thread executes threaded-context calls */\n'
     'bool threaded_context_executing(void);\n', 1),
])
patch('src/gallium/drivers/panfrost/pan_context.h', [
    ('   if (ctx->tc_pipe)\n      threaded_context_unwrap_sync(ctx->tc_pipe);\n',
     '   /* exynos-woa TC T5a: not from inside the threaded context\'s own execution (u_blitter calls\n'
     '    * create_sampler_view on the driver context during a blit) - re-entering _tc_sync re-executed the running\n'
     '    * batch; the driver thread would deadlock */\n'
     '   if (ctx->tc_pipe && !threaded_context_executing())\n      threaded_context_unwrap_sync(ctx->tc_pipe);\n', 1),
])
print('ok')
