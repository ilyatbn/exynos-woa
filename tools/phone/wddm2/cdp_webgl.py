"""cdp_webgl.py [PORT] - G25/G26 browser check without the display (it may be asleep): over the DevTools protocol,
evaluate in the first page a WebGL clear + readPixels and a WebGL triangle draw + readPixels, print the pixels and the
renderer string. Tracing only (AGENTS rule 12): the browser runs with --remote-debugging-port for this read.
Port reached through `ssh -L PORT:127.0.0.1:PORT s10e`."""
import asyncio, json, sys, urllib.request
import websockets

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9222
JS = r"""
(() => {
  const c = document.createElement('canvas'); c.width = c.height = 16;
  const g = c.getContext('webgl'); if (!g) return 'no webgl context';
  g.clearColor(0.2, 0.4, 0.6, 1); g.clear(g.COLOR_BUFFER_BIT);
  const p = new Uint8Array(4); g.readPixels(8, 8, 1, 1, g.RGBA, g.UNSIGNED_BYTE, p);
  const vs = g.createShader(g.VERTEX_SHADER); g.shaderSource(vs, 'attribute vec2 a; void main(){gl_Position=vec4(a,0,1);}'); g.compileShader(vs);
  const fs = g.createShader(g.FRAGMENT_SHADER); g.shaderSource(fs, 'precision mediump float; void main(){gl_FragColor=vec4(1,0.5,0,1);}'); g.compileShader(fs);
  const pr = g.createProgram(); g.attachShader(pr, vs); g.attachShader(pr, fs); g.linkProgram(pr); g.useProgram(pr);
  const b = g.createBuffer(); g.bindBuffer(g.ARRAY_BUFFER, b);
  g.bufferData(g.ARRAY_BUFFER, new Float32Array([-1,-1, 3,-1, -1,3]), g.STATIC_DRAW);
  g.enableVertexAttribArray(0); g.vertexAttribPointer(0, 2, g.FLOAT, false, 0, 0); g.drawArrays(g.TRIANGLES, 0, 3);
  const q = new Uint8Array(4); g.readPixels(8, 8, 1, 1, g.RGBA, g.UNSIGNED_BYTE, q);
  const ext = g.getExtension('WEBGL_debug_renderer_info');
  return 'clear ' + Array.from(p).join(',') + ' draw ' + Array.from(q).join(',') + ' renderer ' +
         (ext ? g.getParameter(ext.UNMASKED_RENDERER_WEBGL) : g.getParameter(g.RENDERER));
})()
"""


async def main():
    pages = json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json', timeout=10))
    page = next(p for p in pages if p.get('type') == 'page')
    async with websockets.connect(page['webSocketDebuggerUrl'], max_size=None) as ws:
        await ws.send(json.dumps({'id': 1, 'method': 'Runtime.evaluate', 'params': {'expression': JS, 'returnByValue': True}}))
        while True:
            m = json.loads(await ws.recv())
            if m.get('id') == 1:
                break
    print(m.get('result', {}).get('result', {}).get('value', m))

asyncio.run(main())
