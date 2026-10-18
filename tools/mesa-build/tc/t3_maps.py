# T3 (notes/perf-threaded-context.md): Panfrost buffer maps follow the threaded-context rules
# (u_threaded_context.h "Transfer_map rules for buffer mappings") + replace_buffer_storage.
# The TC_TRANSFER_MAP_* bits only ever come from the threaded context: no behaviour change without it.
# usage: python t3_maps.py <mesa root>
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
patch(D + 'pan_resource.c', [
    # rule 2: never infer unsynchronized maps from the valid range when TC says so
    ('   /* Upgrade writes to uninitialized ranges to UNSYNCHRONIZED */\n'
     '   if ((usage & PIPE_MAP_WRITE) && resource->target == PIPE_BUFFER &&\n',
     '   /* Upgrade writes to uninitialized ranges to UNSYNCHRONIZED (exynos-woa TC T3: not when the threaded context\n'
     '    * forbids it - TC_TRANSFER_MAP_NO_INFER_UNSYNCHRONIZED, it tracks the valid range itself) */\n'
     '   if (!(usage & TC_TRANSFER_MAP_NO_INFER_UNSYNCHRONIZED) &&\n'
     '       (usage & PIPE_MAP_WRITE) && resource->target == PIPE_BUFFER &&\n', 1),
    # rule 3: no own invalidation (new BO / shadow copy) - TC invalidates with replace_buffer_storage
    ('   /* Shadowing with separate stencil may require additional accounting.\n'
     '    * Bail in these exotic cases.\n'
     '    */\n'
     '   if (rsrc->separate_stencil || rsrc->shadow_image) {\n',
     '   /* Shadowing with separate stencil may require additional accounting.\n'
     '    * Bail in these exotic cases.\n'
     '    * exynos-woa TC T3: the threaded context forbids the driver\'s own buffer invalidation\n'
     '    * (TC_TRANSFER_MAP_NO_INVALIDATE) - a synchronized map then flushes and waits below.\n'
     '    */\n'
     '   if (rsrc->separate_stencil || rsrc->shadow_image ||\n'
     '       (usage & TC_TRANSFER_MAP_NO_INVALIDATE)) {\n', 1),
    # rule 1: an unsynchronized map from the application thread must not touch the index cache (the driver thread
    # may be reading it); the unmap - always on the driver thread - invalidates the same range
    ('      if (usage & PIPE_MAP_WRITE) {\n'
     '         BITSET_SET(rsrc->valid.data, level);\n'
     '         pan_minmax_cache_invalidate(\n'
     '            rsrc->index_cache, util_format_get_blocksize(rsrc->base.format),\n'
     '            transfer->base.box.x, transfer->base.box.width);\n'
     '      }\n',
     '      if (usage & PIPE_MAP_WRITE) {\n'
     '         BITSET_SET(rsrc->valid.data, level);\n'
     '         /* exynos-woa TC T3: not from the application thread (TC_TRANSFER_MAP_THREADED_UNSYNC) - the driver\n'
     '          * thread may be reading the cache; panfrost_ptr_unmap (driver thread) invalidates the same range */\n'
     '         if (!(usage & TC_TRANSFER_MAP_THREADED_UNSYNC))\n'
     '            pan_minmax_cache_invalidate(\n'
     '               rsrc->index_cache, util_format_get_blocksize(rsrc->base.format),\n'
     '               transfer->base.box.x, transfer->base.box.width);\n'
     '      }\n', 1),
    # replace_buffer_storage, before panfrost_ptr_map
    ('static void *\npanfrost_ptr_map(struct pipe_context *pctx, struct pipe_resource *resource,\n',
     '/* exynos-woa TC T3: threaded-context buffer invalidation - dst takes src\'s storage (always queued, driver\n'
     ' * thread). Same swap as the DISCARD path of panfrost_ptr_map: batches hold their own BO references\n'
     ' * (panfrost_batch_add_bo_old), so dropping dst\'s old BO is safe. Descriptors that reference dst are dirtied\n'
     ' * (0131 panfrost_dirty_state_for_rsrc) and dst\'s index min/max cache describes the old contents - emptied. */\n'
     'void\n'
     'panfrost_replace_buffer_storage(struct pipe_context *pctx, struct pipe_resource *pdst,\n'
     '                                struct pipe_resource *psrc, unsigned minimum_num_rebinds,\n'
     '                                uint32_t rebind_mask, uint32_t delete_buffer_id)\n'
     '{\n'
     '   struct panfrost_context *ctx = pan_context(pctx);\n'
     '   struct panfrost_resource *dst = pan_resource(pdst);\n'
     '   struct panfrost_resource *src = pan_resource(psrc);\n'
     '\n'
     '   assert(pdst->target == PIPE_BUFFER && psrc->target == PIPE_BUFFER);\n'
     '   assert(panfrost_bo_size(src->bo) >= panfrost_bo_size(dst->bo));\n'
     '\n'
     '   panfrost_bo_reference(src->bo);\n'
     '   panfrost_bo_unreference(dst->bo);\n'
     '   dst->bo = src->bo;\n'
     '   dst->plane.base = src->bo->ptr.gpu;\n'
     '   if (dst->index_cache) {\n'
     '      dst->index_cache->size = 0;\n'
     '      dst->index_cache->index = 0;\n'
     '   }\n'
     '   panfrost_dirty_state_for_rsrc(ctx, pdst);\n'
     '}\n'
     '\n'
     'static void *\npanfrost_ptr_map(struct pipe_context *pctx, struct pipe_resource *resource,\n', 1),
])
patch(D + 'pan_resource.h', [
    ('struct panfrost_transfer {\n',
     '/* exynos-woa TC T3 (pan_resource.c) */\n'
     'void panfrost_replace_buffer_storage(struct pipe_context *pctx, struct pipe_resource *pdst,\n'
     '                                     struct pipe_resource *psrc, unsigned minimum_num_rebinds,\n'
     '                                     uint32_t rebind_mask, uint32_t delete_buffer_id);\n\n'
     'struct panfrost_transfer {\n', 1),
])
print('ok')
