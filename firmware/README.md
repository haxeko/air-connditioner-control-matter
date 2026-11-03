# Firmware đã build sẵn – v1.2.0 (ESP32-S3, flash 16 MB)

Build với ESP-IDF v6.0.2 + esp_matter 1.6.1, cấu hình mặc định (`sdkconfig.defaults`), phiên bản 1.2.0.

| File | Địa chỉ | Dùng khi |
|---|---|---|
| `ac_matter_ir_v1.2.0_full_0x0.bin` | `0x0` | Nạp lần đầu (bootloader + bảng phân vùng + otadata + app) |
| `ac_matter_ir_v1.2.0_app_0x40000.bin` | `0x40000` | **Nâng cấp từ v1.0.0 hoặc v1.1.0**: chỉ thay app, giữ Wi-Fi, mã ghép, fabric, cảm biến, profile IR |

Nạp lần đầu (thay `COM5` bằng cổng của bo):

```bat
python -m esptool --chip esp32s3 -p COM5 -b 460800 write-flash 0x0 ac_matter_ir_v1.2.0_full_0x0.bin
```

Nâng cấp bo đang chạy v1.0.0 hoặc v1.1.0, giữ nguyên dữ liệu:

```bat
python -m esptool --chip esp32s3 -p COM5 -b 460800 write-flash 0x40000 ac_matter_ir_v1.2.0_app_0x40000.bin
```

File full được đệm `0xFF` qua vùng NVS (`0x10000`–`0x2FFFF`), nên nạp nó sẽ **xóa** Wi-Fi, mã ghép (tạo
mã mới), Fabric B và cảm biến đã ghép; profile IR ở phân vùng `irstore` (`0x840000`) vẫn còn.

SHA-256:
- `94921ffab94a132cb346cc5e6873d1f027b5b2af4e498c24836476ee34b9bbb9`  ac_matter_ir_v1.2.0_full_0x0.bin
- `dfa1bfd53791a69d8c0895c70634face66dfcbf63088528a0f8a7f2c02993deb`  ac_matter_ir_v1.2.0_app_0x40000.bin

## Tên thiết bị (có hậu tố phiên bản)

| Ở đâu | Tên |
|---|---|
| Ứng dụng nhà (Matter ProductName, NodeLabel, DNS-SD) | `May lanh IR v1.2.0` (hãng: `DIY`) |
| Bluetooth | `AC-XXXX-v1.2.0` |
| Wi-Fi cấu hình | `AC-Matter-XXXX-v1.2.0`, mật khẩu `69696969` |

`XXXX` = 4 ký tự cuối địa chỉ MAC, giống nhau ở Bluetooth và Wi-Fi cấu hình.

## Ghép vào ứng dụng nhà

1. Nhấn **BOOT 2 lần** → LED RGB (GPIO48) nháy xanh dương 15 phút, ESP phát Bluetooth.
2. Điện thoại: bật Bluetooth, ở Wi-Fi nhà **2,4 GHz** (không vào Wi-Fi cấu hình), đứng gần ESP.
3. Google Home / Apple Home / SmartThings / Alexa / Home Assistant → *Thêm thiết bị → Matter* → quét QR
   hoặc nhập mã 11 số. Ứng dụng gửi Wi-Fi nhà cho ESP qua Bluetooth (ESP chưa cần có Wi-Fi trước).

Mã QR / mã 11 số: trang "Nhà thông minh" của Wi-Fi cấu hình (BOOT 3 lần, `http://192.168.4.1`) hoặc lệnh
`secrets` trên cổng serial. Mã số không đổi so với v1.0.0; QR có thêm cờ Bluetooth nên nên in lại tem.

Log khởi động phải có `device name: May lanh IR v1.2.0 (vendor DIY), firmware 1.2.0, BLE commissioning on`.

## Quạt và đảo gió trong ứng dụng nhà (v1.2.0)

Thiết bị là **Room Air Conditioner** (kèm loại Thermostat) có cluster **Fan Control**: mỗi mức quạt đã học
là một tốc độ quạt, có chế độ Tự động và đảo gió (nếu đã học đảo gió bật). Học các mức quạt và **khóa
profile trước khi thêm vào ứng dụng nhà**. Cách mỗi ứng dụng hiển thị quạt khác nhau; chưa thử trên thiết bị thật.

## Tạo lại các file này

Tên file luôn có hậu tố phiên bản (`v<PROJECT_VER>`). Sau `idf.py build`, trong ESP-IDF Terminal:

```bat
python tools/package_firmware.py
```

Script tạo hai file theo phiên bản của bản build, xóa file của phiên bản cũ và in các dòng SHA-256 để dán vào đây.
