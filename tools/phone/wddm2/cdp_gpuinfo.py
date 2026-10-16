"""cdp_gpuinfo.py [PORT] - read a Chromium browser's GPU state over the DevTools protocol (SystemInfo.getInfo):
feature status, driver bug workarounds, video decode/encode profiles, GPU device + aux attributes. Tracing only (AGENTS
rule 12): the browser is started with --remote-debugging-port for this read, never as a fix. Port reached through
`ssh -L PORT:127.0.0.1:PORT s10e`."""
import asyncio, json, sys, urllib.request
import websockets

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9222


async def main():
    ver = json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json/version', timeout=10))
    async with websockets.connect(ver['webSocketDebuggerUrl'], max_size=None) as ws:
        await ws.send(json.dumps({'id': 1, 'method': 'SystemInfo.getInfo'}))
        while True:
            m = json.loads(await ws.recv())
            if m.get('id') == 1:
                break
    g = m['result']['gpu']
    print('browser:', ver.get('Browser'))
    for d in g.get('devices', []):
        print('device:', d.get('vendorString'), d.get('deviceString'), 'driver', d.get('driverVersion'))
    print('--- featureStatus')
    for k, v in sorted(g.get('featureStatus', {}).items()):
        print(f'  {k}: {v}')
    print('--- driverBugWorkarounds')
    for w in g.get('driverBugWorkarounds', []):
        print('  ', w)
    print('--- auxAttributes')
    for k, v in sorted(g.get('auxAttributes', {}).items()):
        print(f'  {k}: {v}')
    print('--- videoDecoding', len(g.get('videoDecoding', [])), 'profiles;', 'videoEncoding', len(g.get('videoEncoding', [])))
    for p in g.get('videoDecoding', [])[:12]:
        print('  dec', p.get('profile'), p.get('maxResolution'))

asyncio.run(main())
