"""Independent captured-frame fixtures for the transmit tool's bit codec."""
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from frame import check_crc, make_frame, pack, unpack


class CodecTest(unittest.TestCase):
    def test_captured_request(self):
        raw = bytes.fromhex("290218850546050546008142C100")
        self.assertEqual(make_frame(0xCE8C234F, bytes.fromhex("04310A0A8C0A0A8C0102"), 1), raw)
        self.assertEqual(check_crc(0xCE8C234F, raw, 105),
                         {"calculated": "8582", "received": "8582", "valid": True})

    def test_captured_corrupt_candidate(self):
        raw = bytes.fromhex("2A82188505460505460083161F00")
        self.assertEqual(check_crc(0xCE8C234F, raw, 105),
                         {"calculated": "ECBB", "received": "2C3E", "valid": False})

    def test_address_is_covered(self):
        raw = make_frame(0xD3916AC5, bytes.fromhex("03112110682C10680202"), 2)
        self.assertTrue(check_crc(0xD3916AC5, raw, 105)["valid"])
        self.assertFalse(check_crc(0xCE8C234F, raw, 105)["valid"])
        self.assertEqual(raw[-1] & 127, 0)
        self.assertEqual(pack(unpack(raw, 105)), raw)

    def test_invalid_payload(self):
        for sync, payload, pid in [(0, bytes(10), 0), (0xFFFFFFFF, bytes(10), 0),
                                    (0xCE8C234F, bytes(9), 0), (0xCE8C234F, bytes(10), 4)]:
            with self.assertRaises(ValueError):
                make_frame(sync, payload, pid)


if __name__ == "__main__":
    unittest.main()
