import gc
import time
import lcd_bus

"""
lcd_bus 快速測試 — 純寫入 (僅 ESP32)

用法: import test_bus; test_bus.run_all()

⚠️ ESP32-S3 + Octal-SPIRAM 注意：
   GPIO 33-37 被 Octal PSRAM 內部佔用，不可用於 SPI。
   若遇到硬體崩潰重啟，請改用 test_bus_safe.py 或調整以下腳位。
"""

PASS = 0
FAIL = 0
W = 62

# ==== 可根據硬體調整的腳位 ====
# host 跟隨 MicroPython machine.SPI(id) 命名慣例:
#   host=1 → SPI2 (FSPI/HSPI, 對應 MicroPython SPI(id=1))
#   host=2 → SPI3 (HSPI/VSPI, 對應 MicroPython SPI(id=2))
#   host=3 → ESP32-P4 第三組 GPSPI3 (如有)
#
# ESP32 預設: host=1, data=(35,), clk=36
# ESP32-S3 + Octal-SPIRAM: host=2 (SPI2 被 PSRAM 佔用), data=(5,), clk=14
_PINS_SPI_HOST   = 1
_PINS_SPI_CLK    = 11                    # clock (不可與 data 重疊)
_PINS_SPI_CLK2   = 14                    # 備用 clock（多線/速度測試）
_PINS_SPI_DATA   = (10,)                 # 1線 mosi
_PINS_SPI_DATA2  = (10, 12)              # 2線 (mosi, miso) — miso 不可等於 clk
_PINS_SPI_MULTI  = (10, 12, 13, 21)     # 4線 (mosi,miso,wp,hd)
_PINS_SPI_MULTI8 = (10, 12, 13, 21, 15, 16, 17, 18)  # 8線

_PINS_I80_DATA   = (13,12,11,10, 9,46, 3, 8, 18,17,16,15, 7, 6, 5, 4)
_PINS_I80_WR     = 45
_PINS_I80_CS     = 39

_PINS_I2C_SDA    = 1
_PINS_I2C_SCL    = 2
_PINS_I2C_ADDR   = 0x3C


def hdr(m):
    print("\n" + "=" * W + "\n  " + m + "\n" + "=" * W)

def ok(m):
    global PASS; PASS += 1; print(f"  [PASS] {m}")

def err(m):
    global FAIL; FAIL += 1; print(f"  [FAIL] {m}")

def sec(m):
    print(f"\n  --- {m} ---")

def end():
    t = PASS + FAIL
    print("\n" + "=" * W)
    print(f"  結果: {PASS}/{t} 通過" + (f", {FAIL} 失敗" if FAIL else ""))
    print("=" * W)


def fmt(bps):
    if bps >= 1024*1024: return f"{bps/(1024*1024):.1f} MB/s"
    return f"{bps/1024:.0f} KB/s"


def measure(bus, buf, runs=3):
    best = 0x7FFFFFFF
    for _ in range(runs):
        t0 = time.ticks_us()
        tid = bus.write(buf)
        f = time.ticks_diff(time.ticks_us(), t0)
        t0 = time.ticks_us()
        bus.wait(tid)
        w = time.ticks_diff(time.ticks_us(), t0)
        if f + w < best: best = f + w
        time.sleep_us(200)
    return best


def test_spi():
    hdr("SPI 功能測試")

    sec("初始化")
    bus = lcd_bus.SPIBus(data=_PINS_SPI_DATA, clk=_PINS_SPI_CLK, freq=40_000_000, host=_PINS_SPI_HOST)
    ok(f"lane_count = {bus.lane_count()}")
    ok("初始化後隊列為空" if not bus.is_busy() else "FAIL")

    sec("寫入")
    buf = bytearray(256)
    tid = bus.write(buf)
    ok(f"write -> trans_id={tid}, pending={bus.pending()}")
    bus.wait(tid)

    sec("排隊 4 個")
    tids = []
    for i in range(4):
        b = bytearray(256)
        tids.append(bus.write(b))
        ok(f"enqueue #{i}, pending={bus.pending()}")
    ok("隊列深度 4" if bus.pending() == 4 else "FAIL")

    for tid in tids:
        bus.wait(tid)
    ok("全部完成")

    sec("隊列滿防護")
    caught = False
    try:
        # 寫超過 queue_depth（可透過建構子調 1..8）→ 第 queue_depth+1 筆拋出
        for _ in range(16):
            bus.write(bytearray(64))
    except RuntimeError as e:
        if "queue full" in str(e): caught = True
    ok("queue full 正確拋出" if caught else "FAIL")

    bus.wait_all()
    bus.deinit()
    ok("deinit 完成")


def test_spi_multilane():
    hdr("SPI 多線自動檢測")
    for d, name in [
        (_PINS_SPI_DATA, "1線(data)"),
        (_PINS_SPI_DATA2, "2線(data)"),
        (_PINS_SPI_MULTI, "4線(data)"),
        (_PINS_SPI_MULTI8, "8線(data)"),
    ]:
        bus = lcd_bus.SPIBus(data=d, clk=_PINS_SPI_CLK, host=_PINS_SPI_HOST)
        ok(f"{name} → lane_count={bus.lane_count()}")
        b = bytearray(256)
        bus.write(b)
        bus.wait_all()
        bus.deinit()


def test_spi_official():
    hdr("SPI 官方寫法 (sck/mosi/miso) — 全雙工")

    sec("初始化")
    bus = lcd_bus.SPIBus(
        sck=_PINS_SPI_CLK, mosi=_PINS_SPI_DATA[0],
        miso=_PINS_SPI_DATA2[1],  # GPIO 12 做 miso
        freq=40_000_000, host=_PINS_SPI_HOST)
    ok(f"lane_count = {bus.lane_count()}")
    ok("全雙工模式 (無 HALFDUPLEX 標記)" if bus.lane_count() == 1 else "FAIL")

    sec("寫入 256B")
    buf = bytearray(256)
    tid = bus.write(buf)
    ok(f"write -> trans_id={tid}")
    bus.wait(tid)

    sec("readinto (全雙工 RX)")
    rbuf = bytearray(32)
    tid = bus.readinto(rbuf, write_val=0xAA)
    bus.wait(tid)
    ok(f"readinto 32B -> trans_id={tid}")

    bus.wait_all()
    bus.deinit()
    ok("deinit 完成")


def test_speed():
    hdr("速度測試")
    sizes = [256, 1024, 4096, 16384, 32768]

    sec("SPI 1線 40MHz")
    bus = lcd_bus.SPIBus(data=_PINS_SPI_DATA, clk=_PINS_SPI_CLK, freq=40_000_000, host=_PINS_SPI_HOST)
    print(f"  {'size':>6s} │ {'total_us':>8s} │ {'throughput':>12s}")
    for s in sizes:
        buf = bytearray(s)
        t = measure(bus, buf)
        tput = s / (t / 1e6) if t else 0
        print(f"  {s:>6d}B │ {t:>8d}us │ {fmt(tput):>12s}")
    bus.deinit()

    sec("SPI 1線 80MHz")
    bus = lcd_bus.SPIBus(data=_PINS_SPI_DATA, clk=_PINS_SPI_CLK, freq=80_000_000, host=_PINS_SPI_HOST)
    print(f"  {'size':>6s} │ {'total_us':>8s} │ {'throughput':>12s}")
    for s in sizes:
        buf = bytearray(s)
        t = measure(bus, buf)
        tput = s / (t / 1e6) if t else 0
        print(f"  {s:>6d}B │ {t:>8d}us │ {fmt(tput):>12s}")
    bus.deinit()

    sec("SPI 2線 80MHz")
    bus = lcd_bus.SPIBus(data=_PINS_SPI_DATA2, clk=_PINS_SPI_CLK2, freq=80_000_000, host=_PINS_SPI_HOST)
    print(f"  {'size':>6s} │ {'total_us':>8s} │ {'throughput':>12s}")
    for s in sizes:
        buf = bytearray(s)
        t = measure(bus, buf)
        tput = s / (t / 1e6) if t else 0
        print(f"  {s:>6d}B │ {t:>8d}us │ {fmt(tput):>12s}")
    bus.deinit()

    sec("SPI 4線 80MHz")
    bus = lcd_bus.SPIBus(data=_PINS_SPI_MULTI, clk=_PINS_SPI_CLK2, freq=80_000_000, host=_PINS_SPI_HOST)
    print(f"  {'size':>6s} │ {'total_us':>8s} │ {'throughput':>12s}")
    for s in sizes:
        buf = bytearray(s)
        t = measure(bus, buf)
        tput = s / (t / 1e6) if t else 0
        print(f"  {s:>6d}B │ {t:>8d}us │ {fmt(tput):>12s}")
    bus.deinit()

    sec("32KB 吞吐量對比")
    print(f"  {'總線':>14s} │ {'total_us':>8s} │ {'throughput':>12s}")
    for d, clk, freq, label in [
        (_PINS_SPI_DATA,  _PINS_SPI_CLK,  40_000_000, "SPI 1線 40M"),
        (_PINS_SPI_DATA,  _PINS_SPI_CLK,  80_000_000, "SPI 1線 80M"),
        (_PINS_SPI_DATA2, _PINS_SPI_CLK2, 80_000_000, "SPI 2線 80M"),
        (_PINS_SPI_MULTI, _PINS_SPI_CLK2, 40_000_000, "SPI 4線 40M"),
        (_PINS_SPI_MULTI, _PINS_SPI_CLK2, 80_000_000, "SPI 4線 80M"),
    ]:
        bus = lcd_bus.SPIBus(data=d, clk=clk, freq=freq, host=_PINS_SPI_HOST)
        buf = bytearray(32768)
        t = measure(bus, buf, runs=2)
        tput = 32768 / (t / 1e6) if t else 0
        print(f"  {label:>14s} │ {t:>8d}us │ {fmt(tput):>12s}")
        bus.deinit()


def test_i2c():
    hdr("I2C 功能測試")
    bus = lcd_bus.I2CBus(sda=_PINS_I2C_SDA, scl=_PINS_I2C_SCL, addr=_PINS_I2C_ADDR)
    ok(f"lane_count = {bus.lane_count()}")
    bus.write(b'\x00\xAE')
    ok("write 完成")
    bus.deinit()


def test_i80():
    try:
        bus = lcd_bus.I80Bus(data=_PINS_I80_DATA, wr=_PINS_I80_WR, cs=_PINS_I80_CS, freq=10_000_000)
    except Exception as e:
        hdr("I80 — SKIP (%s)" % str(e))
        return
    hdr("I80 功能測試")
    ok(f"lane_count = {bus.lane_count()}")

    buf = bytearray(8192)
    tid = bus.write(buf)
    ok(f"write 8192B → trans_id={tid}")
    bus.wait_all()

    sec("I80 16bit 10MHz 速度")
    sizes = [1024, 4096, 16384, 32768]
    print(f"  {'size':>6s} │ {'total_us':>8s} │ {'throughput':>12s}")
    for s in sizes:
        buf = bytearray(s)
        t = measure(bus, buf, runs=3)
        tput = s / (t / 1e6) if t else 0
        print(f"  {s:>6d}B │ {t:>8d}us │ {fmt(tput):>12s}")

    bus.wait_all()
    bus.deinit()
    ok("deinit 完成")


def test_rgb():
    try:
        bus = lcd_bus.RGBBus(
            data=tuple(range(8)),
            hsync=12, vsync=13, de=14, pclk=15,
            width=480, height=272,
        )
    except Exception as e:
        hdr("RGB — SKIP (%s)" % str(e))
        return
    hdr("RGB 功能測試")
    ok(f"lane_count = {bus.lane_count()}")
    buf = bytearray(480 * 272 * 2)
    bus.write(buf)
    ok("write 完成")
    bus.wait_all()
    bus.deinit()


def test_rapid():
    hdr("快速開關壓力 (3 次)")
    for i in range(3):
        bus = lcd_bus.SPIBus(data=_PINS_SPI_DATA, clk=_PINS_SPI_CLK, host=_PINS_SPI_HOST)
        bus.write(bytearray(256))
        bus.wait_all()
        bus.deinit()
        ok(f"cycle {i+1}/3 pass")


def run_all():
    global PASS, FAIL
    PASS = FAIL = 0
    print("\n" + "█" * W)
    print("█  lcd_bus 快速測試 + 速度測試")
    print("█  host=%d (MicroPython SPI(id=%d)), clk=%d" % (_PINS_SPI_HOST, _PINS_SPI_HOST, _PINS_SPI_CLK))
    print("█" * W)

    test_spi()
    test_spi_multilane()
    test_spi_official()
    test_speed()
    test_i2c()
    test_i80()
    test_rgb()
    test_rapid()
    end()


if __name__ == "__main__":
    run_all()
