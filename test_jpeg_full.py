# test_jpeg_full.py
# noise 基準測試：Encoder noise → Decoder 全模式 → 管線播放
#
# 用法：
#   test_jpeg_full.run_bench()        # 純解碼基準（免螢幕）
#   test_jpeg_full.run_pipeline(q=60)  # 管線播放（需螢幕）
#   test_jpeg_full.run_all()           # 兩階段完整測試

import gc, time, random

try:    import jpeg
except ImportError: jpeg = None

try:    import lcd_bus
except ImportError: lcd_bus = None

# ═══════════════════ params ═══════════════════

_W , _H         = 240, 240
_ENC_FORMAT     = "RGB888"
_DEC_FORMAT     = "RGB565_LE"
_DEC_BPP        = 2
_QUALITIES      = [20, 40, 60, 80]
_WARMUP         = 3
_RUNS           = 20

_PINS = {
    "host":1, "clk":11, "data":(10,),
    "dc":12,  "cs":13,  "rst":14, "bl":15,
    "freq":80_000_000,
}
_TFT_CFG = {"driver":"ST7789","width":240,"height":240,"rotation":0,"bpp":2}

# ═══════════════════ TFT init ═══════════════════

_ST7789_INIT = (
    (0x01,None,150),(0x11,None,120),(0x3A,b'\x55',0),
    (0x36,b'\x00',0),(0x13,None,10),(0x29,None,100),
)
_INIT_MAP = {"ST7789": _ST7789_INIT}


# ═══════════════════ utils ═══════════════════

def _alloc_fb(size):
    try:
        import heap_caps
        for caps in (heap_caps.CAP_SPIRAM, heap_caps.CAP_DMA):
            try:
                b = heap_caps.aligned_alloc(16, size, caps)
                if b is not None: return b
            except (AttributeError, TypeError):
                b = heap_caps.malloc(size, caps)
                if b is not None and (id(b)&0xF)==0: return b
    except: pass
    raw = bytearray(size+16)
    off = (16 - (id(raw)&0xF)) & 0xF
    return memoryview(raw)[off:off+size]


def _noise_rgb888(w, h):
    """真實世界壓力測試圖案"""
    buf = bytearray(w*h*3)
    for i in range(len(buf)):
        buf[i] = random.getrandbits(8)&0xFF
    return buf


def _encode_noise(w, h, quality):
    raw = _noise_rgb888(w, h)
    enc = jpeg.Encoder(height=h,width=w,pixel_format=_ENC_FORMAT,
                        quality=quality,rotation=0)
    data = enc.encode(raw)
    del raw, enc  # 立即釋放 raw (172KB) + encoder 內部 buffer
    gc.collect()
    return bytes(data) if isinstance(data,memoryview) else data


# ═══════════════════ Phase 1: pure decode ═══════════════════

def run_bench():
    """noise 純解碼基準 — 多種 decoder 模式 × 多品質"""
    if jpeg is None:
        print("jpeg module not found"); return

    w, h = _W, _H
    fb = _alloc_fb(w*h*_DEC_BPP)
    fb_size = w*h*_DEC_BPP

    print("="*72)
    print("JPEG Decoder Benchmark — noise pattern, {}x{} → {}".format(w,h,_DEC_FORMAT))
    print("="*72)

    for q in _QUALITIES:
        # Q=100 需要連續大塊記憶體，先強制 GC compact
        if q == 100:
            for _ in range(3):
                gc.collect()
                time.sleep_ms(10)
        else:
            gc.collect()

        jb = _encode_noise(w, h, q)
        sz = len(jb)
        ratio = sz*100.0/fb_size

        print("\n  noise Q={} | {}B ({:.1f}%)".format(q, sz, ratio))

        # ── decode_into 系列（直接寫入 framebuffer）──

        # 1. decode_into full (block=False，一發完成)
        dec = jpeg.Decoder(pixel_format=_DEC_FORMAT, block=False)
        for _ in range(_WARMUP):
            dec.decode_into(jb, fb)
        gc.collect(); gc.disable()
        t0 = time.ticks_us()
        for _ in range(_RUNS):
            dec.decode_into(jb, fb)
        t_full = time.ticks_diff(time.ticks_us(), t0)//_RUNS
        gc.enable(); gc.collect()

        # 2. decode_into step (block=True, blocks=1, for 迴圈)
        dec = jpeg.Decoder(pixel_format=_DEC_FORMAT, block=True)
        info = dec.get_img_info(jb)
        blocks = info[2] if len(info)>=3 else 8
        gc.collect(); gc.disable()
        t0 = time.ticks_us()
        for _ in range(_RUNS):
            for _i in range(blocks):
                dec.decode_into(jb, fb, blocks=1)
        t_step1 = time.ticks_diff(time.ticks_us(), t0)//_RUNS
        gc.enable(); gc.collect()

        # 3. decode_into step (blocks=2, while not done)
        dec = jpeg.Decoder(pixel_format=_DEC_FORMAT, block=True)
        gc.collect(); gc.disable()
        t0 = time.ticks_us()
        for _ in range(_RUNS):
            while not dec.decode_into(jb, fb, blocks=2):
                pass
        t_step2 = time.ticks_diff(time.ticks_us(), t0)//_RUNS
        gc.enable(); gc.collect()

        # ── decode 系列（回傳 memoryview/bytes）──

        # 4. decode full (block=False, 回傳 memoryview)
        dec = jpeg.Decoder(pixel_format=_DEC_FORMAT, block=False)
        for _ in range(_WARMUP):
            _ = dec.decode(jb)
        gc.collect(); gc.disable()
        t0 = time.ticks_us()
        for _ in range(_RUNS):
            _ = dec.decode(jb)
        t_dec_full = time.ticks_diff(time.ticks_us(), t0)//_RUNS
        gc.enable(); gc.collect()

        # 5. decode step (block=True, 逐 block 回傳 memoryview)
        dec = jpeg.Decoder(pixel_format=_DEC_FORMAT, block=True)
        _ = dec.get_img_info(jb)
        gc.collect(); gc.disable()
        t0 = time.ticks_us()
        for _ in range(_RUNS):
            for _i in range(blocks):
                block = dec.decode(jb)
                if block is None: break
        t_dec_step = time.ticks_diff(time.ticks_us(), t0)//_RUNS
        gc.enable(); gc.collect()

        # 6. decode step + python slice copy into fb
        dec = jpeg.Decoder(pixel_format=_DEC_FORMAT, block=True)
        _ = dec.get_img_info(jb)
        gc.collect(); gc.disable()
        t0 = time.ticks_us()
        for _ in range(_RUNS):
            for i in range(blocks):
                block = dec.decode(jb)
                if block is None: break
                off = i * len(block)
                fb[off:off+len(block)] = block
        t_dec_copy = time.ticks_diff(time.ticks_us(), t0)//_RUNS
        gc.enable(); gc.collect()

        print("    decode_into(full)     {:>6d}us | {:>5.0f}fps".format(t_full, 1e6/t_full if t_full else 0))
        print("    decode_into(step,N={}) {:>6d}us | {:>5.0f}fps".format(blocks, t_step1, 1e6/t_step1 if t_step1 else 0))
        print("    decode_into(blocks=2)  {:>6d}us | {:>5.0f}fps".format(t_step2, 1e6/t_step2 if t_step2 else 0))
        print("    decode(full)           {:>6d}us | {:>5.0f}fps".format(t_dec_full, 1e6/t_dec_full if t_dec_full else 0))
        print("    decode(step,N={})     {:>6d}us | {:>5.0f}fps".format(blocks, t_dec_step, 1e6/t_dec_step if t_dec_step else 0))
        print("    decode(step+copy)      {:>6d}us | {:>5.0f}fps".format(t_dec_copy, 1e6/t_dec_copy if t_dec_copy else 0))

        del jb; gc.collect()

    if hasattr(gc,'mem_free'):
        gc.collect()
        print("\n  GC free: {} KB".format(gc.mem_free()//1024))
    print("done.")


def bench_quality(q):
    """單一品質測試（獨立記憶體空間）"""
    if jpeg is None:
        print("jpeg module not found"); return

    w, h = _W, _H
    fb = _alloc_fb(w*h*_DEC_BPP)
    fb_size = w*h*_DEC_BPP

    gc.collect(); time.sleep_ms(50)
    jb = _encode_noise(w, h, q)
    sz = len(jb)
    ratio = sz*100.0/fb_size

    print("="*72)
    print("noise Q={} | {}B ({:.1f}%)".format(q, sz, ratio))
    print("="*72)

    # 1. decode_into full
    dec = jpeg.Decoder(pixel_format=_DEC_FORMAT, block=False)
    for _ in range(_WARMUP):
        dec.decode_into(jb, fb)
    gc.collect(); gc.disable()
    t0 = time.ticks_us()
    for _ in range(_RUNS):
        dec.decode_into(jb, fb)
    t_full = time.ticks_diff(time.ticks_us(), t0)//_RUNS
    gc.enable(); gc.collect()

    # 2. decode_into step
    dec = jpeg.Decoder(pixel_format=_DEC_FORMAT, block=True)
    info = dec.get_img_info(jb)
    blocks = info[2] if len(info)>=3 else 8
    gc.collect(); gc.disable()
    t0 = time.ticks_us()
    for _ in range(_RUNS):
        for _i in range(blocks):
            dec.decode_into(jb, fb, blocks=1)
    t_step1 = time.ticks_diff(time.ticks_us(), t0)//_RUNS
    gc.enable(); gc.collect()

    # 3. decode_into blocks=2
    dec = jpeg.Decoder(pixel_format=_DEC_FORMAT, block=True)
    gc.collect(); gc.disable()
    t0 = time.ticks_us()
    for _ in range(_RUNS):
        while not dec.decode_into(jb, fb, blocks=2):
            pass
    t_step2 = time.ticks_diff(time.ticks_us(), t0)//_RUNS
    gc.enable(); gc.collect()

    # 4. decode full
    dec = jpeg.Decoder(pixel_format=_DEC_FORMAT, block=False)
    for _ in range(_WARMUP):
        _ = dec.decode(jb)
    gc.collect(); gc.disable()
    t0 = time.ticks_us()
    for _ in range(_RUNS):
        _ = dec.decode(jb)
    t_dec_full = time.ticks_diff(time.ticks_us(), t0)//_RUNS
    gc.enable(); gc.collect()

    # 5. decode step
    dec = jpeg.Decoder(pixel_format=_DEC_FORMAT, block=True)
    _ = dec.get_img_info(jb)
    gc.collect(); gc.disable()
    t0 = time.ticks_us()
    for _ in range(_RUNS):
        for _i in range(blocks):
            block = dec.decode(jb)
            if block is None: break
    t_dec_step = time.ticks_diff(time.ticks_us(), t0)//_RUNS
    gc.enable(); gc.collect()

    # 6. decode step+copy
    dec = jpeg.Decoder(pixel_format=_DEC_FORMAT, block=True)
    _ = dec.get_img_info(jb)
    gc.collect(); gc.disable()
    t0 = time.ticks_us()
    for _ in range(_RUNS):
        for i in range(blocks):
            block = dec.decode(jb)
            if block is None: break
            off = i * len(block)
            fb[off:off+len(block)] = block
    t_dec_copy = time.ticks_diff(time.ticks_us(), t0)//_RUNS
    gc.enable(); gc.collect()

    print("  decode_into(full)     {:>6d}us | {:>5.0f}fps".format(t_full, 1e6/t_full if t_full else 0))
    print("  decode_into(step,N={}) {:>6d}us | {:>5.0f}fps".format(blocks, t_step1, 1e6/t_step1 if t_step1 else 0))
    print("  decode_into(blocks=2)  {:>6d}us | {:>5.0f}fps".format(t_step2, 1e6/t_step2 if t_step2 else 0))
    print("  decode(full)           {:>6d}us | {:>5.0f}fps".format(t_dec_full, 1e6/t_dec_full if t_dec_full else 0))
    print("  decode(step,N={})     {:>6d}us | {:>5.0f}fps".format(blocks, t_dec_step, 1e6/t_dec_step if t_dec_step else 0))
    print("  decode(step+copy)      {:>6d}us | {:>5.0f}fps".format(t_dec_copy, 1e6/t_dec_copy if t_dec_copy else 0))

    del jb; gc.collect()
    if hasattr(gc,'mem_free'):
        print("  GC free: {} KB".format(gc.mem_free()//1024))


def bench_all_qualities():
    """分批測試所有品質（每次獨立執行，避免記憶體碎片）"""
    for q in _QUALITIES:
        time.sleep_ms(100)
        bench_quality(q)
        time.sleep_ms(100)


# ═══════════════════ Phase 2: pipeline ═══════════════════

def _send_cmd(bus,dc,cmd,data=None):
    dc.value(0); bus.write(bytearray([cmd])); bus.wait_all()
    if data is not None:
        dc.value(1); bus.write(data); bus.wait_all()

def _tft_init(bus,dc,driver):
    seq = _INIT_MAP.get(driver)
    if seq is None: raise ValueError("unknown driver: "+driver)
    for cmd,data,delay in seq:
        _send_cmd(bus,dc,cmd,data)
        if delay: time.sleep_ms(delay)

def _ramwr_start(bus,dc,w,h):
    """設定寫入窗口 + RAMWR — 只在第一幀調用"""
    dc.value(0); bus.write(bytearray([0x2A]))
    dc.value(1); bus.write(bytes([0,0,(w-1)>>8,(w-1)&0xFF]))
    dc.value(0); bus.write(bytearray([0x2B]))
    dc.value(1); bus.write(bytes([0,0,(h-1)>>8,(h-1)&0xFF]))
    bus.wait_all()
    dc.value(0); bus.write(bytearray([0x2C])); bus.wait_all()
    dc.value(1)


def _ramwr_fast(bus,dc):
    """只送 RAMWR（窗口已在 _ramwr_start 設定，不需重設）"""
    dc.value(0); t = bus.write(bytearray([0x2C]))
    bus.wait(t)
    dc.value(1)


class PipelinePlayer:
    def __init__(self,bus,dc,w,h,bpp,frames,loop=True):
        self._bus=bus; self._dc=dc; self._w=w; self._h=h; self._bpp=bpp
        self._frames=frames; self._loop=loop
        self._fb_size=w*h*bpp
        self._fb_a=_alloc_fb(self._fb_size)
        self._fb_b=_alloc_fb(self._fb_size)
        self._fb_decode=self._fb_a; self._fb_dma=self._fb_b
        self._dec=jpeg.Decoder(pixel_format=_DEC_FORMAT,block=False)
        self._idx=0; self._running=False
        self._decode_us=self._dma_us=self._count=0
        self._pending_tids = []
        self._first = True
        self._window_set = False  # RAMWR start flag

        # DMA buffer: heap_caps 確保內部 SRAM，memoryview 可直接傳 spi.write()
        try:
            import heap_caps
            self._dma_buf = heap_caps.malloc(32 * 1024, heap_caps.CAP_DMA)
            if self._dma_buf is None: raise RuntimeError
        except Exception:
            self._dma_buf = bytearray(32 * 1024)

    def _dma_fire(self, fb=None):
        """發送整幀 → 32KB×4 memoryview slice，零 copy"""
        if fb is None: fb = self._fb_dma
        tids = []; off = 0
        while off < self._fb_size:
            n = min(32 * 1024, self._fb_size - off)
            tid = self._bus.write(fb[off:off+n])
            if tid is not None: tids.append(tid)
            off += n
        return tids

    def run(self,max_frames=0):
        """Pipeline: DMA 在 decode 期間背景跑

        每輪順序：
          1. fire DMA(fb_dma) — 發送上一輪解碼的幀（不 wait）
          2. decode → fb_decode — 與 step 1 的 DMA 並行
          3. swap → wait DMA(tids) — 等 step 1 的 DMA 完成

        第一輪特殊處理（沒有上一幀可發）。
        """
        self._running=True
        total=len(self._frames)
        print("pipeline: {} frames | {}x{} | dual fb".format(total,self._w,self._h))
        t_total=time.ticks_ms()
        t_wait = t_dma = 0
        try:
            while self._running:
                if max_frames and self._count>=max_frames: break

                # 1. fire DMA — 發送上一輪的 fb_dma（第一輪跳過）
                if not self._first:
                    t0=time.ticks_us()
                    _ramwr_fast(self._bus, self._dc)
                    self._pending_tids = self._dma_fire(fb=self._fb_dma)
                    t_dma=time.ticks_diff(time.ticks_us(),t0)

                # 2. decode 當前幀 → fb_decode（與 step 1 DMA 並行）
                t0=time.ticks_us()
                self._dec.decode_into(self._frames[self._idx], self._fb_decode)
                t_decode=time.ticks_diff(time.ticks_us(),t0)

                # 3. swap + 等上一輪 DMA 完成
                self._fb_decode, self._fb_dma = self._fb_dma, self._fb_decode
                if self._first:
                    self._first = False
                else:
                    t0=time.ticks_us()
                    for tid in self._pending_tids:
                        self._bus.wait(tid)
                    t_wait=time.ticks_diff(time.ticks_us(),t0)

                # advance
                self._idx += 1
                if self._idx >= total:
                    if self._loop: self._idx = 0
                    else: break
                self._count += 1

                if self._count % 30 == 0:
                    dt=time.ticks_diff(time.ticks_ms(),t_total)
                    fps=self._count*1000.0/dt if dt else 0
                    print("  frame={} fps={:.1f} | dec={}us dma={}us wait={}us".format(
                        self._count,fps,t_decode,t_dma,t_wait))
                self._decode_us+=t_decode; self._dma_us+=t_dma

        finally:
            for tid in self._pending_tids:
                self._bus.wait(tid)
            dt=time.ticks_diff(time.ticks_ms(),t_total)
            fps=self._count*1000.0/dt if dt else 0
            if self._count:
                print("  avg decode: {}us | avg dma: {}us".format(
                    self._decode_us//self._count,self._dma_us//max(self._count-1,1)))
            print("done: {} frames {:.1f}s | {:.1f} fps".format(
                self._count,dt/1000.0,fps))
    def stop(self): self._running=False


def _encode_frames_noise(w,h,n,quality):
    frames=[]
    for i in range(n):
        jb=_encode_noise(w,h,quality)
        frames.append(jb)
    return frames


def run_pipeline(quality=60):
    if jpeg is None: print("jpeg not found"); return
    if lcd_bus is None: print("lcd_bus not found"); return
    from machine import Pin
    w,h,bpp=_TFT_CFG["width"],_TFT_CFG["height"],_TFT_CFG["bpp"]
    dc=Pin(_PINS["dc"],Pin.OUT,value=0)
    cs=Pin(_PINS["cs"],Pin.OUT,value=1) if _PINS["cs"]>=0 else None
    rst=Pin(_PINS["rst"],Pin.OUT,value=1) if _PINS["rst"]>=0 else None
    bl=Pin(_PINS["bl"],Pin.OUT,value=0) if _PINS["bl"]>=0 else None
    if rst: rst.value(0); time.sleep_ms(10); rst.value(1); time.sleep_ms(10)
    spi=lcd_bus.SPIBus(data=_PINS["data"],clk=_PINS["clk"],
                        freq=_PINS["freq"],host=_PINS["host"])
    print("SPIBus host={} lane={}".format(_PINS["host"],spi.lane_count()))
    _tft_init(spi,dc,_TFT_CFG["driver"])
    if bl: bl.value(1)
    print("generating noise frames (Q={})...".format(quality))
    frames=_encode_frames_noise(w,h,5,quality)
    player=PipelinePlayer(spi,dc,w,h,bpp,frames,loop=True)
    try: player.run(max_frames=100)
    except KeyboardInterrupt: player.stop()
    finally:
        if bl: bl.value(0)
        spi.deinit()
        print("clean")


def run_bench_q80_full():
    """Q=80 三步測試：純寫 → 純解碼 → 混合管線"""
    if jpeg is None: print("jpeg not found"); return
    if lcd_bus is None: print("lcd_bus not found"); return
    from machine import Pin

    w, h, bpp = _TFT_CFG["width"], _TFT_CFG["height"], _TFT_CFG["bpp"]
    fb_size = w * h * bpp
    dc = Pin(_PINS["dc"], Pin.OUT, value=0)
    cs = Pin(_PINS["cs"], Pin.OUT, value=1) if _PINS["cs"]>=0 else None
    rst = Pin(_PINS["rst"], Pin.OUT, value=1) if _PINS["rst"]>=0 else None
    bl = Pin(_PINS["bl"], Pin.OUT, value=0) if _PINS["bl"]>=0 else None
    if rst: rst.value(0); time.sleep_ms(10); rst.value(1); time.sleep_ms(10)

    spi = lcd_bus.SPIBus(data=_PINS["data"], clk=_PINS["clk"],
                          freq=_PINS["freq"], host=_PINS["host"])
    print("SPIBus host={} lane={}".format(_PINS["host"], spi.lane_count()))

    # ── heap_caps 記憶體診斷 ──
    try:
        import heap_caps
        for name, caps in [
            ("DMA      ", heap_caps.CAP_DMA),
            ("SPIRAM   ", heap_caps.CAP_SPIRAM),
            ("INTERNAL ", heap_caps.CAP_INTERNAL),
            ("DEFAULT  ", heap_caps.CAP_DEFAULT),
        ]:
            free  = heap_caps.get_free_size(caps)
            total = heap_caps.get_total_size(caps)
            largest = heap_caps.get_largest_free_block(caps)
            print("  {}: free={}KB total={}KB largest={}KB".format(
                name, free//1024, total//1024, largest//1024))
    except Exception as e:
        print("  heap_caps unavailable: {}".format(e))

    _tft_init(spi, dc, _TFT_CFG["driver"])
    if bl: bl.value(1)

    # 一次性設定全螢幕窗口（後續每幀只需 RAMWR）
    _ramwr_start(spi, dc, w, h)

    # ── 1. 純 DMA 寫速度 ──
    print("\n── 1. Pure DMA write (no decode) ──")
    fb = _alloc_fb(fb_size)
    chunk = bytearray(32*1024)
    runs = 30
    times = []
    for i in range(runs + 3):
        t0 = time.ticks_us()
        _ramwr_fast(spi, dc)
        off = 0; tids = []
        while off < fb_size:
            n = min(32*1024, fb_size - off)
            if spi.pending() >= 2:
                spi.wait(tids[0]); tids.pop(0)
            chunk[:n] = fb[off:off+n]
            tid = spi.write(chunk)
            if tid is not None: tids.append(tid)
            off += n
        for tid in tids: spi.wait(tid)
        elapsed = time.ticks_diff(time.ticks_us(), t0)
        if i >= 3: times.append(elapsed)

    times.sort()
    t_dma_avg = sum(times)//len(times)
    fps_dma = 1e6/t_dma_avg if t_dma_avg else 0
    print("  avg: {}us | best: {}us | {:.0f} fps".format(
        t_dma_avg, times[0], fps_dma))

    # ── 2. 純解碼（複用 Phase 1 數據）──
    jb = _encode_noise(w, h, 80)
    dec = jpeg.Decoder(pixel_format=_DEC_FORMAT, block=False)
    for _ in range(_WARMUP): dec.decode_into(jb, fb)
    gc.collect(); gc.disable()
    t0 = time.ticks_us()
    for _ in range(_RUNS): dec.decode_into(jb, fb)
    t_decode = time.ticks_diff(time.ticks_us(), t0)//_RUNS
    gc.enable(); gc.collect()
    fps_dec = 1e6/t_decode if t_decode else 0
    print("\n── 2. Pure decode (no DMA) ──")
    print("  avg: {}us | {:.0f} fps".format(t_decode, fps_dec))

    # ── 3. 混合管線 ──
    print("\n── 3. Pipeline (decode ∥ DMA) ──")
    frames = _encode_frames_noise(w, h, 5, 80)
    player = PipelinePlayer(spi, dc, w, h, bpp, frames, loop=True)
    try: player.run(max_frames=100)
    except KeyboardInterrupt: player.stop()

    # ── 總結 ──
    print("\n── Summary Q=80 noise ──")
    print("  pure decode:  {}us ({:.0f}fps)".format(t_decode, fps_dec))
    print("  pure DMA:     {}us ({:.0f}fps)".format(t_dma_avg, fps_dma))
    print("  limit = max(decode,DMA) = {}us ({:.0f}fps) — 理論上限".format(
        max(t_decode, t_dma_avg), 1e6/max(t_decode, t_dma_avg) if max(t_decode, t_dma_avg) else 0))

    if bl: bl.value(0)
    spi.deinit()
    print("clean")


# ═══════════════════ entry ═══════════════════

def run_all():
    gc.collect()
    print("\n"+"="*72)
    print("PHASE 1: noise decode benchmark (per quality)")
    print("="*72)
    bench_all_qualities()

    # Phase 1 → Phase 2: 釋放所有暫存記憶體
    gc.collect(); time.sleep_ms(200)
    gc.collect(); time.sleep_ms(200)

    # heap_caps.reset() 清空之前所有 DMA 配置，還原 DMA pool
    try:
        import heap_caps
        heap_caps.reset()
        print("  heap_caps.reset() — DMA pool cleaned")
    except Exception:
        pass

    print("\n"+"="*72)
    print("PHASE 2: pipeline playback (noise, Q=80)")
    print("="*72)
    time.sleep_ms(500)
    run_bench_q80_full()
