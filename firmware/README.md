# Firmware đã build sẵn – v1.5.0 (ESP32-S3, flash 16 MB)

Build với ESP-IDF v6.0.2 + esp_matter 1.6.1 + IRremoteESP8266 v2.9.0, cấu hình mặc định
(`sdkconfig.defaults`), phiên bản 1.5.0.

| File | Dùng khi |
|---|---|
| `ac_matter_ir_v1.5.0_app_0x40000.bin` | **Nâng cấp** bo đang chạy v1.0.0 – v1.4.0: giữ Wi-Fi, mã ghép, hệ sinh thái, cảm biến, profile IR. Từ v1.5.0 trở đi tải thẳng file này lên **trang web** của thiết bị |
| `ac_matter_ir_v1.5.0_full_0x0.bin` | Nạp lần đầu cho bo trắng (bootloader + bảng phân vùng + otadata + app). **Xóa** Wi-Fi, mã ghép, fabric, cảm biến |
| `ac_matter_ir_v1.5.0_bootloader_0x0.bin` | Tùy chọn, một lần: thay bootloader cho bo được nạp lần đầu trước v1.5.0 để có tính năng *tự quay về firmware cũ khi bản cập nhật lỗi*. Không xóa dữ liệu |

## Nâng cấp từ v1.4.0 trở về trước (qua UART, lần cuối cần dây)

Thay `COM5` bằng cổng của bo. Một lệnh, nạp cả bootloader mới và app, dữ liệu giữ nguyên:

```bat
python -m esptool --chip esp32s3 -p COM5 -b 460800 write-flash 0x0 ac_matter_ir_v1.5.0_bootloader_0x0.bin 0x40000 ac_matter_ir_v1.5.0_app_0x40000.bin
```

Chỉ muốn thay app (giữ bootloader cũ, vẫn cập nhật qua web được nhưng không tự quay về khi lỗi):

```bat
python -m esptool --chip esp32s3 -p COM5 -b 460800 write-flash 0x40000 ac_matter_ir_v1.5.0_app_0x40000.bin
```

Nếu thiết bị **đã từng cập nhật qua web**, firmware đang chạy có thể nằm ở khe thứ hai (`ota_1`, 0x440000) và file
nạp vào 0x40000 sẽ không được dùng. Khi đó xóa vùng chọn khe trước khi nạp (thiết bị quay lại khởi động từ
0x40000):

```bat
python -m esptool --chip esp32s3 -p COM5 erase-region 0x30000 0x2000
```

## Cập nhật qua web (từ v1.5.0, không cần dây)

1. Nhấn BOOT 3 lần → kết nối Wi-Fi `AC-Matter-XXXX-v…` (mật khẩu `69696969` nếu chưa đổi) → `http://192.168.4.1`.
2. Tab **Chẩn đoán** → **Cập nhật firmware** → chọn file `ac_matter_ir_vX.Y.Z_app_0x40000.bin` → *Tải lên và cập nhật*.
3. Chờ khoảng nửa phút, không ngắt điện. Thiết bị kiểm tra file rồi khởi động lại; Wi-Fi cấu hình mở lại với
   **tên mới theo phiên bản** (`…-vX.Y.Z`). Mọi dữ liệu được giữ.
4. Firmware mới chạy thử 60 giây. Nếu thiết bị khởi động lại trong thời gian đó, bootloader v1.5.0 tự quay về
   bản cũ (bootloader cũ hơn: không có bước này).

File `full`, file bootloader, firmware của dự án khác hoặc file hỏng đều bị từ chối; firmware đang chạy không bị
đụng tới.

## Nạp lần đầu (bo trắng)

```bat
python -m esptool --chip esp32s3 -p COM5 -b 460800 write-flash 0x0 ac_matter_ir_v1.5.0_full_0x0.bin
```

File full được đệm `0xFF` qua vùng NVS (`0x10000`–`0x2FFFF`), nên nạp nó sẽ **xóa** Wi-Fi, mã ghép (tạo
mã mới), Fabric B và cảm biến đã ghép; profile IR ở phân vùng `irstore` (`0x840000`) vẫn còn.

SHA-256:
- `140cc2d28f45326ee89794bbc83ef8562f2f8af51dc4628537332c683c4657da`  ac_matter_ir_v1.5.0_full_0x0.bin
- `d705b9a360d00f28a8e9ec70f849851efca9d4d077a6aea860abe4e6612ac91f`  ac_matter_ir_v1.5.0_app_0x40000.bin
- `fd15d467765dda3410076f61c6f8f701306b80f9371498c66ffe81daa2fbdf12`  ac_matter_ir_v1.5.0_bootloader_0x0.bin

## Nút BOOT

| Thao tác | Kết quả | LED |
|---|---|---|
| Nhấn 2 lần | Chế độ ghép Matter 15 phút | nháy xanh dương |
| Nhấn 3 lần | Mở Wi-Fi cấu hình + trang web | xanh lá |
| Giữ 10 giây rồi **thả** | Rời khỏi **tất cả ứng dụng nhà** (giữ Wi-Fi, cảm biến, profile IR, cài đặt); chế độ ghép tự bật lại | nháy đỏ nhanh khi đủ 10 giây |
| Giữ 20 giây | **Đặt lại gốc**: xóa Wi-Fi, mọi hệ sinh thái, cảm biến, cài đặt; giữ mã ghép Matter và profile IR | đỏ sáng liên tục |

Hai thao tác cuối cũng có trên web: tab **Nhà thông minh** (gỡ từng hệ sinh thái hoặc tất cả) và tab **Chẩn đoán**
(*Đặt lại gốc*). Nên gỡ cảm biến (tab Cảm biến) trước khi đặt lại gốc. Giữ nút lúc cấp điện là chế độ nạp của chip,
không kích hoạt hai thao tác này.

## Sửa lỗi trong v1.5.0

- Thiết bị **không còn tự phát lệnh IR** (thường là *Tắt*) sau khi lưu/khóa/chọn profile hoặc sau khi nạp file full.
- Bật máy kèm đổi chế độ từ ứng dụng nhà không còn có thể bị đổi về chế độ của lần bật trước.
- Tự động hóa lặp lại cùng một giá trị không còn làm lùi chu kỳ bù theo cảm biến ngoài.
- Đổi Wi-Fi mà mất điện giữa lúc thử: lần khởi động sau tự quay về mạng cũ.
- Bộ thu IR: loại bỏ một tình huống tranh chấp hiếm có thể làm thiết bị khởi động lại.

## Đồng bộ khi bấm remote gốc (từ v1.4.0)

Profile đang dùng đã khóa (sau *Lưu và dùng profile này*, hoặc khóa profile học thủ công) thì mắt thu luôn
nghe remote gốc. Bấm remote (bật/tắt, chế độ, nhiệt độ, quạt, đảo gió): trong khoảng 1 giây ứng dụng nhà hiện
trạng thái mới, ESP **không** phát lại lệnh. Lệnh sau từ ứng dụng nối tiếp đúng trạng thái remote đã đặt.

- Kiểm tra: trang **Máy lạnh** → dòng **Remote gốc** (trạng thái cuối, hoặc vì sao lệnh cuối bị bỏ qua), hoặc
  lệnh `status` trên cổng serial (dòng `Remote  :`). Log khi bấm: `climate: remote: Lạnh 24°C, …`.
- Lệnh của remote khác (TV…) bị bỏ qua: `remote: ignored, giao thức NEC, profile dùng SHARP_AC`.
- Tắt tính năng: tab **Thuật toán** → bỏ chọn *Đồng bộ khi bấm remote gốc*.
- Chế độ Auto của remote không có trong thermostat Matter: chỉ bật/tắt, quạt, đảo gió được đồng bộ. Ở chế độ
  Khô, nhiệt độ đặt trong ứng dụng không đổi (Sharp luôn gửi nhiệt độ thấp nhất khi Khô).

## Nhận diện remote tự động (từ v1.3.0)

1. BOOT 3 lần → Wi-Fi cấu hình → `http://192.168.4.1` → tab **Máy lạnh** → **Bắt đầu nhận diện**.
2. Hướng remote máy lạnh vào mắt thu, bấm một nút. Trang hiện giao thức (ví dụ `DAIKIN (280 bit)`) và
   trạng thái đọc được: phải khớp màn hình remote. Chọn model nếu được hỏi.
3. Khai báo số mức quạt, các chế độ máy có, dải nhiệt độ; **Phát thử** vài lệnh và lệnh Tắt.
4. Máy lạnh phản hồi đúng → chọn ô trống, đặt tên → **Lưu và dùng profile này** → khởi động lại khi trang báo.

Không nhận ra, hoặc máy không phản hồi lệnh thử: dùng **Học lệnh IR** (học từng lệnh) như các bản trước.

## Tên thiết bị (có hậu tố phiên bản)

| Ở đâu | Tên |
|---|---|
| Ứng dụng nhà (Matter ProductName, NodeLabel, DNS-SD) | `May lanh IR v1.5.0` (hãng: `DIY`) |
| Bluetooth | `AC-XXXX-v1.5.0` |
| Wi-Fi cấu hình | `AC-Matter-XXXX-v1.5.0`, mật khẩu `69696969` |

`XXXX` = 4 ký tự cuối địa chỉ MAC, giống nhau ở Bluetooth và Wi-Fi cấu hình.

## Ghép vào ứng dụng nhà

1. Nhấn **BOOT 2 lần** → LED RGB (GPIO48) nháy xanh dương 15 phút, ESP phát Bluetooth.
2. Điện thoại: bật Bluetooth, ở Wi-Fi nhà **2,4 GHz** (không vào Wi-Fi cấu hình), đứng gần ESP.
3. Google Home / Apple Home / SmartThings / Alexa / Home Assistant → *Thêm thiết bị → Matter* → quét QR
   hoặc nhập mã 11 số. Ứng dụng gửi Wi-Fi nhà cho ESP qua Bluetooth (ESP chưa cần có Wi-Fi trước).

Mã QR / mã 11 số: trang "Nhà thông minh" của Wi-Fi cấu hình (BOOT 3 lần, `http://192.168.4.1`) hoặc lệnh
`secrets` trên cổng serial. Mã số không đổi so với v1.0.0; QR có thêm cờ Bluetooth từ v1.1.0.

Log khởi động phải có `device name: May lanh IR v1.5.0 (vendor DIY), firmware 1.5.0, BLE commissioning on`.

## Quạt và đảo gió trong ứng dụng nhà

Thiết bị là **Room Air Conditioner** (kèm loại Thermostat) có cluster **Fan Control**: mỗi mức quạt của profile
là một tốc độ quạt, có chế độ Tự động và đảo gió. Tạo và khóa profile **trước khi thêm vào ứng dụng nhà**.
Cách mỗi ứng dụng hiển thị quạt khác nhau; chưa thử trên thiết bị thật.

## Tạo lại các file này

Tên file luôn có hậu tố phiên bản (`v<PROJECT_VER>`). Sau `idf.py build`, trong ESP-IDF Terminal:

```bat
python tools/package_firmware.py
```

Script tạo ba file (full, app, bootloader) theo phiên bản của bản build, xóa file của phiên bản cũ và in các dòng SHA-256 để dán vào đây.
