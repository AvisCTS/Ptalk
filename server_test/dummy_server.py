import asyncio
import wave
import os
from datetime import datetime
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
import uvicorn

# =====================================================
# IMA ADPCM TABLES (Chuẩn)
# =====================================================
STEP_TABLE = [
     7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
]

INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8,
               -1, -1, -1, -1, 2, 4, 6, 8]

# =====================================================
# ADPCM CODEC
# =====================================================
def adpcm_decode(adpcm, state):
    predictor, index = state or (0, 0)
    pcm = bytearray()
    for b in adpcm:
        for nibble in ((b >> 4) & 0x0F, b & 0x0F): # High nibble trước
            step = STEP_TABLE[index]
            diff = step >> 3
            if nibble & 1: diff += step >> 2
            if nibble & 2: diff += step >> 1
            if nibble & 4: diff += step
            if nibble & 8: diff = -diff
            predictor += diff
            predictor = max(-32768, min(32767, predictor))
            index = max(0, min(88, index + INDEX_TABLE[nibble]))
            pcm += predictor.to_bytes(2, "little", signed=True)
    return pcm, (predictor, index)

def adpcm_encode(pcm, state):
    predictor, index = state or (0, 0)
    out = bytearray()
    high = True
    byte = 0
    samples = [int.from_bytes(pcm[i:i+2], "little", signed=True) for i in range(0, len(pcm), 2)]
    for s in samples:
        step = STEP_TABLE[index]
        diff = s - predictor
        code = 0x08 if diff < 0 else 0x00
        if code: diff = -diff
        if diff >= step:
            code |= 4
            diff -= step
        step >>= 1
        if diff >= step:
            code |= 2
            diff -= step
        step >>= 1
        if diff >= step:
            code |= 1
        
        # Update predictor
        step = STEP_TABLE[index]
        diffq = step >> 3
        if code & 4: diffq += step
        if code & 2: diffq += step >> 1
        if code & 1: diffq += step >> 2
        predictor += -diffq if code & 8 else diffq
        predictor = max(-32768, min(32767, predictor))
        index = max(0, min(88, index + INDEX_TABLE[code]))

        if high:
            byte = (code & 0x0F) << 4
            high = False
        else:
            out.append(byte | (code & 0x0F))
            high = True
    return out, (predictor, index)

# =====================================================
# SERVER CONFIG
# =====================================================
SAMPLE_RATE = 16000
RECORD_DIR = "recordings"
REPLY_WAV = "chẳng-phải-tình-đầu-sao-đau-đến-thế.wav" # Đảm bảo file này tồn tại trong cùng thư mục
os.makedirs(RECORD_DIR, exist_ok=True)

app = FastAPI()

def log(tag, msg):
    print(f"[{datetime.now().strftime('%H:%M:%S.%f')[:-3]}] {tag} {msg}")

async def send_wav(ws: WebSocket, path: str):
    """Gửi file WAV trả lời, có hỗ trợ Cancel để ngắt lời"""
    try:
        if not os.path.exists(path):
            log("⚠️", f"File {path} not found")
            return

        # Thông báo bắt đầu phát
        await ws.send_text("PROCESSING_START")
        await asyncio.sleep(0.1)
        await ws.send_text("01") 
        await ws.send_text("SPEAK_START")

        tx_state = (0, 0)
        with wave.open(path, "rb") as wf:
            while True:
                pcm = wf.readframes(1024)
                if not pcm: break
                
                adpcm, tx_state = adpcm_encode(pcm, tx_state)
                await ws.send_bytes(adpcm)
                await asyncio.sleep(0.060) 

        # Chỉ gửi kết thúc khi file đã phát HẾT bình thường
        await ws.send_text("TTS_END")
        log("🏁", "Playback finished naturally")

    except asyncio.CancelledError:
        # KHI BỊ NGẮT LỜI: 
        # Tuyệt đối KHÔNG gửi SPEAK_END hay bất cứ gì về ESP32
        # ESP32 đang trong trạng thái LISTENING, nếu gửi tin nhắn kết thúc 
        # nó sẽ nhảy về IDLE và kết thúc thu âm ngay lập tức.
        log("🚫", "Playback Task Silently Cancelled (Interrupted)")
        raise # Ném lỗi ra để task kết thúc sạch sẽ

    except Exception as e:
        log("❌", f"Error in send_wav: {e}")

@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await ws.accept()
    log("📡", "ESP32 connected")

    rx_state = (0, 0)
    pcm_buf = []
    recording = False
    current_tts_task = None # Quản lý task gửi âm thanh hiện tại

    try:
        while True:
            data = await ws.receive()

            # --- XỬ LÝ DỮ LIỆU ÂM THANH TỪ MIC ---
            if "bytes" in data:
                if recording:
                    adpcm = data["bytes"]
                    # log("⬆️ RX", f"{len(adpcm)} bytes")
                    pcm, rx_state = adpcm_decode(adpcm, rx_state)
                    pcm_buf.append(pcm)

            # --- XỬ LÝ TIN NHẮN ĐIỀU KHIỂN ---
            elif "text" in data:
                msg = data["text"]
                log("📩 TXT", msg)

                if "identify" in msg:
                    continue

                if msg == "START":
                    # LOGIC NGẮT LỜI: Hủy task gửi âm thanh cũ nếu đang chạy
                    if current_tts_task and not current_tts_task.done():
                        current_tts_task.cancel()
                        log("✂️", "Interrupted previous TTS")
                    
                    pcm_buf.clear()
                    rx_state = (0, 0)
                    recording = True
                    log("🎙️", "Record START")

                elif msg == "END":
                    recording = False
                    if pcm_buf:
                        filename = f"rec_{datetime.now().strftime('%H%M%S')}.wav"
                        path = os.path.join(RECORD_DIR, filename)
                        with wave.open(path, "wb") as wf:
                            wf.setnchannels(1)
                            wf.setsampwidth(2)
                            wf.setframerate(SAMPLE_RATE)
                            wf.writeframes(b"".join(pcm_buf))
                        log("💾", f"Saved {path}")
                        
                        # Bắt đầu gửi file trả lời (Lưu task để có thể cancel)
                        current_tts_task = asyncio.create_task(send_wav(ws, REPLY_WAV))
                    else:
                        log("⚠️", "Empty recording ignored")

    except WebSocketDisconnect:
        log("🔌", "Disconnected")
        if current_tts_task: current_tts_task.cancel()
    except Exception as e:
        log("❌", f"Websocket Error: {e}")

if __name__ == "__main__":
    log("🚀", "Server starting at ws://0.0.0.0:8000/ws")
    uvicorn.run(app, host="0.0.0.0", port=8000)