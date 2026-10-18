# T2b (notes/perf-threaded-context.md): Panfrost functions the threaded context calls directly from the application
# thread (u_threaded_context.h "Thread-safety requirements on context functions") must not use the context's pools,
# batches or ralloc tree while the driver thread runs. ctx->tc_pipe (set in T4, NULL before) = the wrapping context.
# usage: python t2b_direct.py <mesa root>
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
patch(D + 'pan_context.h', [
    ('struct panfrost_context {\n   /* Gallium context */\n   struct pipe_context base;\n',
     'struct panfrost_context {\n   /* Gallium context */\n   struct pipe_context base;\n\n'
     '   /* exynos-woa TC: the threaded context wrapping this one (NULL = not threaded) */\n'
     '   struct pipe_context *tc_pipe;\n', 1),
])
# helper after the struct: find the end of the header's include block is fragile; append before the last #endif
p = root + '/' + D + 'pan_context.h'
s = open(p, newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
helper = (nl + '/* exynos-woa TC T2b: the threaded context calls create_sampler_view, create_*_state, get_query_result'
          ' (flushed) ...' + nl +
          ' * directly from the application thread (u_threaded_context.h "Thread-safety requirements"); a function'
          ' that needs' + nl +
          ' * the context\'s pools or batches there drains the driver thread first. No-op without a threaded context. */'
          + nl + 'static inline void' + nl + 'panfrost_tc_sync(struct panfrost_context *ctx)' + nl + '{' + nl +
          '   if (ctx->tc_pipe)' + nl + '      threaded_context_unwrap_sync(ctx->tc_pipe);' + nl + '}' + nl)
i = s.rindex('#endif')
s = s[:i] + helper.lstrip(nl) + nl + s[i:]
if '#include "util/u_threaded_context.h"' not in s:
    s = s.replace('#include "util/u_blitter.h"' + nl,
                  '#include "util/u_blitter.h"' + nl + '#include "util/u_threaded_context.h"' + nl, 1)
open(p, 'w', newline='').write(s)

patch(D + 'pan_cmdstream.c', [
    ('   struct panfrost_context *ctx = pan_context(pctx);\n   struct panfrost_sampler_view *so =\n'
     '      rzalloc(pctx, struct panfrost_sampler_view);\n   struct panfrost_resource *ptexture = pan_resource(texture);\n\n'
     '   pan_legalize_format(ctx, ptexture, template->format, false, false);\n',
     '   struct panfrost_context *ctx = pan_context(pctx);\n'
     '   /* exynos-woa TC T2b: own ralloc root - sampler_view_destroy runs on the application thread under TC */\n'
     '   struct panfrost_sampler_view *so =\n'
     '      rzalloc(NULL, struct panfrost_sampler_view);\n   struct panfrost_resource *ptexture = pan_resource(texture);\n\n'
     '   /* exynos-woa TC T2b: legalizing may convert the resource and the descriptor comes from ctx->descs - both\n'
     '    * need the driver thread idle (view creation is rare) */\n'
     '   panfrost_tc_sync(ctx);\n'
     '   pan_legalize_format(ctx, ptexture, template->format, false, false);\n', 1),
])
patch(D + 'pan_context.c', [
    ('   target = &rzalloc(pctx, struct panfrost_streamout_target)->base;\n',
     '   /* exynos-woa TC T2b: own ralloc root - created/destroyed on the application thread under TC */\n'
     '   target = &rzalloc(NULL, struct panfrost_streamout_target)->base;\n', 1),
])
# get_query_result: a flushed query (TC calls it from the application thread without syncing) has no batch to flush
p = root + '/' + D + 'pan_context.c'
s = open(p, newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
a = s.index(nl + 'panfrost_get_query_result(')
b = s.index(nl + '}' + nl, a)
body = s[a:b]
cnt = 0
for call in ['panfrost_flush_writer(ctx, rsrc, "Occlusion query");',
             'panfrost_flush_writer(ctx, rsrc, "Timestamp query");',
             'panfrost_flush_writer(ctx, rsrc, "Time elapsed query");',
             'panfrost_flush_all_batches(ctx, "Primitive count query");']:
    assert body.count(call) == 1, call
    body = body.replace(call, '/* exynos-woa TC T2b */ if (!tc_flushed) ' + call, 1)
    cnt += 1
anchor = '   struct panfrost_device *dev = pan_device(ctx->base.screen);'
assert body.count(anchor) == 1
body = body.replace(anchor, anchor + nl +
                    '   /* exynos-woa TC T2b: a query the threaded context marked flushed is read from the application'
                    ' thread' + nl + '    * without a sync; its batches were submitted by that flush - nothing to'
                    ' flush, and ctx must not be touched */' + nl +
                    '   const bool tc_flushed = ctx->tc_pipe && ((struct panfrost_query *)q)->tc.flushed;', 1)
s = s[:a] + body + s[b:]
open(p, 'w', newline='').write(s)
print('query flushes guarded:', cnt)

patch(D + 'pan_shader.c', [
    ('   struct panfrost_context *ctx = pan_context(pctx);\n\n   if (so->nir->xfb_info) {\n',
     '   struct panfrost_context *ctx = pan_context(pctx);\n\n   if (so->nir->xfb_info) {\n'
     '      panfrost_tc_sync(ctx);   /* exynos-woa TC T2b: ctx->shaders / ctx->descs pools */\n', 1),
    ('   /* Creating a CSO is single-threaded, so it\'s ok to use the\n'
     '    * locked function without explicitly taking the lock. Creating a\n'
     '    * default variant acts as a precompile.\n'
     '    */\n   panfrost_new_variant_locked(ctx, so, &key);\n',
     '   /* Creating a CSO is single-threaded, so it\'s ok to use the\n'
     '    * locked function without explicitly taking the lock. Creating a\n'
     '    * default variant acts as a precompile.\n'
     '    * exynos-woa TC T2b: not under the threaded context - this runs on the application thread and the\n'
     '    * variant comes from the context\'s pools; the driver thread compiles it at first use instead.\n'
     '    */\n   if (!ctx->tc_pipe)\n      panfrost_new_variant_locked(ctx, so, &key);\n', 1),
    ('   assert(cso->ir_type == PIPE_SHADER_IR_NIR && "TGSI kernels unsupported");\n\n'
     '   panfrost_shader_get(pctx->screen, &ctx->shaders, &ctx->descs, so,\n',
     '   assert(cso->ir_type == PIPE_SHADER_IR_NIR && "TGSI kernels unsupported");\n\n'
     '   panfrost_tc_sync(ctx);   /* exynos-woa TC T2b: ctx->shaders / ctx->descs pools */\n'
     '   panfrost_shader_get(pctx->screen, &ctx->shaders, &ctx->descs, so,\n', 1),
])
print('ok')

# link_shader: tc_link_shader calls it directly from the application thread; it is only a precompile from the
# context's pools - skipped under the threaded context (the driver thread compiles at bind / first draw)
patch(D + 'pan_shader.c', [
    ('panfrost_link_shader(struct pipe_context *pctx, void** handles)\n{\n   struct panfrost_context *ctx = pan_context(pctx);\n',
     'panfrost_link_shader(struct pipe_context *pctx, void** handles)\n{\n   struct panfrost_context *ctx = pan_context(pctx);\n'
     '   if (ctx->tc_pipe)\n      return;   /* exynos-woa TC T2b: application thread, context pools - precompile skipped */\n', 1),
])
print('link_shader ok')
