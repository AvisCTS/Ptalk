# PTalk MQTT Communication Specification

Tài liệu này đặc tả cấu trúc bản tin và quy trình giao tiếp giữa **PTalk Server** và **PTalk Device** (ESP32) thông qua giao thức MQTT.

## 1. Cấu hình hệ thống (System Configuration)

Để đảm bảo khả năng truyền tải các bản tin JSON lớn và các khối dữ liệu Firmware (OTA), thiết bị ESP32 được cấu hình như sau:

*   **MQTT Buffer Size:** `4096 Bytes` (Thiết lập qua `cfg.buffer_size` trong `esp_mqtt_client_config_t`).
*   **Keep Alive:** `60 giây`.
*   **QoS (Quality of Service):** 
    *   Lệnh điều khiển (`/cmd`): `QoS 1`.
    *   Dữ liệu OTA (`/ota_data`): `QoS 1`.
    *   Bản tin trạng thái (`/status`): `QoS 1` với cờ `Retain`.

---

## 2. Cấu trúc Topic

Tất cả các Topic đều bắt đầu bằng tiền tố `devices/` theo sau là **Device MAC ID** (12 ký tự viết hoa).

| Topic | Hướng đi | Mô tả |
| :--- | :--- | :--- |
| `devices/{MAC}/cmd` | Server → Device | Gửi lệnh cấu hình (JSON) |
| `devices/{MAC}/status` | Device → Server | Báo cáo trạng thái (JSON) |
| `devices/{MAC}/ota_data` | Server → Device | Gửi khối dữ liệu Firmware (Binary) |
| `devices/{MAC}/ota_ack` | Device → Server | Phản hồi xác nhận nhận khối OTA (JSON) |

---

## 3. Đặc tả bản tin JSON

### 3.1 Lệnh điều khiển (Topic: `/cmd`)
Bản tin gửi xuống thiết bị phải là định dạng JSON hợp lệ. Trường `cmd` bắt buộc phải là **chữ thường (lowercase)**.

| Lệnh (`cmd`) | Tham số | Mô tả |
| :--- | :--- | :--- |
| `request_status` | Không | Yêu cầu thiết bị gửi lại bản tin Status ngay lập tức. |
| `set_volume` | `{"volume": 0-100}` | Điều chỉnh âm lượng loa (0-100%). |
| `set_brightness` | `{"brightness": 0-100}` | Điều chỉnh độ sáng màn hình (0-100%). |
| `set_device_name` | `{"device_name": "string"}` | Đặt tên gợi nhớ cho thiết bị. |
| `reboot` | Không | Ra lệnh khởi động lại thiết bị ngay lập tức. |
| `request_ble_config`| Không | Chuyển thiết bị sang chế độ cấu hình qua Bluetooth. |
| `request_ota` | `{"size": uint32, "sha256": "string", "chunk_size": int, "total_chunks": int}` | Khởi tạo quy trình cập nhật Firmware. |

### 3.2 Báo cáo trạng thái (Topic: `/status`)
Thiết bị phản hồi trạng thái định kỳ hoặc sau khi thực hiện lệnh.

**Cấu trúc mẫu:**
```json
{
  "status": "ok",
  "device_id": "D4E9F4C13B1C",
  "device_name": "PTalk-V1",
  "battery_percent": 85,
  "volume": 60,
  "brightness": 100,
  "connectivity_state": "ONLINE",
  "firmware_version": "1.0.5",
  "uptime_sec": 3600
}
```

---

## 4. Giao thức OTA (Over-The-Air)

Quy trình OTA sử dụng cơ chế truyền tải theo từng khối (Chunk-based) để đảm bảo tính toàn vẹn trên đường truyền MQTT.

### 4.1 Cấu trúc khối dữ liệu nhị phân (Topic: `/ota_data`)
Mỗi gói tin nhị phân gửi xuống bao gồm **Header cố định 12 bytes** và phần **Payload dữ liệu**.

**Binary Header (Little Endian):**
1.  **Sequence (4 bytes):** Số thứ tự khối (0, 1, 2...).
2.  **Chunk Size (4 bytes):** Độ dài của phần dữ liệu thực tế trong gói này.
3.  **CRC32 (4 bytes):** Mã CRC32 tính toán từ phần Payload dữ liệu.

**Độ dài tối đa:** 
Với cấu hình `buffer_size = 4096`, kích thước khối dữ liệu (Payload) tối ưu là **2048 bytes**.

### 4.2 Cơ chế xác nhận (Topic: `/ota_ack`)
Thiết bị sẽ kiểm tra từng khối và phản hồi:
*   **Thành công (ACK):** `{"ota_ack": sequence_number}`
*   **Thất bại (NACK):** `{"ota_nack": sequence_number, "expected_seq": sequence_number}`

---

## 5. Quy trình xử lý (Logic Flow)

1.  **Khởi tạo:** Server gửi `request_ota`.
2.  **Chuẩn bị:** ESP32 dừng các tác vụ Audio, giải phóng RAM, thực hiện Erase Flash và gửi status `"message": "Ready to receive firmware"`.
3.  **Truyền tải:** Server gửi khối `N`, đợi nhận `ota_ack` từ thiết bị rồi mới gửi khối `N+1`.
4.  **Kết thúc:** Sau khối cuối cùng, ESP32 kiểm tra SHA256 tổng thể, gửi thông báo hoàn tất và tự động `reboot`.

---

## 6. Lưu ý về giới hạn độ dài
- **Maximum JSON Length:** 1024 bytes (để đảm bảo hiệu suất parse JSON).
- **Maximum Binary Length:** 4000 bytes (giới hạn bởi buffer 4096 trừ đi MQTT Header và Topic).
- **Timeout:** Khối đầu tiên (Chunk 0) yêu cầu timeout phía Server tối thiểu **20 giây** do thiết bị bận Erase Flash.

### Ví dụ 1: Thay đổi âm lượng loa (Control Flow)
Giả sử bạn muốn chỉnh âm lượng của thiết bị có MAC là `D4E9F4C13B1C` lên 80%.

1.  **Server gửi (Publish)** tới topic `devices/D4E9F4C13B1C/cmd`:
    ```json
    {
      "cmd": "set_volume",
      "volume": 80
    }
    ```
2.  **Thiết bị phản hồi (Publish)** tới topic `devices/D4E9F4C13B1C/status`:
    ```json
    {
      "status": "ok",
      "device_id": "D4E9F4C13B1C",
      "volume": 80,
      "message": "Volume updated"
    }
    ```

---

### Ví dụ 2: Kiểm tra trạng thái thiết bị (Query Flow)
Khi bạn muốn biết Pin và thời gian hoạt động của thiết bị.

1.  **Server gửi** tới topic `devices/D4E9F4C13B1C/cmd`:
    ```json
    {
      "cmd": "request_status"
    }
    ```
2.  **Thiết bị phản hồi** tới topic `devices/D4E9F4C13B1C/status`:
    ```json
    {
      "status": "ok",
      "device_id": "D4E9F4C13B1C",
      "battery_percent": 92,
      "uptime_sec": 3600,
      "firmware_version": "1.0.5",
      "connectivity_state": "ONLINE"
    }
    ```

---

### Ví dụ 3: Khởi tạo cập nhật Firmware (OTA Handshake)
Đây là bước "chào hỏi" trước khi gửi file Bin.

1.  **Server gửi** tới topic `devices/D4E9F4C13B1C/cmd`:
    ```json
    {
      "cmd": "request_ota",
      "size": 1992048,
      "sha256": "5f3456789abcdef0...",
      "chunk_size": 2048,
      "total_chunks": 973
    }
    ```
2.  **Thiết bị phản hồi** (sau khi Erase Flash xong) tới topic `devices/D4E9F4C13B1C/status`:
    ```json
    {
      "status": "ok",
      "message": "Ready to receive firmware",
      "device_id": "D4E9F4C13B1C"
    }
    ```

---

### Ví dụ 4: Cấu trúc của 1 gói tin dữ liệu OTA (Binary Packet)
Đây là cách Server gửi khối dữ liệu (Chunk) số 0 xuống cho thiết bị. Gói tin này không phải JSON mà là dữ liệu thô (Raw Bytes).

**Gói tin gửi tới topic `devices/D4E9F4C13B1C/ota_data`:**

| Phần | Kích thước | Giá trị ví dụ (Hex/Bytes) | Giải thích |
| :--- | :--- | :--- | :--- |
| **Header: Seq** | 4 Bytes | `00 00 00 00` | Khối số 0 |
| **Header: Size** | 4 Bytes | `00 08 00 00` | 2048 bytes dữ liệu phía sau |
| **Header: CRC32** | 4 Bytes | `A1 B2 C3 D4` | Mã checksum của 2048 bytes dữ liệu |
| **Payload** | 2048 Bytes | `E9 03 4F ...` | Dữ liệu thực tế lấy từ file `.bin` |

**Tổng dung lượng gói tin này:** $12 + 2048 = 2060$ bytes (Nằm trong giới hạn buffer 4096).

---

### Ví dụ 5: Xác nhận sau khi nhận khối dữ liệu (OTA Feedback)
Sau khi nhận được gói tin ở Ví dụ 4, thiết bị kiểm tra CRC32 thấy đúng.

1.  **Thiết bị gửi xác nhận** tới topic `devices/D4E9F4C13B1C/ota_ack`:
    ```json
    {
      "ota_ack": 0
    }
    ```
2.  **Server** nhận được tin nhắn trên mới tiếp tục gửi Khối số 1 (`ota_data` với Seq = 1).

---

### Ví dụ 6: Xử lý lỗi (Error Handling)
Nếu bạn gửi sai tên lệnh hoặc tham số vượt quá giới hạn.

1.  **Server gửi**: `{"cmd": "set_volume", "volume": 150}`
2.  **Thiết bị phản hồi**:
    ```json
    {
      "status": "error",
      "message": "invalid_param",
      "details": "Volume must be 0-100"
    }
    ```

---

### Tóm tắt luồng hoạt động (Visual Flow)

```text
Server (Python)                          Device (ESP32)
   |                                          |
   |---- [cmd] {"cmd":"reboot"} ------------->|
   |                                          | (Xử lý lệnh...)
   |<--- [status] {"status":"ok"} ------------|
   |                                          | (Khởi động lại)
   |                                          |
   |---- [cmd] {"cmd":"request_ota",...} ---->|
   |                                          | (Xoá Flash - mất 5-10s)
   |<--- [status] {"message":"Ready..."} -----|
   |                                          |
   |---- [ota_data] (Header + 2048 bytes) --->|
   |                                          | (Kiểm tra CRC32 + Ghi Flash)
   |<--- [ota_ack] {"ota_ack": 0} ------------|
   |                                          |
   |---- [ota_data] (Tiếp tục khối 1...) ---->|
```