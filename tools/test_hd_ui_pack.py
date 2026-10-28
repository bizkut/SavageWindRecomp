#!/usr/bin/env python3
"""Synthetic checks for the HD pack's channel-preserving decoding and mip layout."""
from pathlib import Path
import importlib.util
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('hd_ui_pack', Path(__file__).with_name('hd_ui_pack.py'))
hd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hd)

class PackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cache=tempfile.TemporaryDirectory()
        hd.setup_policy(Path(cls.cache.name))

    @classmethod
    def tearDownClass(cls):
        cls.cache.cleanup()

    def test_shared_sarc_enumerator(self):
        data=bytearray(96); data[:4]=b'SARC'; data[6:8]=b'\xff\xfe'
        struct.pack_into('<I',data,12,92); data[20:24]=b'SFAT'; struct.pack_into('<H',data,26,1)
        struct.pack_into('<IIII',data,32,0,0x01000000,0,4)
        data[56:72]=b'Layout/a.sblarc\x00'; data[92:96]=b'test'
        self.assertEqual(list(hd.sarc(bytes(data))), [('Layout/a.sblarc', b'test')])
        self.assertEqual(list(hd.sarc(bytes(data), layouts=True)), [('Layout/a.sblarc', b'test')])

    def test_shared_filter_and_scale(self):
        self.assertEqual(hd.texture_policy([hd.Image.new('L',(15,20),0)],1024),1)
        self.assertEqual(hd.texture_policy([hd.Image.new('L',(1025,20),0)],1024),2)
        self.assertEqual(hd.texture_policy([hd.Image.new('L',(32,32),42)],1024),3)
        img=hd.Image.new('L',(32,32)); img.putpixel((16,16),255)
        ramp=hd.Image.frombytes('L',(32,32),bytes(x*4 for y in range(32) for x in range(32)))
        self.assertEqual(hd.texture_policy([ramp],1024),3)
        self.assertEqual(hd.texture_policy([img],1024),0)
        self.assertEqual(hd.policy.hd_ui_texture_scale(256,256),4)
        self.assertEqual(hd.policy.hd_ui_texture_scale(257,256),2)
    def test_bntx_0901_rg8_channels_and_bc5(self):
        self.assertEqual(hd.source_format(0x0901), (1,2,2,0x0d))
        pixels=bytes([0x12,0x34,0x56,0x78]*8)
        channels=hd.unpack_raw(pixels,4,4,2)
        self.assertEqual(channels[0].getpixel((0,0)),0x12)
        self.assertEqual(channels[1].getpixel((0,0)),0x34)
        self.assertEqual(hd.content_hash(pixels),0xf0725719d3338025)
        blob=hd.dds([p.resize((16,16)) for p in channels])
        self.assertEqual(struct.unpack_from('<I',blob,128)[0],83)

    def test_bc5_preserves_channels(self):
        data=bytes([200, 100])+bytes(6)+bytes([30, 20])+bytes(6)
        channels=hd.unpack(data, 4, 4, 2)
        self.assertEqual(channels[0].getpixel((0,0)), 200)
        self.assertEqual(channels[1].getpixel((0,0)), 30)

    def test_bc4_mip_chain(self):
        image=hd.Image.new('L', (16,16), 200)
        blob=hd.dds([image])
        self.assertEqual(len(blob), 148+184)
        self.assertEqual(struct.unpack_from('<I', blob, 28)[0], 5)
        self.assertEqual(struct.unpack_from('<I', blob, 128)[0], 80)

if __name__ == '__main__': unittest.main()
