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

    def show_atomic(self, data, w, h):
        """整頁原子更新 (零撕裂的統一高階介面)。

        命令式 bus (SPI/I80/I2C/RGB): 設全螢幕視窗 + RAMWR 推整幀 (面板 GRAM
        更新原子, 本來就不撕裂)。
        DSI/RGB-PSRAM: 由子類覆寫成 page-flip (離屏 fb 寫完整幀 → 幀邊界原子切換)。
        預設實作 = 命令式退化路徑。"""
        self.set_window(0, 0, w - 1, h - 1)
        self.write_data_async(data)
        self.flush()


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

    def write_frame(self, data):
        """整幀傳輸 (RGB: 流式寫入 + flush, 對齊其他 adapter 語義)"""
        self.write_data_async(data)
        self.flush()

    def write_frame_dma(self, data, chunk=32768):
        """對齊 SPI adapter: 回傳 tid 列表 (RGB write 異步 DMA)"""
        tid = self.write_data_async(data, chunk)
        return [tid] if tid is not None else []

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
    """MIPI DSI (DPI video mode) — P4 + DSI + fb_count>=2 雙緩衝預設模型。

    底層 C 層合約 (簡單 & 零撕裂 & 零複雜決策):
      * bus.write(buf)      資料搬到後台 frame buffer (離屏 fbs[1-cur_fb])
                            搬運引擎 = PPA (P4 的 DMA2D 硬體) 2D blit,
                            CPU 不碰像素 (src 在 DRAM 時自動先 bounce 到 PSRAM)
                            返回即同步完成 (tid=0, wait(0) 直接通過)
                            零撕裂 (不寫前台 fb)、零 esp_cache_msync ERROR
      * bus.present()       下一幀 VSYNC 原子切換 cur_fb，回傳 tid 等幀邊界
      * set_window(...)     軟體維護視窗 (對齊 SPI/RAMWR 模型)
      * write_cmd(...)      DBI 命令通道供 JD9165 init sequence 使用

    撕裂: write() 永不寫前台 → 零撕裂；present() 才在 VSYNC 原子切換 → 零撕裂
          使用者唯一要做的事：「寫完後 call present() 就看到新畫面」
          fb_count=1 則退化為寫前台 (存在撕裂風險, 不鼓勵但不強制阻擋)

    ⚠ 同步合約: write() C 層同步完成 (回 tid=0, wait(0) 直接通過);
    present() 異步入隊回 tid, wait(tid) 等到下一個幀邊界 (VSYNC) 才算完成
    (IDF draw_bitmap 對內部 fb 只做 cache writeback + 切 fb index, 非阻塞)。
    """
    def __init__(self, bus, width, height):
        self._bus = bus
        self._w = width
        self._h = height
        self._dma = hasattr(bus, 'wait') and hasattr(bus, 'pending')
        # page-flip 能力: C 層需暴露 back_buffer() + present() (fb_count>=2)
        self._pflip = hasattr(bus, 'back_buffer') and hasattr(bus, 'present')

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
        """流式寫入目前視窗 — 位置/分段由 C 層 set_window+write 處理。
        DSI C 層已同步完成 (tid=0)，此函式只是為了對齊其他 adapter 的語義。"""
        try:
            tid = self._bus.write(data)
            # C 層 write 已同步完成 (tid=0)，但仍回傳 non-None 讓上層可選擇要不要 wait
            return tid
        except RuntimeError as e:
            self._log_err("write_data_async", e)
            return None

    def set_window(self, x0, y0, x1, y1):
        self._bus.set_window(x0, y0, x1, y1)

    def show_atomic(self, data, w, h):
        """整頁零撕裂更新 — 後台寫入 + present 切換。

        簡單原則：雙緩開啟時，寫後台 → 切前台，別讓使用者選、不用懂 API 歷史包袱。
        走 C write() (PPA 硬體搬運; src 在 PSRAM 時零拷貝), 避免
        back_buffer()[:]=data 的 CPU 整幀拷貝 + present 整幀 cache 寫回。"""
        if not self._pflip:
            return super().show_atomic(data, w, h)   # 退化 (命令式)
        self._bus.set_window(0, 0, w - 1, h - 1)     # 視窗=整頁, 流式位置歸零
        self._bus.write(data)                        # PPA → 後台 fb (不碰前台, 零撕裂)
        tid = self._bus.present()                    # C: 選離屏+flip+入隊, 回 tid
        if self._dma and tid is not None:
            self._bus.wait(tid)                      # 等幀邊界 (與 write 同款)

    def write_frame(self, data):
        """整幀傳輸 (DSI: 寫後台 + 自動 present，對齊其他 adapter 語義)"""
        self._bus.write(data)
        if self._pflip:
            tid = self._bus.present()
            if self._dma and tid:
                self._bus.wait(tid)

    def write_frame_dma(self, data, chunk=32768):
        """對齊 SPI adapter：回傳 tids 列表 (DSI C 層同步, tid=0 但仍回來)"""
        try:
            tids = []
            tid = self._bus.write(data)
            tids.append(tid)
            if self._pflip:
                tid = self._bus.present()
                tids.append(tid)
            return tids
        except RuntimeError as e:
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
