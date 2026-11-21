#!/usr/bin/env python3
# A fake XiaoZhi service for the host test (tools/xiaozhi-host/run.mjs): the OTA endpoint (activation code first,
# 202 once, then 200), and the WebSocket endpoint (hello, listen start/stop, Opus frames up; stt, llm, tts and the
# same Opus frames echoed down as the "spoken" answer). Standard library only. Checks what the client sends and
# writes a report (JSON) for the test to assert on.
import base64, hashlib, json, socket, struct, sys

port, report_path = int(sys.argv[1]), sys.argv[2]
TOKEN = 'test-token-123'
report = {'ota': 0, 'activate': 0, 'ws': 0, 'frames_up': 0, 'errors': [], 'headers': {}}
state = {'bound': False, 'activate_calls': 0}


def save():
    with open(report_path, 'w') as f:
        json.dump(report, f)


def read_head(c):
    data = b''
    while b'\r\n\r\n' not in data:
        b = c.recv(1)
        if not b:
            raise EOFError
        data += b
    head, _, _ = data.partition(b'\r\n\r\n')
    lines = head.decode().split('\r\n')
    hdr = {}
    for l in lines[1:]:
        k, _, v = l.partition(':')
        hdr[k.strip().lower()] = v.strip()
    return lines[0], hdr


def recv_exact(c, n):
    out = b''
    while len(out) < n:
        b = c.recv(n - len(out))
        if not b:
            raise EOFError
        out += b
    return out


def http_reply(c, status, obj):
    body = json.dumps(obj).encode()
    reason = {200: 'OK', 202: 'Accepted', 404: 'Not Found'}[status]
    # chunked on purpose: the client must de-chunk
    chunks = b''.join(b'%x\r\n%s\r\n' % (len(body[i:i + 40]), body[i:i + 40]) for i in range(0, len(body), 40)) + b'0\r\n\r\n'
    c.sendall(b'HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n'
              % (status, reason.encode()) + chunks)


def ws_recv(c):
    h = recv_exact(c, 2)
    op, n = h[0] & 15, h[1] & 127
    if not h[1] & 0x80:
        report['errors'].append('client frame not masked')
    if n == 126:
        n = struct.unpack('>H', recv_exact(c, 2))[0]
    elif n == 127:
        n = struct.unpack('>Q', recv_exact(c, 8))[0]
    mask = recv_exact(c, 4)
    data = bytearray(recv_exact(c, n))
    for i in range(n):
        data[i] ^= mask[i & 3]
    return op, bytes(data)


def ws_send(c, op, data):
    n = len(data)
    h = bytes([0x80 | op]) + (bytes([n]) if n < 126 else bytes([126]) + struct.pack('>H', n))
    c.sendall(h + data)


def ws_session(c, hdr):
    report['ws'] += 1
    report['headers'] = {k: hdr.get(k, '') for k in ('authorization', 'device-id', 'client-id', 'protocol-version')}
    if hdr.get('authorization') != 'Bearer ' + TOKEN:
        report['errors'].append('bad authorization: %r' % hdr.get('authorization'))
    key = hdr.get('sec-websocket-key', '')
    acc = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
    c.sendall(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
               'Sec-WebSocket-Accept: %s\r\n\r\n' % acc).encode())
    frames, listening = [], False
    while True:
        op, data = ws_recv(c)
        if op == 8:
            return
        if op == 1:
            m = json.loads(data)
            if m.get('type') == 'hello':
                ap = m.get('audio_params', {})
                if ap.get('format') != 'opus' or ap.get('sample_rate') != 16000:
                    report['errors'].append('hello audio_params %r' % ap)
                ws_send(c, 1, json.dumps({'type': 'hello', 'transport': 'websocket', 'session_id': 's-42',
                                          'audio_params': {'format': 'opus', 'sample_rate': 24000, 'channels': 1,
                                                           'frame_duration': 60}}).encode())
            elif m.get('type') == 'listen':
                if m.get('session_id') != 's-42':
                    report['errors'].append('listen without session_id')
                if m.get('state') == 'start':
                    listening = True
                elif m.get('state') == 'stop':
                    report['frames_up'] = len(frames)
                    ws_send(c, 1, json.dumps({'session_id': 's-42', 'type': 'stt', 'text': 'hello nucleo'}).encode())
                    ws_send(c, 1, json.dumps({'session_id': 's-42', 'type': 'llm', 'emotion': 'happy', 'text': ':)'}).encode())
                    ws_send(c, 1, json.dumps({'session_id': 's-42', 'type': 'tts', 'state': 'start'}).encode())
                    ws_send(c, 1, json.dumps({'session_id': 's-42', 'type': 'tts', 'state': 'sentence_start',
                                              'text': 'Hi! I am a fake XiaoZhi.'}).encode())
                    ws_send(c, 9, b'p')                                   # a ping in the middle: must be answered
                    for f in frames:
                        ws_send(c, 2, f)
                    ws_send(c, 1, json.dumps({'session_id': 's-42', 'type': 'tts', 'state': 'stop'}).encode())
            save()
        elif op == 2:
            if not listening:
                report['errors'].append('audio before listen start')
            frames.append(data)
        elif op == 10:
            report['pong'] = True


srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', port))
srv.listen(4)
srv.settimeout(120)
print('ready', flush=True)
while True:
    try:
        c, _ = srv.accept()
    except socket.timeout:
        break
    c.settimeout(30)
    try:
        line, hdr = read_head(c)
        method, path = line.split()[:2]
        if method == 'POST':
            body = recv_exact(c, int(hdr.get('content-length', '0')))
            if not hdr.get('client-id') or not hdr.get('device-id'):
                report['errors'].append('OTA without identity headers')
            if path == '/xiaozhi/ota/':
                report['ota'] += 1
                json.loads(body)
                r = {'websocket': {'url': 'ws://127.0.0.1:%d/xiaozhi/v1/' % port, 'token': TOKEN},
                     'firmware': {'version': '2.0.0', 'url': ''}}
                if not state['bound']:
                    r['activation'] = {'code': '424242', 'message': 'xiaozhi.me\n424242', 'challenge': 'c-1'}
                http_reply(c, 200, r)
            elif path == '/xiaozhi/ota/activate':
                report['activate'] += 1
                state['activate_calls'] += 1
                if state['activate_calls'] >= 2:
                    state['bound'] = True
                    http_reply(c, 200, {'message': 'Device activated', 'device_id': 1})
                else:
                    http_reply(c, 202, {'message': 'Device activation timeout'})
            else:
                http_reply(c, 404, {})
        elif method == 'GET' and hdr.get('upgrade', '').lower() == 'websocket':
            ws_session(c, hdr)
    except Exception as e:  # report, keep serving
        report['errors'].append('%s: %s' % (type(e).__name__, e))
    finally:
        c.close()
        save()
