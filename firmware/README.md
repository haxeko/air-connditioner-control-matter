# Firmware đã build sẵn – v1.7.0 (ESP32-S3, flash 16 MB)

Build với ESP-IDF v6.0.2 + esp_matter 1.6.1 + IRremoteESP8266 v2.9.0, cấu hình mặc định
(`sdkconfig.defaults`), phiên bản 1.7.0.

| File | Dùng khi |
|---|---|
| `ac_matter_ir_v1.7.0_app_0x40000.bin` | **Nâng cấp**: giữ Wi-Fi, mã ghép, hệ sinh thái, cảm biến, profile IR. Bo đang chạy v1.5.0 trở lên: tải thẳng file này lên **trang web** của thiết bị. Bo v1.0.0 – v1.4.0: nạp qua UART (mục dưới) |
| `ac_matter_ir_v1.7.0_full_0x0.bin` | Nạp lần đầu cho bo trắng (bootloader + bảng phân vùng + otadata + app). **Xóa** Wi-Fi, mã ghép, fabric, cảm biến |
| `ac_matter_ir_v1.7.0_bootloader_0x0.bin` | Tùy chọn, một lần: thay bootloader cho bo được nạp lần đầu trước v1.5.0 để có tính năng *tự quay về firmware cũ khi bản cập nhật lỗi*. Không xóa dữ liệu |
| `ac_matter_ir_v1.7.0_partitions_0xC000.bin` | Tùy chọn, một lần: bảng phân vùng có thêm `coredump` cho bo được nạp lần đầu trước v1.6.0, để thiết bị **lưu báo cáo sự cố**. Không xóa dữ liệu |
| `ac_matter_ir_v1.7.0_symbols.txt.gz` | Không nạp vào bo: bảng địa chỉ hàm của bản build này, dùng với `tools/decode_backtrace.py` |

## Có gì mới trong v1.7.0 – đồng bộ với Apple Home

- Ô máy lạnh trong Apple Home hiện **đang làm lạnh** khi máy chạy (thiết bị báo thêm trạng thái chạy). Trước đây ô
  có thể hiện Tắt dù máy đang chạy, cả khi bật bằng remote gốc lẫn ngay sau khi bật từ Apple Home.
- Quạt tự động không còn báo tốc độ 0 % (Apple Home hiểu là tắt); quạt **không còn bật/tắt máy**: kéo tốc độ quạt
  về 0 % hay quạt Off nghĩa là quạt tự động, chọn tốc độ khi máy tắt được dùng ở lần bật sau.
- Trang **Máy lạnh** có dòng **Ứng dụng nhà đang thấy**: thiết bị đang báo gì qua Matter và ai đổi lần cuối.

Không đổi: cách ghép, mã QR/mã số, profile IR, cài đặt. Không cần gỡ/ghép lại thiết bị trong Apple Home.

### Kiểm tra đồng bộ remote → Apple Home

1. BOOT 3 lần → Wi-Fi cấu hình → `http://192.168.4.1` → tab **Máy lạnh**, khung *Điều khiển*.
2. Bấm remote gốc (ví dụ bật, Lạnh 24 °C). Trong 1–2 giây:
   - dòng **Remote gốc** hiện trạng thái vừa bấm → thiết bị đã nhận và hiểu lệnh;
   - dòng **Ứng dụng nhà đang thấy** hiện `Bật · Làm lạnh 24 °C · đang làm lạnh · … (đổi bởi remote gốc, …)` →
     thiết bị đã báo cho Apple Home.
3. Đối chiếu với Apple Home. Nếu hai dòng trên đổi mà Apple Home vẫn không đổi, chụp màn hình trang này và ô máy
   lạnh trong Apple Home gửi lại (kèm log cổng serial nếu có).

Nếu dòng **Remote gốc** ghi *đang nghe, chưa nhận lệnh nào*: lệnh không tới được thiết bị (hướng remote, khoảng
cách, mắt thu). Nếu ghi *bỏ qua lệnh cuối: …*: lệnh nhận được nhưng không khớp profile (lý do ghi ngay đó). Nếu ghi
*chưa theo dõi*: profile chưa khóa hoặc tắt tùy chọn *Đồng bộ khi bấm remote gốc* ở tab Thuật toán.

### Chế độ trong Apple Home

Apple Home chỉ có **Tắt / Sưởi / Làm lạnh / Tự động** cho máy lạnh và chỉ hiện chế độ thiết bị khai báo được: máy
chỉ làm lạnh → **Tắt và Làm lạnh**. **Khô** và **Quạt** không có trong HomeKit nên Apple Home không hiển thị được;
**Tự động** của Matter đòi máy có Sưởi, và với loại máy lạnh (Room Air Conditioner) Apple Home hiện gửi Làm lạnh khi
chọn Tự động (lỗi đã được kỹ sư Apple xác nhận). Ba chế độ này vẫn dùng bằng remote gốc, trang web của thiết bị, và
ứng dụng hỗ trợ (Home Assistant, SmartThings…).

## Có gì mới trong v1.6.0

- Treo thì **tự khởi động lại** (watchdog tác vụ 15 giây); trước đây thiết bị treo cho tới khi rút điện.
- **Báo cáo sự cố**: tab Chẩn đoán → *Sự cố khởi động lại* cho biết tác vụ và vị trí gây lỗi của lần khởi động lại
  bất thường gần nhất (xem mục *Khi thiết bị tự khởi động lại* bên dưới).
- Trang Chẩn đoán có thêm **ngăn xếp của từng tác vụ**; lý do khởi động phân biệt watchdog tác vụ / watchdog ngắt
  / sụt áp nguồn. Lệnh console mới: `diag`.
- Ứng dụng nhà bấm *nhận diện thiết bị* (Identify): LED nháy **trắng**.
- Sửa lỗi: phát IR quá hạn không còn đụng vùng nhớ đã giải phóng; lệnh lặp lại ngay sau khi bấm remote gốc không
  còn bị bỏ; khởi động lại từ web/sau cập nhật không làm mất lệnh đang chờ và trạng thái 10 giây cuối.
- Không còn mã ghép thử nghiệm của SDK trong firmware; GPIO cấu hình sai bị từ chối lúc build.

Không đổi: cách ghép, mã QR/mã số, profile IR, cài đặt, giao diện các trang khác.

## Nâng cấp từ v1.5.0 / v1.6.0 (không cần dây)

Tải `ac_matter_ir_v1.7.0_app_0x40000.bin` lên trang web như mục *Cập nhật qua web* bên dưới. Bo nạp lần đầu trước
v1.6.0 chạy đủ mọi tính năng, riêng **báo cáo sự cố không được lưu** (bảng phân vùng cũ chưa có `coredump`); log
khởi động có một dòng `No core dump partition found!`, bỏ qua được. Muốn có báo cáo sự cố: nạp thêm bảng phân vùng
qua UART một lần (không mất dữ liệu):

```bat
python -m esptool --chip esp32s3 -p COM5 -b 460800 write-flash 0xC000 ac_matter_ir_v1.7.0_partitions_0xC000.bin
```

## Nâng cấp từ v1.4.0 trở về trước (qua UART, lần cuối cần dây)

Thay `COM5` bằng cổng của bo. Một lệnh, nạp bootloader mới, bảng phân vùng mới và app, dữ liệu giữ nguyên:

```bat
python -m esptool --chip esp32s3 -p COM5 -b 460800 write-flash 0x0 ac_matter_ir_v1.7.0_bootloader_0x0.bin 0xC000 ac_matter_ir_v1.7.0_partitions_0xC000.bin 0x40000 ac_matter_ir_v1.7.0_app_0x40000.bin
```

Chỉ muốn thay app (giữ bootloader và bảng phân vùng cũ: vẫn cập nhật qua web được, nhưng không tự quay về khi
lỗi và không lưu báo cáo sự cố):

```bat
python -m esptool --chip esp32s3 -p COM5 -b 460800 write-flash 0x40000 ac_matter_ir_v1.7.0_app_0x40000.bin
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
python -m esptool --chip esp32s3 -p COM5 -b 460800 write-flash 0x0 ac_matter_ir_v1.7.0_full_0x0.bin
```

File full được đệm `0xFF` qua vùng NVS (`0x10000`–`0x2FFFF`), nên nạp nó sẽ **xóa** Wi-Fi, mã ghép (tạo
mã mới), Fabric B và cảm biến đã ghép; profile IR ở phân vùng `irstore` (`0x840000`) vẫn còn.

SHA-256:
- `36d15bf967426aee56553b15fa5144ac66a70f41f00c6cc963b9aa30752fa596`  ac_matter_ir_v1.7.0_full_0x0.bin
- `0dc823012eef85416936009b60ecc0f08b73ab28f0ea30e5e5db5328586ff522`  ac_matter_ir_v1.7.0_app_0x40000.bin
- `dd2bf5aa392a48ce12d2900462cbfaa9c43a18eb504ef12446f523259f9a8b22`  ac_matter_ir_v1.7.0_bootloader_0x0.bin
- `f75816b7bf236a29f5043f316f4aa8cc736c658f091a4badf48896b4d0413259`  ac_matter_ir_v1.7.0_partitions_0xC000.bin

SHA-256 của file ELF của bản build này bắt đầu bằng `a326127fb` (dòng *Firmware lúc lỗi* trong báo cáo sự cố).

## Khi thiết bị tự khởi động lại (từ v1.6.0)

BOOT 3 lần → `http://192.168.4.1` → tab **Chẩn đoán**:

- **Lý do khởi động**: *Cấp nguồn* / *Khởi động lại bằng phần mềm* là bình thường. *Sụt áp nguồn (brownout)*: nguồn
  5 V hoặc dây cấp yếu (đổi cốc sạc/dây, xem tụ trên bo). *Watchdog* hoặc *Lỗi nghiêm trọng (panic)*: lỗi firmware.
- **Sự cố khởi động lại**: tác vụ gây lỗi, vị trí (PC), nguyên nhân và dòng *Backtrace*. Chép nguyên các dòng này
  (chỉ là địa chỉ trong chương trình, không chứa mật khẩu hay mã ghép) gửi cho người hỗ trợ, rồi bấm *Xóa báo cáo*.
- **Ngăn xếp của các tác vụ**: dòng nào dưới 512 byte là tác vụ sát giới hạn bộ nhớ.

Tự giải mã (trên máy có thư mục dự án, chỉ cần Python):

```bat
python tools/decode_backtrace.py --elf-sha <ELF trong báo cáo> 0x4201a2c4 0x4201a2c1 0x4201b7f0
```

Lệnh in tên hàm của từng địa chỉ và cảnh báo nếu báo cáo thuộc bản firmware khác với file
`ac_matter_ir_v1.7.0_symbols.txt.gz`. Trên cổng serial: lệnh `diag` in cùng thông tin, `diag clear` xóa báo cáo.

## Nút BOOT

| Thao tác | Kết quả | LED |
|---|---|---|
| Nhấn 2 lần | Chế độ ghép Matter 15 phút | nháy xanh dương |
| Nhấn 3 lần | Mở Wi-Fi cấu hình + trang web | xanh lá |
| (ứng dụng nhà bấm *nhận diện*) | – | nháy trắng |
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
| Ứng dụng nhà (Matter ProductName, NodeLabel, DNS-SD) | `May lanh IR v1.7.0` (hãng: `DIY`) |
| Bluetooth | `AC-XXXX-v1.7.0` |
| Wi-Fi cấu hình | `AC-Matter-XXXX-v1.7.0`, mật khẩu `69696969` |

`XXXX` = 4 ký tự cuối địa chỉ MAC, giống nhau ở Bluetooth và Wi-Fi cấu hình.

## Ghép vào ứng dụng nhà

1. Nhấn **BOOT 2 lần** → LED RGB (GPIO48) nháy xanh dương 15 phút, ESP phát Bluetooth.
2. Điện thoại: bật Bluetooth, ở Wi-Fi nhà **2,4 GHz** (không vào Wi-Fi cấu hình), đứng gần ESP.
3. Google Home / Apple Home / SmartThings / Alexa / Home Assistant → *Thêm thiết bị → Matter* → quét QR
   hoặc nhập mã 11 số. Ứng dụng gửi Wi-Fi nhà cho ESP qua Bluetooth (ESP chưa cần có Wi-Fi trước).

Mã QR / mã 11 số: trang "Nhà thông minh" của Wi-Fi cấu hình (BOOT 3 lần, `http://192.168.4.1`) hoặc lệnh
`secrets` trên cổng serial. Mã số không đổi so với v1.0.0; QR có thêm cờ Bluetooth từ v1.1.0.

Log khởi động phải có `device name: May lanh IR v1.7.0 (vendor DIY), firmware 1.7.0, BLE commissioning on`.

## Quạt và đảo gió trong ứng dụng nhà

Thiết bị là **Room Air Conditioner** (kèm loại Thermostat) có cluster **Fan Control**: mỗi mức quạt của profile
là một tốc độ quạt, có chế độ Tự động và đảo gió. Tạo và khóa profile **trước khi thêm vào ứng dụng nhà**.
Cách mỗi ứng dụng hiển thị quạt khác nhau; chưa thử trên thiết bị thật.

## Tạo lại các file này

Tên file luôn có hậu tố phiên bản (`v<PROJECT_VER>`). Sau `idf.py build`, trong ESP-IDF Terminal:

```bat
python tools/package_firmware.py
```

Script tạo năm file (full, app, bootloader, bảng phân vùng, symbols) theo phiên bản của bản build, xóa file của phiên bản cũ và in các dòng SHA-256 để dán vào đây.
