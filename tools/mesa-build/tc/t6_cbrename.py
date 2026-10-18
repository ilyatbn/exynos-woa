# T6 (notes/perf-threaded-context.md): d3d10umd renames DYNAMIC constant buffers into pipe->const_uploader at every
# WRITE_DISCARD map - a suballocation instead of a new resource (threaded context invalidation: 32 us per map in
# NFSU2, 1036 maps/frame) or Panfrost's BO swap. Every slot the buffer is bound to is rebound with buffer+offset.
# Copies, UpdateSubresource and any other map first copy the current contents back into the resource (materialize).
# usage: python t6_cbrename.py <mesa root>
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


F = 'src/gallium/frontends/d3d10umd/'
patch(F + 'State.h', [
    ('   /* exynos-woa G24: D3D11 structure byte stride (D3D11DDIARG_CREATERESOURCE::ByteStride), for structured views */\n'
     '   unsigned byte_stride;\n};\n',
     '   /* exynos-woa G24: D3D11 structure byte stride (D3D11DDIARG_CREATERESOURCE::ByteStride), for structured views */\n'
     '   unsigned byte_stride;\n'
     '   /* exynos-woa T6 (notes/perf-threaded-context.md): a DYNAMIC constant buffer is renamed into pipe->const_uploader\n'
     '    * at every WRITE_DISCARD map. cb_buf/cb_offset = where its contents live now (NULL = in `resource`);\n'
     '    * cb_mapped = a renamed map is open (Unmap flushes the uploader). */\n'
     '   bool cb_rename;\n'
     '   bool cb_mapped;\n'
     '   struct pipe_resource *cb_buf;\n'
     '   unsigned cb_offset;\n'
     '};\n', 1),
    ('   struct pipe_constant_buffer tracked_cb[2][GS_EMU_TRACKED_CBS];\n',
     '   struct pipe_constant_buffer tracked_cb[2][GS_EMU_TRACKED_CBS];\n'
     '   /* exynos-woa T6: the constant buffers bound per stage (VS, PS, GS, CS) - renamed ones are rebound at Map */\n'
     '   struct Resource *bound_cb[4][PIPE_MAX_CONSTANT_BUFFERS];\n', 1),
    ('static inline struct pipe_resource *\nCastPipeResource(D3D10DDI_HRESOURCE hResource)\n',
     '/* exynos-woa T6 (Resource.cpp): bind a constant buffer (its current storage), rebind every slot of a renamed one,\n'
     ' * copy a renamed one back into its resource */\n'
     'void CbBind(Device *dev, mesa_shader_stage stage, unsigned slot, Resource *res);\n'
     'void CbRebindAll(Device *dev, Resource *res);\n'
     'void CbMaterialize(Device *dev, Resource *res);\n\n'
     'static inline struct pipe_resource *\nCastPipeResource(D3D10DDI_HRESOURCE hResource)\n', 1),
])
patch(F + 'Shader.cpp', [
    ('   for (UINT i = 0; i < NumBuffers; i++) {\n'
     '      struct pipe_constant_buffer cb;\n'
     '      memset(&cb, 0, sizeof cb);\n'
     '      cb.buffer = CastPipeResource(phBuffers[i]);\n'
     '      cb.buffer_offset = 0;\n'
     '      cb.buffer_size = cb.buffer ? cb.buffer->width0 : 0;\n'
     '      pipe->set_constant_buffer(pipe,\n'
     '                                shader_type,\n'
     '                                StartBuffer + i,\n'
     '                                &cb);\n'
     '      DeviceTrackConstantBuffer(pDevice, shader_type, StartBuffer + i, &cb);   /* exynos-woa G25 */\n'
     '   }\n',
     '   (void)pipe;\n'
     '   for (UINT i = 0; i < NumBuffers; i++)\n'
     '      CbBind(pDevice, shader_type, StartBuffer + i, CastResource(phBuffers[i]));   /* exynos-woa T6 (+G25 tracking) */\n', 1),
])
patch(F + 'Compute.cpp', [
    ('   for (UINT i = 0; i < NumBuffers; i++) {\n'
     '      struct pipe_constant_buffer cb;\n'
     '      memset(&cb, 0, sizeof cb);\n'
     '      cb.buffer = CastPipeResource(phBuffers[i]);\n'
     '      cb.buffer_size = cb.buffer ? cb.buffer->width0 : 0;\n'
     '      pDevice->pipe->set_constant_buffer(pDevice->pipe, MESA_SHADER_COMPUTE, StartSlot + i, &cb);\n'
     '      DeviceTrackConstantBuffer(pDevice, MESA_SHADER_COMPUTE, StartSlot + i, &cb);   /* exynos-woa G25 */\n'
     '   }\n',
     '   for (UINT i = 0; i < NumBuffers; i++)\n'
     '      CbBind(pDevice, MESA_SHADER_COMPUTE, StartSlot + i, CastResource(phBuffers[i]));   /* exynos-woa T6 (+G25) */\n', 1),
])

HELPERS = r'''

/* ---- exynos-woa T6 (notes/perf-threaded-context.md): DYNAMIC constant buffer renaming ---- */
static int
CbStage(mesa_shader_stage stage)
{
   switch (stage) {
   case MESA_SHADER_VERTEX:   return 0;
   case MESA_SHADER_FRAGMENT: return 1;
   case MESA_SHADER_GEOMETRY: return 2;
   case MESA_SHADER_COMPUTE:  return 3;
   default:                   return -1;
   }
}

void
CbBind(Device *dev, mesa_shader_stage stage, unsigned slot, Resource *res)
{
   struct pipe_constant_buffer cb;
   memset(&cb, 0, sizeof cb);
   if (res && res->resource) {
      cb.buffer = res->cb_buf ? res->cb_buf : res->resource;
      cb.buffer_offset = res->cb_buf ? res->cb_offset : 0;
      cb.buffer_size = res->resource->width0;
   }
   dev->pipe->set_constant_buffer(dev->pipe, stage, slot, &cb);
   DeviceTrackConstantBuffer(dev, stage, slot, &cb);   /* exynos-woa G25 */
   int st = CbStage(stage);
   if (st >= 0 && slot < PIPE_MAX_CONSTANT_BUFFERS)
      dev->bound_cb[st][slot] = res;
}

void
CbRebindAll(Device *dev, Resource *res)
{
   static const mesa_shader_stage stages[4] = { MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT, MESA_SHADER_GEOMETRY,
                                                MESA_SHADER_COMPUTE };
   for (unsigned st = 0; st < 4; st++)
      for (unsigned slot = 0; slot < PIPE_MAX_CONSTANT_BUFFERS; slot++)
         if (dev->bound_cb[st][slot] == res)
            CbBind(dev, stages[st], slot, res);
}

void
CbMaterialize(Device *dev, Resource *res)
{
   if (!res || !res->cb_buf)
      return;
   struct pipe_box box;
   u_box_1d(res->cb_offset, res->resource->width0, &box);
   dev->pipe->resource_copy_region(dev->pipe, res->resource, 0, 0, 0, 0, res->cb_buf, 0, &box);
   pipe_resource_reference(&res->cb_buf, NULL);
   res->cb_offset = 0;
   CbRebindAll(dev, res);
}

'''
patch(F + 'Resource.cpp', [
    ('#include "BcEtc.h"   /* exynos-woa G26 */\n',
     '#include "BcEtc.h"   /* exynos-woa G26 */\n#include "util/u_upload_mgr.h"   /* exynos-woa T6 */\n#include "util/box.h"\n'
     + HELPERS, 1),
    # eligibility
    ('   templat.usage      = translate_resource_usage(pCreateResource->Usage);\n',
     '   templat.usage      = translate_resource_usage(pCreateResource->Usage);\n'
     '   /* exynos-woa T6: renamed at every WRITE_DISCARD (only a constant buffer, nothing else bound) */\n'
     '   pResource->cb_rename = pCreateResource->Usage == D3D10_DDI_USAGE_DYNAMIC &&\n'
     '                          pCreateResource->BindFlags == D3D10_DDI_BIND_CONSTANT_BUFFER;\n', 1),
    # map: the renaming path, right after the per-kind timer
    ('   UmdSectionTimer map_kind_timer(map_kind);\n',
     '   UmdSectionTimer map_kind_timer(map_kind);\n\n'
     '   /* exynos-woa T6: DYNAMIC constant buffer + WRITE_DISCARD -> a fresh slice of the constant uploader */\n'
     '   if (pResource->cb_rename && DDIMap == D3D10_DDI_MAP_WRITE_DISCARD && SubResource == 0) {\n'
     '      struct pipe_resource *buf = NULL;\n'
     '      unsigned off = 0;\n'
     '      void *ptr = NULL;\n'
     '      u_upload_alloc_ref(pipe->const_uploader, 0, resource->width0, 256, &off, &buf, &ptr);\n'
     '      if (buf && ptr) {\n'
     '         pipe_resource_reference(&pResource->cb_buf, NULL);\n'
     '         pResource->cb_buf = buf;   /* the reference u_upload_alloc_ref took */\n'
     '         pResource->cb_offset = off;\n'
     '         pResource->cb_mapped = true;\n'
     '         CbRebindAll(CastDevice(hDevice), pResource);\n'
     '         pMappedSubResource->pData = ptr;\n'
     '         pMappedSubResource->RowPitch = resource->width0;\n'
     '         pMappedSubResource->DepthPitch = resource->width0;\n'
     '         return;\n'
     '      }\n'
     '      pipe_resource_reference(&buf, NULL);   /* allocation failed: the ordinary path below */\n'
     '   }\n'
     '   if (pResource->cb_buf)\n'
     '      CbMaterialize(CastDevice(hDevice), pResource);   /* any other map sees the current contents */\n', 1),
    # unmap
    ('   Resource *pResource = CastResource(hResource);\n\n   assert(SubResource < pResource->NumSubResources);\n\n'
     '   if (pResource->bc_map_usage && pResource->bc_map_usage[SubResource]) {\n',
     '   Resource *pResource = CastResource(hResource);\n\n   assert(SubResource < pResource->NumSubResources);\n\n'
     '   if (pResource->cb_mapped) {   /* exynos-woa T6: written into the uploader - flush it */\n'
     '      pResource->cb_mapped = false;\n'
     '      u_upload_unmap(pipe->const_uploader);\n'
     '      return;\n'
     '   }\n\n'
     '   if (pResource->bc_map_usage && pResource->bc_map_usage[SubResource]) {\n', 1),
    # destroy
    ('   if (pResource->so_target) {\n      pipe_so_target_reference(&pResource->so_target, NULL);\n   }\n\n'
     '   for (UINT SubResource = 0; SubResource < pResource->NumSubResources; ++SubResource) {\n',
     '   if (pResource->so_target) {\n      pipe_so_target_reference(&pResource->so_target, NULL);\n   }\n\n'
     '   {  /* exynos-woa T6 */\n'
     '      Device *dev = CastDevice(hDevice);\n'
     '      pipe_resource_reference(&pResource->cb_buf, NULL);\n'
     '      for (unsigned st = 0; st < 4; st++)\n'
     '         for (unsigned slot = 0; slot < PIPE_MAX_CONSTANT_BUFFERS; slot++)\n'
     '            if (dev->bound_cb[st][slot] == pResource)\n'
     '               dev->bound_cb[st][slot] = NULL;\n'
     '   }\n\n'
     '   for (UINT SubResource = 0; SubResource < pResource->NumSubResources; ++SubResource) {\n', 1),
    # copies / update: materialize first (ResourceCopy and ResourceCopyRegion share these lines)
    ('   Resource *pDstResource = CastResource(hDstResource);\n'
     '   Resource *pSrcResource = CastResource(hSrcResource);\n'
     '   struct pipe_resource *dst_resource = pDstResource->resource;\n'
     '   struct pipe_resource *src_resource = pSrcResource->resource;\n',
     '   Resource *pDstResource = CastResource(hDstResource);\n'
     '   Resource *pSrcResource = CastResource(hSrcResource);\n'
     '   CbMaterialize(pDevice, pDstResource);   /* exynos-woa T6 */\n'
     '   CbMaterialize(pDevice, pSrcResource);\n'
     '   struct pipe_resource *dst_resource = pDstResource->resource;\n'
     '   struct pipe_resource *src_resource = pSrcResource->resource;\n', 0),
    ('   struct pipe_context *pipe = pDevice->pipe;\n'
     '   Resource *pDstResource = CastResource(hDstResource);\n'
     '   struct pipe_resource *dst_resource = pDstResource->resource;\n\n'
     '   unsigned level;\n',
     '   struct pipe_context *pipe = pDevice->pipe;\n'
     '   Resource *pDstResource = CastResource(hDstResource);\n'
     '   CbMaterialize(pDevice, pDstResource);   /* exynos-woa T6 */\n'
     '   struct pipe_resource *dst_resource = pDstResource->resource;\n\n'
     '   unsigned level;\n', 1),
])
s = open(root + '/' + F + 'Resource.cpp', newline='').read()
print('materialize sites:', s.count('CbMaterialize(pDevice,'))
print('ok')
