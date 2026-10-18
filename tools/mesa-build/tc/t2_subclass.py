# T2 (notes/perf-threaded-context.md): Panfrost resource/transfer/query subclass the threaded-context structs;
# transfers and queries leave the context's ralloc tree (TC calls create_query and unsynchronized maps from the
# application thread, concurrently with the driver thread); create_query zeroes its buffer through the BO instead
# of pipe_buffer_write on the driver context. No behaviour change without a threaded context.
# usage: python t2_subclass.py <mesa root>
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
patch(D + 'pan_resource.h', [
    ('#include "util/u_range.h"\n', '#include "util/u_range.h"\n#include "util/u_threaded_context.h"\n', 1),
    ('struct panfrost_resource {\n   struct pipe_resource base;\n',
     'struct panfrost_resource {\n'
     '   /* exynos-woa TC T2: threaded_resource starts with the pipe_resource (tc.b aliases base) */\n'
     '   union {\n      struct pipe_resource base;\n      struct threaded_resource tc;\n   };\n', 1),
    ('struct panfrost_transfer {\n   struct pipe_transfer base;\n',
     'struct panfrost_transfer {\n'
     '   /* exynos-woa TC T2: threaded_transfer starts with the pipe_transfer (tc.b aliases base) */\n'
     '   union {\n      struct pipe_transfer base;\n      struct threaded_transfer tc;\n   };\n', 1),
])
patch(D + 'pan_context.h', [
    ('struct panfrost_query {\n   /* Passthrough from Gallium */\n',
     'struct panfrost_query {\n'
     '   /* exynos-woa TC T2: the threaded context keeps its own state at the start of every query */\n'
     '   struct threaded_query tc;\n\n'
     '   /* Passthrough from Gallium */\n', 1),
])
patch(D + 'pan_resource.c', [
    # resource_from_handle
    ('   prsc = &rsc->base;\n\n   *prsc = *templat;\n\n   pipe_reference_init(&prsc->reference, 1);\n'
     '   util_range_init(&rsc->valid_buffer_range);\n',
     '   prsc = &rsc->base;\n\n   *prsc = *templat;\n\n   pipe_reference_init(&prsc->reference, 1);\n'
     '   util_range_init(&rsc->valid_buffer_range);\n'
     '   threaded_resource_init(prsc, false);   /* exynos-woa TC T2 */\n', 1),
    # create_with_modifier
    ('   pipe_reference_init(&so->base.reference, 1);\n\n   util_range_init(&so->valid_buffer_range);\n',
     '   pipe_reference_init(&so->base.reference, 1);\n\n   util_range_init(&so->valid_buffer_range);\n'
     '   threaded_resource_init(&so->base, false);   /* exynos-woa TC T2 */\n', 1),
    # destroy
    ('   util_range_destroy(&rsrc->valid_buffer_range);\n   free(rsrc);\n}\n',
     '   util_range_destroy(&rsrc->valid_buffer_range);\n'
     '   threaded_resource_deinit(&rsrc->base);   /* exynos-woa TC T2 */\n   free(rsrc);\n}\n', 1),
    # transfer: own ralloc root (TC maps unsynchronized buffers from the application thread)
    ('   struct panfrost_transfer *transfer = rzalloc(pctx, struct panfrost_transfer);\n',
     '   /* exynos-woa TC T2: not a child of the context - the threaded context maps unsynchronized buffers from the\n'
     '    * application thread while the driver thread allocates from the context */\n'
     '   struct panfrost_transfer *transfer = rzalloc(NULL, struct panfrost_transfer);\n', 1),
])
patch(D + 'pan_context.c', [
    ('   struct panfrost_query *q = rzalloc(pipe, struct panfrost_query);\n',
     '   /* exynos-woa TC T2: create_query runs on the application thread under the threaded context - no child of\n'
     '    * the context (ralloc is not thread-safe) */\n'
     '   struct panfrost_query *q = rzalloc(NULL, struct panfrost_query);\n', 1),
    ('      /* Default to 0 if nothing at all drawn. */\n      uint8_t *zeroes = alloca(size);\n'
     '      memset(zeroes, 0, size);\n      pipe_buffer_write(pipe, q->rsrc, 0, size, zeroes);\n',
     '      /* Default to 0 if nothing at all drawn. exynos-woa TC T2: through the fresh (idle, unshared) BO, not\n'
     '       * pipe_buffer_write on the context - this may run on the application thread beside the driver thread. */\n'
     '      struct panfrost_resource *qr = pan_resource(q->rsrc);\n'
     '      if (panfrost_bo_mmap(qr->bo)) {\n'
     '         pipe_resource_reference(&q->rsrc, NULL);\n'
     '         ralloc_free(q);\n'
     '         return NULL;\n'
     '      }\n'
     '      memset(qr->bo->ptr.cpu, 0, size);\n', 1),
])
print('ok')
