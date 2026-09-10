import io
import unittest
import zlib
from capture import read_exact, receive_frame, rgb565_to_rgb


class FragmentedPort(io.BytesIO):
    def read(self, count=-1):
        return super().read(min(count, 7))


class CaptureTests(unittest.TestCase):
    def test_primary_colors(self):
        self.assertEqual(rgb565_to_rgb(bytes.fromhex('00f8e0071f00ffff0000')),
                         bytes.fromhex('ff000000ff000000ffffffff000000'))

    def test_fragmented_transfer(self):
        data = bytes(range(256)) * 600
        header = f'FRAME 320 240 RGB565 {len(data)} {zlib.crc32(data):08x}\r\n'.encode()
        self.assertEqual(receive_frame(FragmentedPort(header + data)), (320, 240, data))

    def test_corrupt_crc(self):
        port = io.BytesIO(b'FRAME 320 240 RGB565 153600 00000000\n' + b'\x00' * 153600)
        with self.assertRaisesRegex(ValueError, 'CRC'):
            receive_frame(port)

    def test_error_response(self):
        with self.assertRaisesRegex(RuntimeError, 'SCCB'):
            receive_frame(io.BytesIO(b'ERR SCCB\n'))

    def test_invalid_size(self):
        with self.assertRaises(ValueError):
            receive_frame(io.BytesIO(b'FRAME 640 480 RGB565 614400 0\n'))

    def test_truncated_timeout(self):
        with self.assertRaises(TimeoutError):
            read_exact(io.BytesIO(b'abc'), 4, timeout=0.01)


if __name__ == '__main__':
    unittest.main()
