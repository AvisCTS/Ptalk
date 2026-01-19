import json
import time
import paho.mqtt.client as mqtt
import struct
import zlib
import os
import socket

# === CẤU HÌNH ===
MQTT_BROKER = "127.0.0.1" 
MQTT_PORT = 1883
CHUNK_SIZE = 400  # Nên để 400 nếu chưa tăng buffer trên ESP32

discovered_devices = {} 
selected_mac = None

# Trạng thái luồng OTA
ota_context = {
    "is_running": False,
    "waiting_for_ack": -1,
    "is_ready": False,
    "error": False
}

def get_local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except: return "127.0.0.1"

local_ip = get_local_ip()

# --- CALLBACKS MQTT ---
def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print(f"\n✅ ĐÃ KẾT NỐI BROKER ({MQTT_BROKER})")
        client.subscribe("devices/+/status")
        client.subscribe("devices/+/ota_ack")
    else: print(f"❌ Lỗi kết nối: {rc}")

def on_message(client, userdata, msg):
    global selected_mac
    topic_parts = msg.topic.split('/')
    mac = topic_parts[1]
    
    try:
        payload = json.loads(msg.payload.decode())
        
        # 1. Xử lý tin nhắn Status (Heartbeat hoặc Phản hồi lệnh)
        if topic_parts[2] == "status":
            if mac not in discovered_devices:
                print(f"\n✨ Thiết bị online: {mac}")
                if selected_mac is None: selected_mac = mac
            discovered_devices[mac] = payload
            
            # Kiểm tra nếu ESP32 báo sẵn sàng nhận OTA
            if payload.get("message") == "Ready to receive firmware":
                ota_context["is_ready"] = True
            elif payload.get("status") == "error":
                print(f"\n❌ ESP32 báo lỗi: {payload.get('message')}")
                ota_context["error"] = True

        # 2. Xử lý ACK từ luồng OTA
        elif topic_parts[2] == "ota_ack":
            if "ota_ack" in payload:
                ack_seq = payload["ota_ack"]
                if ack_seq == ota_context["waiting_for_ack"]:
                    ota_context["waiting_for_ack"] = -1
            elif "ota_nack" in payload:
                print(f"\n⚠️ Nhận NACK cho khối {payload['ota_nack']}")
                ota_context["error"] = True

    except: pass

# --- KHỞI TẠO CLIENT ---
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message

def send_cmd(cmd_name, params=None):
    if not selected_mac: return
    data = {"cmd": cmd_name}
    if params: data.update(params)
    topic = f"devices/{selected_mac}/cmd"
    client.publish(topic, json.dumps(data), qos=1)

# --- HÀM THỰC HIỆN OTA ---
def run_ota_flow():
    if not selected_mac:
        print("❌ Chưa chọn thiết bị để update!")
        return

    fw_path = input("📂 Nhập tên file firmware (vd: 1.0.5.bin): ")
    if not os.path.exists(fw_path):
        print(f"❌ Không tìm thấy file {fw_path}")
        return

    # Đọc file và tính toán
    file_size = os.path.getsize(fw_path)
    with open(fw_path, "rb") as f:
        fw_data = f.read()
    
    total_chunks = (file_size + CHUNK_SIZE - 1) // CHUNK_SIZE
    
    # Reset context
    ota_context["is_ready"] = False
    ota_context["error"] = False
    ota_context["waiting_for_ack"] = -1
    
    print(f"📦 Bắt đầu OTA cho {selected_mac}...")
    print(f"   Size: {file_size} bytes | Chunks: {total_chunks}")

    # Gửi lệnh mồi request_ota
    send_cmd("request_ota", {
        "size": file_size,
        "sha256": "dummy_hash",
        "chunk_size": CHUNK_SIZE,
        "total_chunks": total_chunks
    })

    # Đợi phản hồi Ready
    print("⏳ Đợi ESP32 chuẩn bị Flash...")
    timeout = 15
    start_wait = time.time()
    while not ota_context["is_ready"]:
        if time.time() - start_wait > timeout or ota_context["error"]:
            print("❌ ESP32 không phản hồi Ready hoặc gặp lỗi.")
            return
        time.sleep(0.1)

    # Gửi từng khối
    data_topic = f"devices/{selected_mac}/ota_data"
    for seq in range(total_chunks):
        if ota_context["error"]: break

        start_idx = seq * CHUNK_SIZE
        end_idx = min(start_idx + CHUNK_SIZE, file_size)
        chunk = fw_data[start_idx:end_idx]
        
        # Đóng gói Header: [Seq 4B][Size 4B][CRC 4B]
        crc = zlib.crc32(chunk) & 0xFFFFFFFF
        header = struct.pack('<III', seq, len(chunk), crc)
        
        ota_context["waiting_for_ack"] = seq
        client.publish(data_topic, header + chunk, qos=1)

        # Chờ ACK
        ack_start = time.time()
        while ota_context["waiting_for_ack"] != -1:
            if time.time() - ack_start > 10: # 10s timeout mỗi khối
                print(f"\n❌ Mất ACK khối {seq}")
                return
            time.sleep(0.01)

        if seq % 5 == 0 or seq == total_chunks - 1:
            print(f"📤 Progress: {(seq+1)/total_chunks*100:.1f}% ({seq+1}/{total_chunks})", end='\r')

    if not ota_context["error"]:
        print("\n✅ OTA Thành công! Thiết bị sẽ Reboot.")

# --- MAIN LOOP ---
try:
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    client.loop_start()

    while True:
        if not selected_mac:
            print("⏳ Đang quét thiết bị...", end='\r')
            time.sleep(1); continue

        print(f"\n--- [ PTALK MENU: {selected_mac} ] ---")
        print("1. Xem Status      | 2. Chỉnh Volume   | 3. Chỉnh Brightness")
        print("4. Đổi tên thiết bị | 5. Reboot         | 6. Mở BLE Config")
        print("7. UPDATE FIRMWARE (OTA)")
        print("0. Thoát")
        
        choice = input("Chọn lệnh: ")
        
        if choice == '1': send_cmd("request_status")
        elif choice == '2': 
            vol = input("Nhập volume (0-100): ")
            send_cmd("set_volume", {"volume": int(vol)})
        elif choice == '3':
            bri = input("Nhập độ sáng (0-100): ")
            send_cmd("set_brightness", {"brightness": int(bri)})
        elif choice == '5': send_cmd("reboot")
        elif choice == '6': send_cmd("request_ble_config")
        elif choice == '7': run_ota_flow()
        elif choice == '0': break
        time.sleep(0.5)

except Exception as e: print(f"❌ Lỗi hệ thống: {e}")
finally:
    client.loop_stop()
    client.disconnect()