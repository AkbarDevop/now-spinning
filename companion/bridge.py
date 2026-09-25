#!/usr/bin/env python3
"""Local YouTube Music helper with Wi-Fi transport and USB setup/fallback."""
import argparse
import base64
import binascii
import hmac
import json
import struct
import threading
import time
import unicodedata
import zlib
import urllib.request
import urllib.error
import ipaddress
import os
import termios
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import serial
from serial.tools import list_ports

ROOT = Path(__file__).resolve().parents[1]

def packet(image, sequence, playing):
    if len(image) != 8192:
        raise ValueError('Artwork must contain 8192 RGB565 bytes')
    return struct.pack('<4sIHHI', b'MXA1', sequence, len(image), int(playing), zlib.crc32(image)) + image

def title_packet(title, sequence):
    # The tiny built-in panel font is ASCII; preserve common accented letters.
    text = unicodedata.normalize('NFKD', title).encode('ascii', 'ignore')
    text = bytes(c for c in text if 32 <= c <= 126).strip()[:60] or b'NOW PLAYING'
    return struct.pack('<4sIHHI', b'MXA1', sequence, len(text), 2, zlib.crc32(text)) + text

def control_packet(data, sequence, flags):
    return struct.pack('<4sIHHI', b'MXA1', sequence, len(data), flags, zlib.crc32(data)) + data

class Device:
    def __init__(self, port=None, token=None, config_path=None):
        self.port = port
        self.token = token or (ROOT/'companion/pairing-token.txt').read_text().strip()
        self.config_path = config_path or ROOT/'companion/device.json'
        self.connection = None
        self.lock = threading.Lock()
        self.sequence = 0
        self.last_title = None
        self.next_network_attempt = 0
        self.last_network_ok = None
        self.network_host = None
        self.device_info = {}
        self.status = {'connected': False, 'title': '', 'artist': '', 'playing': False}
        self.http = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        try:
            host = json.loads(self.config_path.read_text()).get('ip')
            if host and ipaddress.ip_address(host).is_private: self.network_host = host
        except (OSError, ValueError): pass

    def _remember(self, info):
        self.device_info = info
        address = info.get('ip', '')
        if address:
            try: valid = ipaddress.ip_address(address).is_private
            except ValueError: valid = False
            if valid and address != self.network_host:
                self.network_host = address
                self.config_path.write_text(json.dumps({'ip': address})+'\n')
                os.chmod(self.config_path, 0o600)

    def _wifi_recent(self, window=600):
        # Opening the USB port toggles DTR/RTS, which resets the ESP32-S3
        # (reset reason: USB). Once Wi-Fi works, a short blip (e.g. during a
        # Wi-Fi firmware update) must not trigger the USB fallback.
        return self.last_network_ok is not None and time.monotonic()-self.last_network_ok < window

    def _network(self, path, data=None):
        hosts = list(dict.fromkeys([self.network_host, 'akbar-matrix.local']))
        last_error = None
        for host in filter(None, hosts):
            request = urllib.request.Request('http://'+host+':18766'+path, data=data,
                headers={'X-Matrix-Token': self.token, 'Content-Type': 'application/json'})
            try:
                with self.http.open(request, timeout=2) as r:
                    result = json.loads(r.read(4096))
                if path == '/status': self._remember(result)
                self.last_network_ok = time.monotonic()
                return result
            except (OSError, ValueError) as e: last_error=e
        raise OSError('Display not reachable on Wi-Fi') from last_error

    def connect(self):
        if self.connection and self.connection.is_open: return
        ports = [p.device for p in list_ports.comports() if p.vid == 0x303a and p.pid == 0x1001]
        port = self.port or (ports[0] if len(ports) == 1 else None)
        if not port: raise OSError('Display is offline. For first-time Wi-Fi setup, connect its USB data cable.')
        s = serial.Serial()
        s.port = port; s.baudrate = 115200; s.timeout = .1; s.write_timeout = 3
        s.dtr = False; s.rts = False; s.open()
        # macOS otherwise drops modem-control lines on close (HUPCL), which
        # can reset the native USB controller while switching to Wi-Fi.
        attrs=termios.tcgetattr(s.fileno());attrs[2]&=~termios.HUPCL
        termios.tcsetattr(s.fileno(),termios.TCSANOW,attrs)
        self.connection = s; self.last_title = None
        time.sleep(1.5); s.reset_input_buffer()

    def _exchange(self, payload, flags, expected=None):
        self.connect(); self.sequence = (self.sequence + 1) & 0xffffffff
        self.connection.write(control_packet(payload, self.sequence, flags)); self.connection.flush()
        deadline=time.monotonic()+4
        while time.monotonic()<deadline:
            line=self.connection.readline().decode('utf-8','replace').strip()
            prefix=f'ACK {self.sequence} '
            if line.startswith(prefix):
                reply=line[len(prefix):]
                if expected and not reply.startswith(expected): raise OSError('Unexpected display acknowledgement')
                return reply
            if line.startswith('ERR '): raise OSError(line)
        raise OSError('Display did not acknowledge the USB message')

    def _close_usb(self):
        if self.connection:
            self.connection.close(); self.connection=None

    def send(self, image, playing, title='', artist=''):
        if len(image)!=8192: raise ValueError('Invalid artwork length')
        with self.lock:
            try:
                transport='USB'; ack=None; command=None
                if time.monotonic()>=self.next_network_attempt:
                    try:
                        reply=self._network('/frame', json.dumps({'playing':playing,'pixels':base64.b64encode(image).decode()}).encode())
                        if isinstance(reply,dict) and reply.get('command') in ('next','toggle','previous'): command=reply['command']
                        transport='Wi-Fi'; ack='Wi-Fi accepted'; self._close_usb()
                    except OSError:
                        if not self._wifi_recent(): self.next_network_attempt=time.monotonic()+15
                if ack is None:
                    if self._wifi_recent():
                        raise OSError('Display briefly unreachable on Wi-Fi; not opening USB because that resets the board')
                    ack=self._exchange(image, int(playing))
                    if title!=self.last_title:
                        text=title_packet(title,0)[16:];self._exchange(text,2,'TITLE');self.last_title=title
                    # Learn DHCP address over USB so subsequent Wi-Fi requests are fast.
                    if not self.device_info.get('ip'):
                        try: self._remember(json.loads(self._exchange(b'?',4,'STATUS ')[7:]))
                        except (OSError,ValueError): pass
                self.status={'connected':True,'title':title,'artist':artist,'playing':playing,
                             'updated':time.time(),'ack':ack,'transport':transport,**self.device_info}
                result=self.status.copy()
                if command: result['command']=command  # one-shot tap gesture for the extension
                return result
            except (OSError,serial.SerialException) as e:
                self._close_usb();self.status={'connected':False,'error':str(e)}
                raise OSError(str(e)) from e

    def get_device_status(self):
        with self.lock:
            try:
                info=self._network('/status'); transport='Wi-Fi'
            except OSError:
                if self._wifi_recent():
                    return {**self.status,'device_online':False,'error':'Display briefly unreachable on Wi-Fi'}
                try:
                    info=json.loads(self._exchange(b'?',4,'STATUS ')[7:]);transport='USB'
                    self._remember(info)
                except (OSError,ValueError,serial.SerialException) as e:
                    self._close_usb();return {**self.status,'device_online':False,'error':str(e)}
            return {**self.status,**info,'device_online':True,'transport':transport}

    def configure_wifi(self, ssid, password):
        if not isinstance(ssid,str) or not isinstance(password,str): raise ValueError('Network name and password must be text')
        name=ssid.encode('utf-8');secret=password.encode('utf-8')
        if not 1<=len(name)<=32 or len(secret)>64 or b'\0' in name+secret: raise ValueError('Invalid network name or password length')
        with self.lock:
            # Provisioning is deliberately USB-only; the password is not saved on the Mac.
            try: self._exchange(bytes([len(name),len(secret)])+name+secret,3,'WIFI_SAVED')
            except (OSError,serial.SerialException): self._close_usb();raise
            self.next_network_attempt=0
        return {'saved':True,'message':'Saved on the display. Allow up to 30 seconds to connect.'}

def handler_type(device, token):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def local_host(self):
            # Prevent a rebound external hostname from reading the setup token.
            return self.headers.get('Host','') in (f'127.0.0.1:{self.server.server_port}',f'localhost:{self.server.server_port}')

        def authorized(self):
            return hmac.compare_digest(self.headers.get('X-Matrix-Token', ''), token)

        def reply(self, code, data):
            body = json.dumps(data).encode()
            self.send_response(code)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers(); self.wfile.write(body)

        def do_GET(self):
            if not self.local_host(): return self.reply(403, {'error':'Local host required'})
            if self.path in ('/', '/setup'):
                page=(ROOT/'companion/setup.html').read_text().replace('__PAIRING_TOKEN__',json.dumps(token))
                body=page.encode();self.send_response(200)
                self.send_header('Content-Type','text/html; charset=utf-8')
                self.send_header('Cache-Control','no-store');self.send_header('X-Frame-Options','DENY')
                self.send_header('Content-Security-Policy',"default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; form-action 'none'")
                self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body);return
            if self.path not in ('/status','/device'): return self.reply(404, {'error':'Not found'})
            if not self.authorized(): return self.reply(403, {'error':'Not paired'})
            self.reply(200, device.get_device_status() if self.path=='/device' else device.status)

        def do_POST(self):
            if not self.local_host(): return self.reply(403, {'error':'Local host required'})
            if self.path not in ('/frame','/provision'): return self.reply(404, {'error':'Not found'})
            if not self.authorized(): return self.reply(403, {'error':'Not paired'})
            try:
                size = int(self.headers.get('Content-Length', '0'))
                if not 0 < size <= 16000: raise ValueError('Invalid message size')
                if self.headers.get('Content-Type','').split(';')[0] != 'application/json':
                    raise ValueError('JSON required')
                data = json.loads(self.rfile.read(size))
                if self.path=='/provision':
                    if not isinstance(data,dict): raise ValueError('Invalid settings')
                    result=device.configure_wifi(data.get('ssid'),data.get('password'))
                    return self.reply(200,result)
                if not isinstance(data, dict) or type(data.get('playing')) is not bool:
                    raise ValueError('Invalid playback state')
                image = base64.b64decode(data['pixels'], validate=True)
                if len(image) != 8192: raise ValueError('Invalid artwork length')
                title, artist = data.get('title',''), data.get('artist','')
                if not isinstance(title,str) or not isinstance(artist,str): raise ValueError('Invalid metadata')
                result = device.send(image, data['playing'], title[:240], artist[:240])
                self.reply(200, result)
            except (ValueError, KeyError, TypeError, binascii.Error) as e:
                self.reply(400, {'error':str(e)})
            except OSError as e:
                self.reply(503, {'error':str(e)})
    return Handler

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port')
    parser.add_argument('--listen-port', type=int, default=18765)
    args = parser.parse_args()
    token = (ROOT/'companion/pairing-token.txt').read_text().strip()
    server = ThreadingHTTPServer(('127.0.0.1', args.listen_port), handler_type(Device(args.port, token), token))
    print('Music Matrix helper ready. Setup: http://127.0.0.1:18765/setup', flush=True)
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally: server.server_close()

if __name__ == '__main__': main()
