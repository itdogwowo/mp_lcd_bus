class BusAdapter:
    def write_cmd(self, cmd):
        raise NotImplementedError

    def write_data(self, data):
        return self.write_data_async(data)

    def write_cmd_data(self, cmd, data=None):
        self.write_cmd(cmd)
        if data:
            self.write_data(data)

    def set_window(self, x0, y0, x1, y1):
        self.write_cmd(0x2A)
        self.write_data(bytes([x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF]))
        self.write_cmd(0x2B)
        self.write_data(bytes([y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF]))
        self.write_cmd(0x2C)

    def reset(self):
        raise NotImplementedError

    def write_data_async(self, data):
        raise NotImplementedError

    def flush(self):
        pass

    def wait(self, handle):
        pass


class SpiBusAdapter(BusAdapter):
    def __init__(self, spi, dc=None, cs=None, rst=None, bounce_size=32768):
        self._spi = spi
        self._dc = dc
        self._cs = cs
        self._rst = rst
        self._dma = (hasattr(spi, 'wait') and hasattr(spi, 'pending')
                     and hasattr(spi, 'wait_all'))
        self._qspi = hasattr(spi, 'lane_count') and spi.lane_count() > 1
        self._bounce_size = int(bounce_size)
        self._bounce = None
        if self._dma:
            # 內建 DMA bounce buffer（內部 SRAM）— 供大 buffer / PSRAM 分 chunk 過
            try:
                import heap_caps
                b = heap_caps.malloc(self._bounce_size, heap_caps.CAP_DMA)
                if b is not None:
                    self._bounce = b
            except Exception:
                self._bounce = None

    def close(self):
        """釋放內建 bounce buffer（bus 不再使用時呼叫）"""
        if self._bounce is not None:
            try:
                import heap_caps
                heap_caps.free(self._bounce)
            except Exception:
                pass
            self._bounce = None

    def write_cmd(self, cmd):
        if self._qspi:
            self._cs.value(0)
            self._spi.write(b'\x00', cmd=0x02, addr=cmd << 8)
            self._spi.wait_all()
            self._cs.value(1)
        elif self._dma:
            self._dc.value(0)
            self._cs.value(0)
            self._spi.write(bytearray([cmd]))
            self._spi.wait_all()
        else:
            self._dc.value(0)
            self._cs.value(0)
            self._spi.write(bytearray([cmd]))
            self._cs.value(1)

    def write_cmd_data(self, cmd, data=None):
        if self._qspi:
            self._cs.value(0)
            payload = data if data else b'\x00'
            self._spi.write(payload, cmd=0x02, addr=cmd << 8)
            self._spi.wait_all()
            self._cs.value(1)
        elif self._dma:
            self._dc.value(0)
            self._cs.value(0)
            self._spi.write(bytearray([cmd]))
            self._spi.wait_all()
            if data:
                self._dc.value(1)
                self._spi.write(data)
                self._spi.wait_all()
        else:
            self.write_cmd(cmd)
            if data:
                self.write_data(data)

    def write_data_async(self, data):
        if self._qspi:
            self._cs.value(0)
            try:
                return self._spi.write(data)
            except RuntimeError as e:
                self._log_err("write_data_async qspi", e)
                return None
        if self._dma:
            self._dc.value(1)
            # 大 buffer（>32KB max_transfer_sz）→ 自動分 chunk 過 bounce
            if self._bounce is not None and len(data) > self._bounce_size:
                return self._write_bounced(data)
            # 正常單筆：queue 滿（RuntimeError）→ wait_all 清空後重試一次
            for attempt in range(2):
                try:
                    return self._spi.write(data)
                except RuntimeError as e:
                    try:
                        self._spi.wait_all()
                    except Exception:
                        pass
                    if attempt == 0:
                        continue
                    self._log_err("write_data_async", e)
                    return None
            return None
        self._dc.value(1)
        self._cs.value(0)
        self._spi.write(data)
        self._cs.value(1)
        return True

    def _write_bounced(self, data):
        """大 buffer 自動分 chunk 過 bounce 送（解決 >32KB 上限 + PSRAM 直送問題）。
        每 chunk 送後 queue 滿時 wait_all 讓出；回傳最後的 tid。"""
        mv = memoryview(data)
        off, rem = 0, len(mv)
        last_tid = None
        while rem > 0:
            n = min(rem, self._bounce_size)
            self._bounce[:n] = mv[off:off + n]
            bmv = self._bounce[:n]
            for attempt in range(2):
                try:
                    tid = self._spi.write(bmv)
                    last_tid = tid if tid is not None else last_tid
                    break
                except RuntimeError as e:
                    try:
                        self._spi.wait_all()
                    except Exception:
                        pass
                    if attempt == 0:
                        continue
                    self._log_err("write_data_async chunk", e)
                    return None
            off += n
            rem -= n
        return last_tid

    @staticmethod
    def _log_err(where, e):
        try:
            from lib.log_service import get_log
            get_log().warn("[bus_adapter] {} error: {}".format(where, e))
        except Exception:
            print("[bus_adapter] {} error: {}".format(where, e))

    def flush(self):
        if self._qspi:
            self._spi.wait_all()
            self._cs.value(1)
        elif self._dma:
            self._spi.wait_all()
            self._cs.value(1)

    def write_frame(self, data):
        """整幀阻塞傳輸（每 chunk wait）— 相容舊介面，序列化低效"""
        if self._qspi:
            self._cs.value(0)
            self._spi.write(b'', cmd=0x32, addr=0x002C00, multiline=False)
            mv = memoryview(data)
            off, rem = 0, len(mv)
            while rem > 0:
                n = min(rem, 32768)
                tid = self._spi.write(mv[off:off + n])
                self.wait(tid)
                rem -= n; off += n
            self._cs.value(1)
        elif self._dma:
            self._dc.value(1); self._cs.value(0)
            mv = memoryview(data)
            off, rem = 0, len(mv)
            while rem > 0:
                n = min(rem, 32768)
                tid = self._spi.write(mv[off:off + n])
                self.wait(tid)
                rem -= n; off += n
            self._cs.value(1)
        else:
            self._dc.value(1); self._cs.value(0); self._spi.write(data); self._cs.value(1)

    def write_frame_dma(self, data, chunk=32768):
        """DMA 幀傳輸：分 chunk 填滿 4-deep queue，不逐 chunk wait。
        回傳 tid 列表；caller 需後續 flush()/wait_all() 等完成。
        呼叫前 RAMWR 命令須已送達；本方法負責 DC=1（data 模式）。"""
        if self._dma:
            self._dc.value(1)
            self._cs.value(0)
        elif self._qspi:
            pass  # qspi DC 由命令相位處理
        mv = data if isinstance(data, memoryview) else memoryview(data)
        off, rem = 0, len(mv)
        tids = []
        if self._dma:
            # 分 chunk 填 4-deep queue，pending>=3 時退讓最早的（留 1 slot 餘裕）
            while rem > 0:
                n = min(chunk, rem)
                if self._spi.pending() >= 3 and tids:
                    self._spi.wait(tids.pop(0))
                try:
                    tid = self._spi.write(mv[off:off + n])
                    if tid is not None:
                        tids.append(tid)
                except RuntimeError:
                    # queue full — 等全部清空再重試同一 chunk
                    self._spi.wait_all()
                    continue
                off += n; rem -= n
        elif self._qspi:
            while rem > 0:
                n = min(chunk, rem)
                try:
                    tid = self._spi.write(mv[off:off + n])
                    if tid is not None:
                        tids.append(tid)
                except RuntimeError:
                    self._spi.wait_all()
                    continue
                off += n; rem -= n
        else:
            # 非 DMA：同步送，無 tid
            self._dc.value(1); self._cs.value(0)
            self._spi.write(mv[:len(mv)])
            self._cs.value(1)
        return tids

    def wait(self, handle):
        if self._qspi and handle is not None:
            self._spi.wait(handle)
        elif self._dma and handle is not None:
            self._spi.wait(handle)

    def set_window(self, x0, y0, x1, y1):
        if self._qspi:
            # CASET — CS HIGH between commands (RM67162 requires framing)
            self._cs.value(0)
            self._spi.write(
                bytes([x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF]),
                cmd=0x02, addr=0x2A << 8)
            self._spi.wait_all()
            self._cs.value(1)
            # RASET
            self._cs.value(0)
            self._spi.write(
                bytes([y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF]),
                cmd=0x02, addr=0x2B << 8)
            self._spi.wait_all()
            self._cs.value(1)
            # RAMWR — CS stays low for subsequent pixel data
            self._cs.value(0)
            self._spi.write(b'', cmd=0x32, addr=0x002C00, multiline=False)
            self._spi.wait_all()
        elif self._dma:
            self._cs.value(0)
            self._dc.value(0); self._spi.write(bytearray([0x2A])); self._spi.wait_all()
            self._dc.value(1); self._spi.write(bytes([x0>>8,x0&0xFF,x1>>8,x1&0xFF])); self._spi.wait_all()
            self._dc.value(0); self._spi.write(bytearray([0x2B])); self._spi.wait_all()
            self._dc.value(1); self._spi.write(bytes([y0>>8,y0&0xFF,y1>>8,y1&0xFF])); self._spi.wait_all()
            self._dc.value(0); self._spi.write(bytearray([0x2C])); self._spi.wait_all(); self._dc.value(1)
        else:
            super().set_window(x0, y0, x1, y1)

    def reset(self):
        if self._rst is None:
            return
        self._rst.value(0)
        import time
        time.sleep_ms(50)
        self._rst.value(1)
        time.sleep_ms(50)


class I2cBusAdapter(BusAdapter):
    def __init__(self, i2c, addr, rst=None, cmd_ctrl=0x00, data_ctrl=0x40):
        self._i2c = i2c
        self._addr = addr
        self._rst = rst
        self._cmd_ctrl = cmd_ctrl
        self._data_ctrl = data_ctrl
        self._dma = hasattr(i2c, 'wait') and hasattr(i2c, 'pending')

    def write_cmd(self, cmd):
        buf = bytearray([self._cmd_ctrl, cmd])
        if self._dma:
            self._i2c.write(buf)
            self._i2c.wait_all()
        else:
            self._i2c.writeto(self._addr, buf)

    def write_data_async(self, data, chunk=4096):
        """分 chunk 送，避免大 buffer 一次分配（I2C 同步，無 queue 語意）。
        每 chunk 固定小 bytearray 重複利用，降低 GC 壓力。"""
        total = len(data)
        if total <= chunk:
            buf = bytearray(total + 1)
            buf[0] = self._data_ctrl
            buf[1:] = data
            if self._dma:
                try:
                    return self._i2c.write(buf)
                except RuntimeError as e:
                    self._log_err("write_data_async", e)
                    return None
            self._i2c.writeto(self._addr, buf)
            return True
        # 大 buffer 分 chunk
        off = 0
        while off < total:
            n = min(chunk, total - off)
            buf = bytearray(n + 1)
            buf[0] = self._data_ctrl
            buf[1:] = data[off:off + n]
            if self._dma:
                try:
                    tid = self._i2c.write(buf)
                    if tid is not None:
                        self._i2c.wait(tid)
                except RuntimeError as e:
                    self._log_err("write_data_async chunk", e)
                    return None
            else:
                self._i2c.writeto(self._addr, buf)
            off += n
        return True

    @staticmethod
    def _log_err(where, e):
        try:
            from lib.log_service import get_log
            get_log().warn("[i2c_adapter] {} error: {}".format(where, e))
        except Exception:
            print("[i2c_adapter] {} error: {}".format(where, e))

    def flush(self):
        if self._dma:
            self._i2c.wait_all()

    def wait(self, handle):
        if self._dma and handle is not None:
            self._i2c.wait(handle)

    def reset(self):
        if self._rst is None:
            return
        self._rst.value(0)
        import time
        time.sleep_ms(50)
        self._rst.value(1)
        time.sleep_ms(50)


class I80BusAdapter(BusAdapter):
    def __init__(self, bus, dcx=None, rst=None):
        self._bus = bus
        self._dcx = dcx
        self._rst = rst
        self._dma = hasattr(bus, 'wait') and hasattr(bus, 'pending')

    def write_cmd(self, cmd):
        if self._dcx:
            self._dcx.value(0)
        if self._dma:
            self._bus.write(bytearray([cmd]))
            self._bus.wait_all()
        else:
            self._bus.write(bytearray([cmd]))

    def write_cmd_data(self, cmd, data=None):
        if self._dcx:
            self._dcx.value(0)
        if self._dma:
            self._bus.write(bytearray([cmd]))
            self._bus.wait_all()
        else:
            self._bus.write(bytearray([cmd]))
        if data:
            if self._dcx:
                self._dcx.value(1)
            self._bus.write(data)
            if self._dma:
                self._bus.wait_all()

    def write_data_async(self, data, chunk=32768):
        if self._dcx:
            self._dcx.value(1)
        total = len(data)
        if total <= chunk:
            if self._dma:
                for attempt in range(2):
                    try:
                        return self._bus.write(data)
                    except RuntimeError as e:
                        try:
                            self._bus.wait_all()
                        except Exception:
                            pass
                        if attempt == 0:
                            continue
                        self._log_err("write_data_async", e)
                        return None
            self._bus.write(data)
            return True
        # 大 buffer 分 chunk（對齊 max_transfer_bytes=32KB）
        mv = memoryview(data)
        off = 0
        last_tid = None
        while off < total:
            n = min(chunk, total - off)
            if self._dma:
                for attempt in range(2):
                    try:
                        tid = self._bus.write(mv[off:off + n])
                        last_tid = tid if tid is not None else last_tid
                        break
                    except RuntimeError as e:
                        try:
                            self._bus.wait_all()
                        except Exception:
                            pass
                        if attempt == 0:
                            continue
                        self._log_err("write_data_async chunk", e)
                        return None
            else:
                self._bus.write(mv[off:off + n])
            off += n
        return last_tid if self._dma else True

    @staticmethod
    def _log_err(where, e):
        try:
            from lib.log_service import get_log
            get_log().warn("[i80_adapter] {} error: {}".format(where, e))
        except Exception:
            print("[i80_adapter] {} error: {}".format(where, e))

    def set_window(self, x0, y0, x1, y1):
        if self._dcx:
            self._dcx.value(0)
        w0 = bytearray([0x2A])
        w1 = bytes([x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF])
        w2 = bytearray([0x2B])
        w3 = bytes([y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF])
        w4 = bytearray([0x2C])
        if self._dma:
            self._bus.write(w0); self._bus.wait_all()
            if self._dcx: self._dcx.value(1)
            self._bus.write(w1); self._bus.wait_all()
            if self._dcx: self._dcx.value(0)
            self._bus.write(w2); self._bus.wait_all()
            if self._dcx: self._dcx.value(1)
            self._bus.write(w3); self._bus.wait_all()
            if self._dcx: self._dcx.value(0)
            self._bus.write(w4); self._bus.wait_all()
        else:
            self._bus.write(w0)
            if self._dcx: self._dcx.value(1)
            self._bus.write(w1)
            if self._dcx: self._dcx.value(0)
            self._bus.write(w2)
            if self._dcx: self._dcx.value(1)
            self._bus.write(w3)
            if self._dcx: self._dcx.value(0)
            self._bus.write(w4)

    def flush(self):
        if self._dma:
            self._bus.wait_all()

    def wait(self, handle):
        if self._dma and handle is not None:
            self._bus.wait(handle)

    def reset(self):
        if self._rst is None:
            return
        self._rst.value(0)
        import time
        time.sleep_ms(50)
        self._rst.value(1)
        time.sleep_ms(50)


class RgbBusAdapter(BusAdapter):
    def __init__(self, bus, width, height):
        self._bus = bus
        self._width = width
        self._height = height
        self._dma = hasattr(bus, 'wait') and hasattr(bus, 'pending')

    def write_cmd(self, cmd):
        # RGB 面板無 command 介面（硬體持續掃描）
        pass

    def write_data_async(self, data, chunk=32768):
        total = len(data)
        if total <= chunk:
            if self._dma:
                for attempt in range(2):
                    try:
                        return self._bus.write(data)
                    except RuntimeError as e:
                        try:
                            self._bus.wait_all()
                        except Exception:
                            pass
                        if attempt == 0:
                            continue
                        self._log_err("write_data_async", e)
                        return None
            self._bus.write(data)
            return True
        # 大 buffer 分 chunk
        mv = memoryview(data)
        off = 0
        last_tid = None
        while off < total:
            n = min(chunk, total - off)
            if self._dma:
                for attempt in range(2):
                    try:
                        tid = self._bus.write(mv[off:off + n])
                        last_tid = tid if tid is not None else last_tid
                        break
                    except RuntimeError as e:
                        try:
                            self._bus.wait_all()
                        except Exception:
                            pass
                        if attempt == 0:
                            continue
                        self._log_err("write_data_async chunk", e)
                        return None
            else:
                self._bus.write(mv[off:off + n])
            off += n
        return last_tid if self._dma else True

    def set_window(self, x0, y0, x1, y1):
        # 記憶體映射 bus: 視窗是 C 層軟體狀態 (RGBBus.set_window)
        if hasattr(self._bus, 'set_window'):
            self._bus.set_window(x0, y0, x1, y1)
        else:
            self._log_warn("set_window ignored (bus has no window state)")

    def write_cmd_data(self, cmd, data=None):
        self._log_warn("write_cmd_data ignored (RGB bus has no command interface)")

    @staticmethod
    def _log_err(where, e):
        try:
            from lib.log_service import get_log
            get_log().warn("[rgb_adapter] {} error: {}".format(where, e))
        except Exception:
            print("[rgb_adapter] {} error: {}".format(where, e))

    @staticmethod
    def _log_warn(msg):
        try:
            from lib.log_service import get_log
            get_log().warn("[rgb_adapter] " + msg)
        except Exception:
            print("[rgb_adapter] " + msg)

    def flush(self):
        if self._dma:
            self._bus.wait_all()

    def wait(self, handle):
        if self._dma and handle is not None:
            self._bus.wait(handle)

    def reset(self):
        pass


class DsiBusAdapter(BusAdapter):
    """MIPI DSI (DPI video mode) — 像素由 frame buffer 串流,
    write() = DMA2D 把外部 buffer 拷進「顯示中」的 fb (LVGL 同款路徑)。

    命令語義規劃 (對齊 SPI adapter 的 cmd 規劃):
      * write_cmd / write_cmd_data → DBI 命令通道 (bus.cmd)，供 init sequence
                                     (JD9165 的 page 切換 / SLPOUT / DISPON) 使用
      * CASET(0x2A) / PASET(0x2B) / RAMWR(0x2C) → 忽略: DSI video mode 沒有
        視窗/RAMWR 命令, 視窗語義由 C 層 set_window() 軟體維持
      * set_window(x0,y0,x1,y1)    → 直映 C bus.set_window (軟體視窗 + 流式位置)
      * write_data_async(data)     → bus.write(data): 流式語義在 C 層
                                    (視窗內位置追蹤 + 跨行自動分段), 回傳 tid
      * write_frame_dma(fb)        → [bus.write(fb)] 一次整幀 DMA2D,
                                    回傳 tids 列表 (對齊 SPI 語義)

    撕裂: DMA2D 拷進顯示中 fb，拷貝 ~4ms 期間有一條撕裂線掃過 —
          整幀更新可接受；要零撕裂需 fb_count=2 內部雙緩衝 (見 README)。
    """
    def __init__(self, bus, width, height):
        self._bus = bus
        self._w = width
        self._h = height
        self._dma = hasattr(bus, 'wait') and hasattr(bus, 'pending')

    def write_cmd(self, cmd):
        # video mode 無視窗/RAMWR 命令 → 忽略 (保留 set_window/present 語義)
        if cmd in (0x2A, 0x2B, 0x2C):
            return
        self._bus.cmd(cmd)

    def write_cmd_data(self, cmd, data=None):
        if cmd in (0x2A, 0x2B, 0x2C):
            return
        self._bus.cmd(cmd, param=data if data is not None else b'')

    def write_data_async(self, data, chunk=32768):
        """流式寫入目前視窗 — 位置/分段由 C 層 set_window+write 處理"""
        if self._dma:
            for attempt in range(2):
                try:
                    return self._bus.write(data)
                except RuntimeError as e:
                    try:
                        self._bus.wait_all()
                    except Exception:
                        pass
                    if attempt == 0:
                        continue
                    self._log_err("write_data_async", e)
                    return None
        self._bus.write(data)
        return True

    def set_window(self, x0, y0, x1, y1):
        self._bus.set_window(x0, y0, x1, y1)

    def write_frame(self, data):
        """整幀阻塞傳輸"""
        if self._dma:
            self._bus.write(data)
            self._bus.wait_all()
        else:
            self._bus.write(data)

    def write_frame_dma(self, data, chunk=32768):
        """DMA 幀傳輸 — 一次整幀 DMA2D (無 32KB 分塊限制)。
        回傳 tids 列表 (對齊 SPI adapter 語義)。"""
        try:
            tid = self._bus.write(data)
            return [tid] if tid is not None else []
        except RuntimeError as e:
            self._bus.wait_all()
            try:
                tid = self._bus.write(data)
                return [tid] if tid is not None else []
            except RuntimeError:
                self._log_err("write_frame_dma", e)
                return []

    @staticmethod
    def _log_err(where, e):
        try:
            from lib.log_service import get_log
            get_log().warn("[dsi_adapter] {} error: {}".format(where, e))
        except Exception:
            print("[dsi_adapter] {} error: {}".format(where, e))

    def flush(self):
        if self._dma:
            self._bus.wait_all()

    def wait(self, handle):
        if self._dma and handle is not None:
            self._bus.wait(handle)

    def reset(self):
        # DSI reset 已由 DSIBus(rst=) 建構時處理
        pass
