import base64
import json
import struct
import zlib
import threading
import unittest
import urllib.request
import urllib.error
from http.server import ThreadingHTTPServer
from bridge import handler_type, title_packet

class TitlePacketTests(unittest.TestCase):
    def test_title_is_bounded_printable_and_crc_protected(self):
        message = title_packet('Caf\u00e9\n' + 'x'*100, 42)
        magic, sequence, size, flags, crc = struct.unpack('<4sIHHI', message[:16])
        body = message[16:]
        self.assertEqual((magic, sequence, size, flags), (b'MXA1', 42, 60, 2))
        self.assertTrue(body.startswith(b'Cafe'))
        self.assertTrue(all(32 <= c <= 126 for c in body))
        self.assertEqual(crc, zlib.crc32(body))

    def test_unrepresentable_title_has_visible_fallback(self):
        self.assertEqual(title_packet('\u266b', 1)[16:], b'NOW PLAYING')

class FakeDevice:
    status = {'connected': False}
    def __init__(self): self.calls=[]
    def send(self,*args):
        self.calls.append(args)
        return {'connected':True,'ack':'ACK 1 PAUSE'}

class BridgeBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.device=FakeDevice()
        self.server=ThreadingHTTPServer(('127.0.0.1',0),handler_type(self.device,'test-pairing-token'))
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True)
        self.thread.start()
        self.url=f'http://127.0.0.1:{self.server.server_port}/frame'

    def tearDown(self):
        self.server.shutdown(); self.server.server_close(); self.thread.join()

    def post(self,data,token='test-pairing-token'):
        request=urllib.request.Request(self.url,data=json.dumps(data).encode(),headers={
            'Content-Type':'application/json','X-Matrix-Token':token})
        try:
            with urllib.request.urlopen(request,timeout=3) as r:return r.status
        except urllib.error.HTTPError as e:
            code=e.code; e.close(); return code

    def test_unpaired_requests_cannot_write_to_device(self):
        self.assertEqual(self.post({'pixels':'','playing':True},'wrong-token'),403)
        self.assertEqual(self.device.calls,[])

    def test_malformed_frames_cannot_write_to_device(self):
        for data in [{'pixels':'!!!','playing':True},
                     {'pixels':base64.b64encode(bytes(8191)).decode(),'playing':True},
                     {'pixels':base64.b64encode(bytes(8192)).decode(),'playing':'false'}]:
            self.assertEqual(self.post(data),400)
        self.assertEqual(self.device.calls,[])

    def test_valid_paused_artwork_reaches_device_without_losing_pause_state(self):
        image=bytes(range(256))*32
        self.assertEqual(self.post({'pixels':base64.b64encode(image).decode(),
                                   'playing':False,'title':'Fiesta','artist':'Shoxrux'}),200)
        self.assertEqual(self.device.calls,[(image,False,'Fiesta','Shoxrux')])


class WirelessTests(unittest.TestCase):
    def device(self):
        import tempfile
        from pathlib import Path
        from bridge import Device
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        return Device(token='test-token',config_path=Path(self.temp.name)/'device.json')

    def test_wifi_frame_preserves_pixels_and_pause(self):
        from unittest.mock import Mock
        d=self.device();d._network=Mock(return_value={'accepted':True});d.connect=Mock(side_effect=AssertionError('USB should not open'))
        image=bytes(range(256))*32
        result=d.send(image,False,'test','artist')
        self.assertEqual(d._network.call_args.args[0],'/frame')
        payload=json.loads(d._network.call_args.args[1]);self.assertFalse(payload['playing'])
        self.assertEqual(base64.b64decode(payload['pixels']),image)
        self.assertEqual(result['transport'],'Wi-Fi');self.assertFalse(result['playing'])

    def test_wifi_failure_falls_back_to_usb_and_retains_phone_priority_ack(self):
        from unittest.mock import Mock
        d=self.device();d._network=Mock(side_effect=OSError('offline'));d.device_info={'ip':'192.168.1.42'}
        d._exchange=Mock(side_effect=['PHONE_ACTIVE','TITLE'])
        result=d.send(bytes(8192),False,'test')
        self.assertEqual(result['transport'],'USB');self.assertEqual(result['ack'],'PHONE_ACTIVE')
        self.assertEqual(d._exchange.call_args_list[0].args,(bytes(8192),0))

    def test_tap_command_is_passed_to_extension_once(self):
        from unittest.mock import Mock
        d=self.device();d._network=Mock(return_value={'accepted':True,'command':'next'})
        result=d.send(bytes(8192),True,'Song','Artist')
        self.assertEqual(result['command'],'next');self.assertNotIn('command',d.status)
        d._network=Mock(return_value={'accepted':True,'command':'rm -rf'})
        self.assertNotIn('command',d.send(bytes(8192),True,'Song','Artist'))

    def test_recent_wifi_success_never_opens_usb_on_a_blip(self):
        import time
        from unittest.mock import Mock
        d=self.device();d.last_network_ok=time.monotonic()
        d._network=Mock(side_effect=OSError('timeout'));d._exchange=Mock(side_effect=AssertionError('USB must not open'))
        with self.assertRaises(OSError): d.send(bytes(8192),True,'Song','Artist')
        d._exchange.assert_not_called()
        self.assertFalse(d.get_device_status()['device_online']);d._exchange.assert_not_called()

    def test_provisioning_is_usb_only_and_password_is_not_persisted(self):
        from unittest.mock import Mock
        d=self.device();d._network=Mock(side_effect=AssertionError('Must not send credentials over Wi-Fi'));d._exchange=Mock(return_value='WIFI_SAVED')
        self.assertTrue(d.configure_wifi('Home','secret-example')['saved'])
        d._exchange.assert_called_once_with(bytes([4,14])+b'Home'+b'secret-example',3,'WIFI_SAVED')
        self.assertFalse(d.config_path.exists())

    def test_invalid_wifi_credentials_do_not_reach_device(self):
        from unittest.mock import Mock
        d=self.device();d._exchange=Mock()
        for name,password in [('', 'x'),('x'*33,'x'),('Home','x'*65),('Ho\0me','pass'),('Home',None)]:
            with self.assertRaises(ValueError):d.configure_wifi(name,password)
        d._exchange.assert_not_called()

class SetupBoundaryTests(unittest.TestCase):
    setUp=BridgeBoundaryTests.setUp
    tearDown=BridgeBoundaryTests.tearDown
    post=BridgeBoundaryTests.post

    def test_setup_rejects_external_hostnames(self):
        request=urllib.request.Request(self.url.replace('/frame','/setup'),headers={'Host':'evil.example'})
        with self.assertRaises(urllib.error.HTTPError) as result:urllib.request.urlopen(request,timeout=2)
        self.assertEqual(result.exception.code,403);result.exception.close()

    def test_unpaired_provisioning_is_rejected(self):
        self.url=self.url.replace('/frame','/provision')
        self.assertEqual(self.post({'ssid':'Home','password':'example'},'wrong-token'),403)

    def test_status_cannot_be_read_without_pairing_token(self):
        url=self.url.replace('/frame','/device')
        with self.assertRaises(urllib.error.HTTPError) as result:urllib.request.urlopen(url,timeout=2)
        self.assertEqual(result.exception.code,403);result.exception.close()

if __name__=='__main__': unittest.main()
