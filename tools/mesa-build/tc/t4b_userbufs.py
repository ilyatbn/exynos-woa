# T4b (notes/perf-threaded-context.md): what the first threaded-context run (x86 tc1) showed - threaded_context_create
# asserts caps.prefer_real_buffer_in_constbuf0, and the threaded context does not take user buffers at all
# (tc_set_constant_buffer: "frontend must handle this" -> UNREACHABLE; tc_set_vertex_buffers asserts
# !is_user_buffer). d3d10umd used user memory for unbound vertex-buffer slots (a static float[4]) and for two
# compute constant buffers (GsEmu parameters, VideoProc CSC constants).
# - panfrost: caps->prefer_real_buffer_in_constbuf0 = true (applied in the tree before this script; re-checked here)
# - d3d10umd: a real 16-byte zero buffer (Device::dummy_vb) in every unbound slot (stride 0 -> element 0 = zeros,
#   the same "unbound reads 0" result as before); the two constant buffers go through pipe->const_uploader.
# usage: python t4b_userbufs.py <mesa root>
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


assert 'prefer_real_buffer_in_constbuf0 = true' in open(root + '/src/gallium/drivers/panfrost/pan_screen.c').read()

F = 'src/gallium/frontends/d3d10umd/'
patch(F + 'State.h', [
    ('   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];\n',
     '   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];\n'
     '   /* exynos-woa TC T4b: 16 zero bytes bound to every unbound vertex-buffer slot (stride 0) - the threaded\n'
     '    * context takes no user vertex buffers */\n'
     '   struct pipe_resource *dummy_vb;\n', 1),
])
patch(F + 'Device.cpp', [
    ('   pDevice->empty_vs = CreateEmptyShader(pDevice, MESA_SHADER_VERTEX);\n',
     '   /* exynos-woa TC T4b: unbound vertex-buffer slots read this (was a static user array) */\n'
     '   {\n'
     '      static const float zeros[4] = {0.0f, 0.0f, 0.0f, 0.0f};\n'
     '      pDevice->dummy_vb = pipe_buffer_create(screen, PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_IMMUTABLE, sizeof zeros);\n'
     '      if (pDevice->dummy_vb)\n'
     '         pipe_buffer_write(pipe, pDevice->dummy_vb, 0, sizeof zeros, zeros);\n'
     '      for (unsigned v = 0; v < PIPE_MAX_ATTRIBS; ++v) {\n'
     '         pDevice->vertex_buffers[v].is_user_buffer = false;\n'
     '         pDevice->vertex_buffers[v].buffer_offset = 0;\n'
     '         pipe_resource_reference(&pDevice->vertex_buffers[v].buffer.resource, pDevice->dummy_vb);\n'
     '      }\n'
     '   }\n'
     '   pDevice->empty_vs = CreateEmptyShader(pDevice, MESA_SHADER_VERTEX);\n', 1),
    ('   for (i = 0; i < PIPE_MAX_ATTRIBS; ++i) {\n'
     '      if (!pDevice->vertex_buffers[i].is_user_buffer) {\n'
     '         pipe_resource_reference(&pDevice->vertex_buffers[i].buffer.resource, NULL);\n'
     '      }\n'
     '   }\n',
     '   for (i = 0; i < PIPE_MAX_ATTRIBS; ++i) {\n'
     '      if (!pDevice->vertex_buffers[i].is_user_buffer) {\n'
     '         pipe_resource_reference(&pDevice->vertex_buffers[i].buffer.resource, NULL);\n'
     '      }\n'
     '   }\n'
     '   pipe_resource_reference(&pDevice->dummy_vb, NULL);   /* exynos-woa TC T4b */\n', 1),
])
patch(F + 'InputAssembly.cpp', [
    ('   static const float dummy[4] = {0.0f, 0.0f, 0.0f, 0.0f};\n\n   LOG_ENTRYPOINT();\n',
     '   LOG_ENTRYPOINT();\n', 1),
    ('      else {\n'
     '         pDevice->vertex_strides[StartBuffer + i] = 0;\n'
     '         vb->buffer_offset = 0;\n'
     '         if (!vb->is_user_buffer) {\n'
     '            pipe_resource_reference(&vb->buffer.resource, NULL);\n'
     '            vb->is_user_buffer = true;\n'
     '         }\n'
     '         vb->buffer.user = dummy;\n'
     '      }\n',
     '      else {\n'
     '         /* exynos-woa TC T4b: unbound -> the device\'s zero buffer (no user buffers under the threaded context) */\n'
     '         pDevice->vertex_strides[StartBuffer + i] = 0;\n'
     '         vb->buffer_offset = 0;\n'
     '         if (vb->is_user_buffer) {\n'
     '            vb->buffer.resource = NULL;\n'
     '            vb->is_user_buffer = false;\n'
     '         }\n'
     '         pipe_resource_reference(&vb->buffer.resource, pDevice->dummy_vb);\n'
     '      }\n', 1),
    ('      /* XXX this is odd... */\n'
     '      if (!vb->is_user_buffer && !vb->buffer.resource) {\n'
     '         pDevice->vertex_strides[i] = 0;\n'
     '         vb->buffer_offset = 0;\n'
     '         vb->is_user_buffer = true;\n'
     '         vb->buffer.user = dummy;\n'
     '      }\n',
     '      /* XXX this is odd... (exynos-woa TC T4b: the zero buffer instead of a user array) */\n'
     '      if (!vb->is_user_buffer && !vb->buffer.resource) {\n'
     '         pDevice->vertex_strides[i] = 0;\n'
     '         vb->buffer_offset = 0;\n'
     '         pipe_resource_reference(&vb->buffer.resource, pDevice->dummy_vb);\n'
     '      }\n', 1),
])
patch(F + 'GsEmu.cpp', [
    ('#include "util/u_inlines.h"\n', '#include "util/u_inlines.h"\n#include "util/u_upload_mgr.h"\n', 1),
    ('   struct pipe_constant_buffer pcb;\n'
     '   memset(&pcb, 0, sizeof pcb);\n'
     '   pcb.user_buffer = param;\n'
     '   pcb.buffer_size = sizeof param;\n'
     '   pipe->set_constant_buffer(pipe, MESA_SHADER_COMPUTE, GS_EMU_TRACKED_CBS - 1, &pcb);\n',
     '   struct pipe_constant_buffer pcb;\n'
     '   memset(&pcb, 0, sizeof pcb);\n'
     '   /* exynos-woa TC T4b: uploaded - the threaded context takes no user constant buffers */\n'
     '   u_upload_data_ref(pipe->const_uploader, 0, sizeof param, 256, param, &pcb.buffer_offset, &pcb.buffer);\n'
     '   u_upload_unmap(pipe->const_uploader);\n'
     '   pcb.buffer_size = sizeof param;\n'
     '   pipe->set_constant_buffer(pipe, MESA_SHADER_COMPUTE, GS_EMU_TRACKED_CBS - 1, &pcb);\n'
     '   pipe_resource_reference(&pcb.buffer, NULL);\n', 1),
])
patch(F + 'VideoProc.cpp', [
    ('#include "util/u_inlines.h"\n', '#include "util/u_inlines.h"\n#include "util/u_upload_mgr.h"\n', 1),
    ('   struct pipe_constant_buffer cb = {};\n'
     '   cb.user_buffer = &k;\n'
     '   cb.buffer_size = sizeof(k);\n',
     '   struct pipe_constant_buffer cb = {};\n'
     '   /* exynos-woa TC T4b: uploaded - the threaded context takes no user constant buffers (cb.buffer is dropped\n'
     '    * after set_constant_buffer below) */\n'
     '   u_upload_data_ref(pipe->const_uploader, 0, sizeof(k), 256, &k, &cb.buffer_offset, &cb.buffer);\n'
     '   u_upload_unmap(pipe->const_uploader);\n'
     '   cb.buffer_size = sizeof(k);\n', 1),
    ('   pipe->set_constant_buffer(pipe, MESA_SHADER_COMPUTE, 0, &cb);\n',
     '   pipe->set_constant_buffer(pipe, MESA_SHADER_COMPUTE, 0, &cb);\n'
     '   pipe_resource_reference(&cb.buffer, NULL);   /* exynos-woa TC T4b */\n', 1),
])
print('ok')
