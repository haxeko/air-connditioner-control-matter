# Firmware đã build sẵn (ESP32-S3, flash 16 MB)

Build với ESP-IDF v6.0.2 + esp_matter 1.6.1, cấu hình mặc định (`sdkconfig.defaults`).

| File | Địa chỉ | Dùng khi |
|---|---|---|
| `ac_matter_ir_full_0x0.bin` | `0x0` | Nạp lần đầu (bootloader + bảng phân vùng + otadata + app) |
| `ac_matter_ir_app_0x40000.bin` | `0x40000` | Cập nhật app, giữ nguyên cấu hình/fabric/profile IR |

```bat
python -m esptool --chip esp32s3 -p COM5 -b 460800 write-flash 0x0 ac_matter_ir_full_0x0.bin
```

SHA-256:
- `2ad198613a34aed1a70130a0daf92a70f1151fc02323c38c20c838a102f060fc`  ac_matter_ir_full_0x0.bin
- `40016dda50162f1cc6f7fcfc0bf02e0da4a30dd1097284f2152a01caf12e1512`  ac_matter_ir_app_0x40000.bin

Wi-Fi cấu hình: `AC-Matter-XXXX`, mật khẩu `69696969`.

Nút BOOT: 2 lần = chế độ ghép Matter (LED RGB GPIO48 nháy xanh dương), 3 lần = Wi-Fi cấu hình
(`http://192.168.4.1`). Mã QR / mã 11 số: trang "Nhà thông minh" hoặc lệnh `secrets` trên cổng serial.
