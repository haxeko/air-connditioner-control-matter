# Firmware đã build sẵn (ESP32-S3, flash 16 MB)

Build từ commit `77c8d8c` với ESP-IDF v6.0.2 + esp_matter 1.6.1, cấu hình mặc định (`sdkconfig.defaults`).

| File | Địa chỉ | Dùng khi |
|---|---|---|
| `ac_matter_ir_full_0x0.bin` | `0x0` | Nạp lần đầu (bootloader + bảng phân vùng + otadata + app) |
| `ac_matter_ir_app_0x40000.bin` | `0x40000` | Cập nhật app, giữ nguyên cấu hình/fabric/profile IR |

```bat
python -m esptool --chip esp32s3 -p COM5 -b 460800 write-flash 0x0 ac_matter_ir_full_0x0.bin
```

SHA-256:
- `5cee698d4d9555f0fb40301a2f4aa576396538c6b0800cb97c33c890b604af82`  ac_matter_ir_full_0x0.bin
- `06bf37edd3b8a3f43da12aa74d59692e5935b874792e9c1b1cccb71b16d66b94`  ac_matter_ir_app_0x40000.bin
