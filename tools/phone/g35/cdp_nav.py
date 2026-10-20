"""cdp_nav.py URL [PORT] [WAIT_S] - G35 diagnosis: open a new tab of the CDP browser, enable the DevTools Media domain
(the page's media player messages/properties/errors = what edge://media-internals shows, incl. the chosen video
decoder and MEDIA_LOG lines), navigate to URL, collect for WAIT_S seconds, print the media events and the page's
document.title (browservideo's page puts its frame count there), close the tab. Tracing only (AGENTS rule 12).
Port reached through `ssh -L PORT:127.0.0.1:PORT s10e`."""
import asyncio, json, sys, time, urllib.request
import websockets

url = sys.argv[1]; port = int(sys.argv[2]) if len(sys.argv) > 2 else 9222; wait = float(sys.argv[3]) if len(sys.argv) > 3 else 20


async def main():
    ver = json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json/version', timeout=10))
    async with websockets.connect(ver['webSocketDebuggerUrl'], max_size=None) as ws:
        n = [0]
        events = []

        async def call(method, params=None, session=None):
            n[0] += 1; i = n[0]
            m = {'id': i, 'method': method, 'params': params or {}}
            if session:
                m['sessionId'] = session
            await ws.send(json.dumps(m))
            while True:
                r = json.loads(await ws.recv())
                if r.get('id') == i:
                    return r.get('result', r)
                if r.get('method', '').startswith('Media.'):
                    events.append(r)

        tid = (await call('Target.createTarget', {'url': 'about:blank'}))['targetId']
        sid = (await call('Target.attachToTarget', {'targetId': tid, 'flatten': True}))['sessionId']
        await call('Media.enable', session=sid)
        await call('Page.navigate', {'url': url}, sid)
        end = time.time() + wait
        while time.time() < end:
            try:
                r = json.loads(await asyncio.wait_for(ws.recv(), timeout=max(0.1, end - time.time())))
                if r.get('method', '').startswith('Media.'):
                    events.append(r)
            except asyncio.TimeoutError:
                break
        r = await call('Runtime.evaluate', {'expression': 'document.title + " | " + (document.getElementById("s") || {}).textContent'}, sid)
        for e in events:
            p = e['params']
            if e['method'] == 'Media.playerPropertiesChanged':
                for x in p.get('properties', []):
                    print('prop ', x.get('name'), '=', str(x.get('value'))[:200])
            elif e['method'] == 'Media.playerMessagesLogged':
                for x in p.get('messages', []):
                    print('msg  ', x.get('level'), x.get('message', '')[:300])
            elif e['method'] == 'Media.playerErrorsRaised':
                for x in p.get('errors', []):
                    print('error', json.dumps(x)[:400])
            elif e['method'] == 'Media.playerEventsAdded':
                for x in p.get('events', []):
                    print('event', x.get('value', '')[:200])
        print('page:', r.get('result', {}).get('value'))
        await call('Target.closeTarget', {'targetId': tid})

asyncio.run(main())
