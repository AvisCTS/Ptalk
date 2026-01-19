
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