import gc
import time
import lcd_bus

"""
lcd_bus 安全測試 — ESP32-S3 + Octal-SPIRAM 適用
避免使用 GPIO 33-37（被 Octal PSRAM 佔用）

安全腳位建議:
  SPI data: 5,6,7,8,9,10,11,12
  SPI clk:  14
  I2C:      SDA=1, SCL=2 (或其他空閒腳位)

用法: import test_bus_safe; test_bus_safe.run_all()
"""

PASS = 0
FAIL = 0
W = 62

# ==== 請根據你的硬體接線調整以下腳位 ====
# host 跟隨 MicroPython machine.SPI(id) 命名慣例:
#   host=1 → SPI2 (對應 MicroPython SPI(id=1))
#   host=2 → SPI3 (對應 MicroPython SPI(id=2))
# ESP32-S3 + Octal-SPIRAM: SPI2 被 PSRAM 佔用, 須改用 host=2
SPI_HOST = 2
SPI_DATA = (5,)                # MOSI (1線模式)
SPI_MULTI = (5, 6, 7, 8)     # 多線模式用的 data pins
SPI_CLK = 14                   # clock
SPI_CLK_ALT = 15               # 備用 clock（多線測試用，避免和 14 衝）
SPI_FREQ = 40_000_000

I2C_SDA = 1
I2C_SCL = 2
I2C_ADDR = 0x3C


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
    if bps >= 1024 * 1024:
        return f"{bps / (1024 * 1024):.1f} MB/s"
    return f"{bps / 1024:.0f} KB/s"


def measure(bus, buf, runs=3):
    best = 0x7FFFFFFF
    for _ in range(runs):
        t0 = time.ticks_us()
        tid = bus.write(buf)
        f = time.ticks_diff(time.ticks_us(), t0)
        t0 = time.ticks_us()
        bus.wait(tid)
        w = time.ticks_diff(time.ticks_us(), t0)
        if f + w < best:
            best = f + w
        time.sleep_us(200)
    return best


def test_basic():
    hdr("最小冒煙測試 — 確認 SPI 初始化正常")
    sec("SPI 初始化 (data=%s, clk=%d)" % (str(SPI_DATA), SPI_CLK))
    try:
        bus = lcd_bus.SPIBus(data=SPI_DATA, clk=SPI_CLK, freq=SPI_FREQ, host=SPI_HOST)
    except Exception as e:
        err("初始化失敗: " + str(e))
        return False
    ok("初始化成功 lane_count=%d" % bus.lane_count())

    sec("寫入 256 bytes")
    try:
        buf = bytearray(256)
        tid = bus.write(buf)
        bus.wait(tid)
        ok("寫入+等待完成")
    except Exception as e:
        err("寫入失敗: " + str(e))
        bus.deinit()
        return False

    bus.deinit()
    ok("deinit 完成")
    return True


def test_spi():
    hdr("SPI 功能測試")

    sec("初始化")
    bus = lcd_bus.SPIBus(data=SPI_DATA, clk=SPI_CLK, freq=SPI_FREQ, host=SPI_HOST)
    ok(f"lane_count = {bus.lane_count()}")
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
        if "queue full" in str(e):
            caught = True
    ok("queue full 正確拋出" if caught else "FAIL")

    bus.wait_all()
    bus.deinit()
    ok("deinit 完成")


def test_speed():
    hdr("速度測試")
    sizes = [256, 1024, 4096, 16384, 32768]

    sec("SPI 1線 40MHz")
    bus = lcd_bus.SPIBus(data=SPI_DATA, clk=SPI_CLK, freq=40_000_000, host=SPI_HOST)
    print(f"  {'size':>6s} │ {'total_us':>8s} │ {'throughput':>12s}")
    for s in sizes:
        buf = bytearray(s)
        t = measure(bus, buf)
        tput = s / (t / 1e6) if t else 0
        print(f"  {s:>6d}B │ {t:>8d}us │ {fmt(tput):>12s}")
    bus.deinit()


def test_i2c():
    hdr("I2C 功能測試")
    try:
        bus = lcd_bus.I2CBus(sda=I2C_SDA, scl=I2C_SCL, addr=I2C_ADDR)
        ok(f"lane_count = {bus.lane_count()}")
        bus.write(b'\x00\xAE')
        ok("write 完成")
        bus.deinit()
    except Exception as e:
        err("I2C 失敗: " + str(e))


def test_rapid():
    hdr("快速開關壓力 (3 次)")
    for i in range(3):
        bus = lcd_bus.SPIBus(data=SPI_DATA, clk=SPI_CLK, host=SPI_HOST)
        bus.write(bytearray(256))
        bus.wait_all()
        bus.deinit()
        ok(f"cycle {i + 1}/3 pass")


def run_all():
    global PASS, FAIL
    PASS = FAIL = 0
    print("\n" + "█" * W)
    print("█  lcd_bus 安全測試 (ESP32-S3 + Octal-SPIRAM)")
    print("█  使用腳位: data=%s, clk=%d" % (str(SPI_DATA), SPI_CLK))
    print("█" * W)

    if not test_basic():
        end()
        return

    test_spi()
    test_speed()
    test_i2c()
    test_rapid()
    end()


if __name__ == "__main__":
    run_all()
