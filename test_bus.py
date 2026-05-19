import gc
import time
import lcd_bus

"""
lcd_bus 快速測試 — 純寫入 (僅 ESP32)

用法: import test_bus; test_bus.run_all()
"""

PASS = 0
FAIL = 0
W = 62


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
    bus = lcd_bus.SPIBus(data=(35,), clk=36, freq=40_000_000)
    ok(f"lane_count = {bus.lane_count}")
    ok("初始化後隊列為空" if not bus.is_busy() else "FAIL")

    sec("寫入")
    buf = bytearray(256)
    tid = bus.write(buf)
    ok(f"write -> trans_id={tid}, pending={bus.pending()}")

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
        for _ in range(8):
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
        ((35,), "1線"),
        ((35, 36), "2線"),
        ((35, 36, 37, 38), "4線"),
        ((35, 36, 37, 38, 39, 40, 41, 42), "8線"),
    ]:
        bus = lcd_bus.SPIBus(data=d, clk=36)
        ok(f"{name} → lane_count={bus.lane_count}")
        b = bytearray(256)
        bus.write(b)
        bus.wait_all()
        bus.deinit()


def test_speed():
    hdr("速度測試")
    sizes = [256, 1024, 4096, 16384, 32768]

    sec("SPI 1線 40MHz")
    bus = lcd_bus.SPIBus(data=(35,), clk=36, freq=40_000_000)
    print(f"  {'size':>6s} │ {'total_us':>8s} │ {'throughput':>12s}")
    for s in sizes:
        buf = bytearray(s)
        t = measure(bus, buf)
        tput = s / (t / 1e6) if t else 0
        print(f"  {s:>6d}B │ {t:>8d}us │ {fmt(tput):>12s}")
    bus.deinit()

    sec("SPI 4線 80MHz")
    bus = lcd_bus.SPIBus(data=(35, 36, 37, 38), clk=39, freq=80_000_000)
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
        ((35,),          36, 40_000_000, "SPI 1線 40M"),
        ((35,36,37,38),  39, 40_000_000, "SPI 4線 40M"),
        ((35,36,37,38),  39, 80_000_000, "SPI 4線 80M"),
    ]:
        bus = lcd_bus.SPIBus(data=d, clk=clk, freq=freq)
        buf = bytearray(32768)
        t = measure(bus, buf, runs=2)
        tput = 32768 / (t / 1e6) if t else 0
        print(f"  {label:>14s} │ {t:>8d}us │ {fmt(tput):>12s}")
        bus.deinit()


def test_i2c():
    hdr("I2C 功能測試")
    bus = lcd_bus.I2CBus(sda=21, scl=22, addr=0x3C)
    ok(f"lane_count = {bus.lane_count}")
    bus.write(b'\x00\xAE')
    ok("write 完成")
    bus.deinit()


def test_i80():
    try:
        bus = lcd_bus.I80Bus(data=tuple(range(8)), wr=10)
    except NotImplementedError:
        hdr("I80 — SKIP (not supported)")
        return
    hdr("I80 功能測試")
    ok(f"lane_count = {bus.lane_count}")
    buf = bytearray(8192)
    bus.write(buf)
    ok("write 完成")
    bus.wait_all()
    bus.deinit()


def test_rgb():
    try:
        bus = lcd_bus.RGBBus(
            data=tuple(range(8)),
            hsync=12, vsync=13, de=14, pclk=15,
            width=480, height=272,
        )
    except NotImplementedError:
        hdr("RGB — SKIP (not supported)")
        return
    hdr("RGB 功能測試")
    ok(f"lane_count = {bus.lane_count}")
    buf = bytearray(480 * 272 * 2)
    bus.write(buf)
    ok("write 完成")
    bus.wait_all()
    bus.deinit()


def test_rapid():
    hdr("快速開關壓力 (3 次)")
    for i in range(3):
        bus = lcd_bus.SPIBus(data=(35,), clk=36)
        bus.write(bytearray(256))
        bus.wait_all()
        bus.deinit()
        ok(f"cycle {i+1}/3 pass")


def run_all():
    global PASS, FAIL
    PASS = FAIL = 0
    print("\n" + "█" * W + "\n█  lcd_bus 快速測試 + 速度測試\n█" * W)

    test_spi()
    test_spi_multilane()
    test_speed()
    test_i2c()
    test_i80()
    test_rgb()
    test_rapid()
    end()


if __name__ == "__main__":
    run_all()
