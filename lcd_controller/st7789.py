from ._base import LCDController


class ST7789(LCDController):

    _COLMOD_TABLE = {
        "RGB565": (0x55, 2),
        "RGB888": (0x77, 3),
    }

    def _build_init_sequence(self):
        w = self._width - 1
        h = self._height - 1
        caset = bytes([0, 0, w >> 8, w & 0xFF])
        raset = bytes([0, 0, h >> 8, h & 0xFF])

        return [
            (0x01, None, 120),
            (0x11, None, 120),
            (0x13, None, 0),
            (0x36, self._get_madctl_cmd(), 10),
            (0x3A, bytes([self._colmod_val]), 10),
            (0xB2, b'\x0C\x0C\x00\x33\x33', 10),
            (0xB7, b'\x35', 10),
            (0xBB, b'\x19', 10),
            (0xC0, b'\x2C', 10),
            (0xC2, b'\x01', 10),
            (0xC3, b'\x12', 10),
            (0xC4, b'\x20', 10),
            (0xC6, b'\x0F', 10),
            (self._get_inversion_cmd(), None, 10),
            (0xD0, b'\xA4\xA1', 10),
            (0xE0, b'\xD0\x00\x02\x07\x0A\x28\x32\x44\x42\x06\x0E\x12\x14\x17', 10),
            (0xE1, b'\xD0\x00\x02\x07\x0A\x28\x31\x54\x47\x0E\x1C\x17\x1B\x1E', 10),
            (0x21, None, 10),
            (0x2A, caset, 0),
            (0x2B, raset, 0),
            (0x29, None, 120),
            (0x11, None, 120),
        ]

    def _get_madctl_cmd(self):
        rotation_map = {
            0: 0x00,
            90: 0x60,
            180: 0xC0,
            270: 0xA0,
        }
        base = rotation_map.get(self._rotation, 0x00)
        if self._color_order == "BGR":
            base |= 0x08
        return bytes([base])

    def _get_inversion_cmd(self):
        return 0x21 if self._invert else 0x20

    def _update_madctl(self):
        self._bus.tx_param(0x36, self._get_madctl_cmd())

    def _update_inversion(self):
        self._bus.tx_param(self._get_inversion_cmd())
