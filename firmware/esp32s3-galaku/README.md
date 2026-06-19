# ESP32-S3 GALAKU 固件

这个目录包含本项目实际使用的 ESP32-S3 固件源码。

固件作用：

1. ESP32-S3 通过 BLE 扫描并连接目标设备 `GK36`。
2. 发现 GALAKU/GK36 写入特征。
3. 通过 USB Serial/JTAG 接收电脑端命令。
4. 把 `SET`、`HIT`、`STOP` 等命令转换为 BLE 控制包。
5. 与仓库根目录的 `esp32-bridge.ps1` 和 `vibration-control.py` 配合使用。

## 目录结构

```text
firmware/esp32s3-galaku/
  CMakeLists.txt
  partitions.csv
  sdkconfig.defaults
  main/
    CMakeLists.txt
    main.c
```

## 环境要求

- ESP-IDF 5.5 或相近版本
- ESP32-S3 开发板
- USB Serial/JTAG 可用
- 目标 BLE 设备名称为 `GK36`

## 构建与烧录

进入固件目录：

```powershell
cd firmware\esp32s3-galaku
```

设置目标：

```powershell
idf.py set-target esp32s3
```

构建：

```powershell
idf.py build
```

烧录并打开监视器：

```powershell
idf.py -p COM3 flash monitor
```

如果你的开发板不是 `COM3`，把命令里的端口改成自己的实际串口。

## 串口命令

烧录后，固件会通过 USB Serial/JTAG 接收纯文本命令，每条命令以换行结尾：

```text
PING
STATUS
SCAN
SERVICES
SET <0-100>
HIT <damage>
STOP
```

返回示例：

```text
PONG
STATUS ble=1 host_synced=1 scanning=0 connecting=0 connected=1 service_ready=1 target=GK36 level=20 handle=10
OK SET 20
OK HIT damage=3.00 level=30
OK STOP
ERR unknown command: ...
```

## 与 Windows 桥配合

固件烧录完成后，在仓库根目录启动 PowerShell 桥：

```powershell
powershell -ExecutionPolicy Bypass -File .\esp32-bridge.ps1 -SerialPort COM3
```

然后用 Python CLI 测试：

```powershell
py .\vibration-control.py --ping
py .\vibration-control.py --status
py .\vibration-control.py --set 20
py .\vibration-control.py --hit 3
py .\vibration-control.py --stop
```

通信链路如下：

```text
vibration-control.py
        ↓ TCP 127.0.0.1:25363
esp32-bridge.ps1
        ↓ USB Serial/JTAG
ESP32-S3 固件
        ↓ BLE
GK36 / GALAKU 设备
```

## 行为说明

- `SET <0-100>`：直接设置当前强度。
- `HIT <damage>`：事件式反馈。固件会把 damage 换算为强度增量，并维持一段时间后逐步衰减。
- `STOP`：立即归零并发送停止包。

当前固件中的默认行为：

- `HOLD_MS = 7000`：一次触发后约维持 7 秒。
- `DECAY_PER_TICK = 1`：超过维持时间后逐步下降。
- `DAMAGE_TO_PERCENT = 10`：`HIT` 的 damage 到强度增量换算倍率。

如果你要改反馈节奏，可以在 `main/main.c` 里调整这些常量。

## 注意

- `build/`、`sdkconfig`、`.bin`、`.elf`、`.map` 等构建产物已被根目录 `.gitignore` 排除。
- 建议使用 `sdkconfig.defaults` 作为默认配置来源。
- 如果目标 BLE 名称不是 `GK36`，需要修改 `main/main.c` 里的 `TARGET_NAME`。

