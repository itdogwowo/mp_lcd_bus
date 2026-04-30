import lcd_bus
import machine
import gc

try:
    import heap_caps
    _HAS_HEAP_CAPS = True
except ImportError:
    _HAS_HEAP_CAPS = False


class LCDController:

    _COLMOD_TABLE = {}

    def __init__(
        self,
        bus,
        rst,
        width,
        height,
        frame_buffer=None,
        rotation=0,
        color_order="RGB",
        invert=False,
        color_space="RGB565",
        rgb565_byte_swap=False,
    ):
        colmod_table = type(self)._COLMOD_TABLE
        if color_space not in colmod_table:
            raise ValueError(
                f"color_space must be one of {tuple(colmod_table.keys())}"
            )

        self._bus = bus
        self._width = width
        self._height = height
        self._rotation = rotation
        self._color_order = color_order.upper()
        self._invert = bool(invert)
        self._color_space = color_space
        self._colmod_val, self._bytes_per_pixel = colmod_table[color_space]
        self._rgb565_byte_swap = rgb565_byte_swap
        self._initialized = False

        self._param_buf = bytearray(4)
        self._param_mv = memoryview(self._param_buf)

        if isinstance(rst, int):
            self._rst = machine.Pin(rst, machine.Pin.OUT)
        else:
            self._rst = rst
        self._rst.value(1)

        if frame_buffer is None:
            frame_buffer = [None]

        if isinstance(frame_buffer, list):
            for i, fb in enumerate(frame_buffer):
                if fb is None:
                    frame_buffer[i] = self._alloc_one_fb()
            self._fb1 = frame_buffer[0]
            self._fb2 = frame_buffer[1] if len(frame_buffer) > 1 else None
        else:
            self._fb1 = frame_buffer
            self._fb2 = None

        bpp = self._bytes_per_pixel * 8
        self._bus.init(
            width=width,
            height=height,
            bpp=bpp,
            buffer_size=len(self._fb1),
            rgb565_byte_swap=rgb565_byte_swap,
        )

    def _alloc_one_fb(self):
        size = self._width * self._height * self._bytes_per_pixel

        if _HAS_HEAP_CAPS:
            gc.collect()
            for caps in (
                heap_caps.CAP_INTERNAL | heap_caps.CAP_DMA,
                heap_caps.CAP_SPIRAM | heap_caps.CAP_DMA,
            ):
                try:
                    return heap_caps.malloc(size, caps)
                except MemoryError:
                    pass
            raise MemoryError(
                f"unable to allocate frame buffer ({size} bytes)"
            )
        else:
            return bytearray(size)

    def write_cmd(self, cmd):
        self._bus.tx_param(cmd)

    def write_cmd_data(self, cmd, data):
        self._bus.tx_param(cmd, data)

    def write_color(self, data, x1, y1, x2, y2):
        self._bus.tx_color(0x2C, data, x1, y1, x2, y2)

    def set_window(self, x1, y1, x2, y2):
        self._param_mv[0] = x1 >> 8
        self._param_mv[1] = x1 & 0xFF
        self._param_mv[2] = x2 >> 8
        self._param_mv[3] = x2 & 0xFF
        self._bus.tx_param(0x2A, self._param_mv)

        self._param_mv[0] = y1 >> 8
        self._param_mv[1] = y1 & 0xFF
        self._param_mv[2] = y2 >> 8
        self._param_mv[3] = y2 & 0xFF
        self._bus.tx_param(0x2B, self._param_mv)

        self._bus.tx_param(0x2C)

    def fill(self, color):
        if isinstance(color, tuple) and len(color) == 3:
            r, g, b = color
            color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

        size = self._width * self._height * self._bytes_per_pixel
        buf = bytearray(size)
        mv = memoryview(buf)

        if self._bytes_per_pixel == 2:
            for i in range(0, size, 2):
                mv[i] = color >> 8
                mv[i + 1] = color & 0xFF

        self.set_window(0, 0, self._width - 1, self._height - 1)
        self.write_color(mv, 0, 0, self._width - 1, self._height - 1)

    def init(self):
        self._hardware_reset()
        self._run_init_sequence()
        self._initialized = True

    def _hardware_reset(self):
        self._rst.value(0)
        machine.time.sleep_ms(50)
        self._rst.value(1)
        machine.time.sleep_ms(50)

    def _run_init_sequence(self):
        for cmd, data, delay in self._build_init_sequence():
            self.write_cmd_data(cmd, data)
            if delay:
                machine.time.sleep_ms(delay)

    def _build_init_sequence(self):
        return []

    def set_rotation(self, rotation):
        if rotation not in (0, 90, 180, 270):
            raise ValueError("rotation must be 0, 90, 180, or 270")
        self._rotation = rotation
        self._update_madctl()

    def get_rotation(self):
        return self._rotation

    def set_color_order(self, order):
        if order.upper() not in ("RGB", "BGR"):
            raise ValueError("color_order must be 'RGB' or 'BGR'")
        self._color_order = order.upper()
        self._update_madctl()

    def get_color_order(self):
        return self._color_order

    def invert_display(self, invert=True):
        self._invert = bool(invert)
        self._update_inversion()

    def get_inversion_state(self):
        return self._invert

    def _update_madctl(self):
        pass

    def _update_inversion(self):
        pass

    def deinit(self):
        self._bus.deinit()
