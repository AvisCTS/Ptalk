import asyncio
import wave
import os
import sys
import json
import hashlib
import struct
import zlib
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
        for nibble in ((b >> 4) & 0x0F, b & 0x0F):  # High nibble trước
            step = STEP_TABLE[index]
            diff = step >> 3
            if nibble & 1:
                diff += step >> 2
            if nibble & 2:
                diff += step >> 1
            if nibble & 4:
                diff += step
            if nibble & 8:
                diff = -diff
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
    samples = [int.from_bytes(pcm[i:i + 2], "little", signed=True) for i in range(0, len(pcm), 2)]
    for s in samples:
        step = STEP_TABLE[index]
        diff = s - predictor
        code = 0x08 if diff < 0 else 0x00
        if code:
            diff = -diff
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
        if code & 4:
            diffq += step
        if code & 2:
            diffq += step >> 1
        if code & 1:
            diffq += step >> 2
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
REPLY_WAV = "chẳng-phải-tình-đầu-sao-đau-đến-thế.wav"  # Ensure this file exists
os.makedirs(RECORD_DIR, exist_ok=True)

app = FastAPI()


def log(tag, msg):
    print(f"[{datetime.now().strftime('%H:%M:%S.%f')[:-3]}] {tag} {msg}")

# Track the most recent/active WebSocket connection
ACTIVE_WS = None  # type: WebSocket | None


def read_checksum(bin_path: str, provided: str | None) -> str:
    if provided:
        # Check if provided is a file path that exists
        if os.path.exists(provided):
            with open(provided, "r", encoding="utf-8") as f:
                return f.read().strip().lower()
        # Otherwise treat it as a hex checksum string
        return provided.strip().lower()
    # Try <bin>.checksum
    chk_path = bin_path + ".checksum"
    if os.path.exists(chk_path):
        with open(chk_path, "r", encoding="utf-8") as f:
            return f.read().strip().lower()
    # Else compute SHA-256
    h = hashlib.sha256()
    with open(bin_path, "rb") as f:
        for chunk in iter(lambda: f.read(4096), b""):
            h.update(chunk)
    return h.hexdigest()


# =====================================================
# OTA CHUNK PROTOCOL WITH CRC32 VERIFICATION
# =====================================================
# Chunk format: [4B seq (LE)][4B size (LE)][4B crc32 (LE)][data...]
# Device responds: {"ota_ack": seq} or {"ota_nack": seq}
# Server retries on NACK or timeout

OTA_CHUNK_SIZE = 2048  # Data payload size per chunk
OTA_ACK_TIMEOUT = 5.0  # Seconds to wait for ACK
OTA_MAX_RETRIES = 3    # Max retries per chunk

# Global state for OTA ACK tracking
ota_ack_event = asyncio.Event()
ota_last_ack_seq = -1
ota_last_nack_seq = -1


def build_ota_chunk(seq: int, data: bytes) -> bytes:
    """Build OTA chunk with header: [seq][size][crc32][data]"""
    crc = zlib.crc32(data) & 0xFFFFFFFF
    header = struct.pack("<III", seq, len(data), crc)
    return header + data


def handle_ota_ack(msg: dict):
    """Process OTA ACK/NACK from device"""
    global ota_last_ack_seq, ota_last_nack_seq, ota_ack_event
    if "ota_ack" in msg:
        ota_last_ack_seq = msg["ota_ack"]
        ota_ack_event.set()
        log("✅", f"ACK received for chunk {ota_last_ack_seq}")
    elif "ota_nack" in msg:
        ota_last_nack_seq = msg["ota_nack"]
        ota_ack_event.set()
        log("❌", f"NACK received for chunk {ota_last_nack_seq}")


async def wait_for_ack(expected_seq: int, timeout: float = OTA_ACK_TIMEOUT) -> bool:
    """Wait for ACK with specific sequence number. Returns True if ACK, False if NACK/timeout."""
    global ota_ack_event, ota_last_ack_seq, ota_last_nack_seq
    ota_ack_event.clear()
    
    try:
        await asyncio.wait_for(ota_ack_event.wait(), timeout=timeout)
        if ota_last_ack_seq == expected_seq:
            return True
        elif ota_last_nack_seq == expected_seq:
            return False
        else:
            log("⚠️", f"Unexpected ACK seq: got {ota_last_ack_seq}, expected {expected_seq}")
            return False
    except asyncio.TimeoutError:
        log("⏰", f"ACK timeout for chunk {expected_seq}")
        return False


async def handle_ota_command(ws: WebSocket, bin_path: str, checksum_hint: str | None):
    global ota_last_ack_seq, ota_last_nack_seq
    
    if not os.path.exists(bin_path):
        log("⚠️", f"File not found: {bin_path}")
        return

    size = os.path.getsize(bin_path)
    checksum = read_checksum(bin_path, checksum_hint)
    
    # Calculate total chunks
    total_chunks = (size + OTA_CHUNK_SIZE - 1) // OTA_CHUNK_SIZE

    # 1) Request OTA with size + checksum + chunk info
    req = {
        "cmd": "request_ota", 
        "size": size, 
        "sha256": checksum,
        "chunk_size": OTA_CHUNK_SIZE,
        "total_chunks": total_chunks
    }
    await ws.send_text(json.dumps(req))
    log("➡️", f"Sent OTA request size={size} chunks={total_chunks} sha256={checksum[:16]}...")
    
    # Reset ACK state
    ota_last_ack_seq = -1
    ota_last_nack_seq = -1

    # 2) Stream binary in chunks with CRC32 verification
    seq = 0
    sent = 0
    failed_chunks = 0
    
    try:
        with open(bin_path, "rb") as f:
            while True:
                data = f.read(OTA_CHUNK_SIZE)
                if not data:
                    break
                
                # Build chunk with CRC32
                chunk = build_ota_chunk(seq, data)
                
                # Send with retry logic
                success = False
                for retry in range(OTA_MAX_RETRIES):
                    await ws.send_bytes(chunk)
                    
                    if retry > 0:
                        log("🔄", f"Retry {retry}/{OTA_MAX_RETRIES} for chunk {seq}")
                    
                    # Wait for ACK
                    if await wait_for_ack(seq):
                        success = True
                        break
                    else:
                        log("⚠️", f"Chunk {seq} not ACKed, retrying...")
                
                if not success:
                    failed_chunks += 1
                    log("❌", f"Chunk {seq} failed after {OTA_MAX_RETRIES} retries")
                    # Continue anyway - device will detect via final SHA256
                
                sent += len(data)
                seq += 1
                
                # Progress log every 10 chunks
                if seq % 10 == 0:
                    percent = (sent * 100) // size
                    log("⬆️", f"Progress: {percent}% ({seq}/{total_chunks} chunks, {sent}/{size} bytes)")
                
                await asyncio.sleep(0)  # yield
                
    except Exception as e:
        log("❌", f"OTA send failed: {e}")
        return

    if failed_chunks > 0:
        log("⚠️", f"OTA completed with {failed_chunks} failed chunks")
    else:
        log("✅", f"OTA binary sent: {sent}/{size} bytes, {seq} chunks, all verified")

    # 3) Notify completion
    await ws.send_text("OTA_COMPLETE")
    log("➡️", "Sent OTA_COMPLETE")


async def send_wav(ws: WebSocket, path: str):
    """Send WAV as ADPCM frames; supports cancellation for barge-in."""
    try:
        if not os.path.exists(path):
            log("⚠️", f"File {path} not found")
            return

        await ws.send_text("PROCESSING_START")
        await asyncio.sleep(0.1)
        await ws.send_text("01")
        await ws.send_text("SPEAK_START")

        tx_state = (0, 0)
        with wave.open(path, "rb") as wf:
            while True:
                pcm = wf.readframes(1024)
                if not pcm:
                    break
                adpcm, tx_state = adpcm_encode(pcm, tx_state)
                await ws.send_bytes(adpcm)
                await asyncio.sleep(0.060)

        await ws.send_text("TTS_END")
        log("🏁", "Playback finished naturally")

    except asyncio.CancelledError:
        log("🚫", "Playback Task Silently Cancelled (Interrupted)")
        raise
    except Exception as e:
        log("❌", f"Error in send_wav: {e}")


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    global ACTIVE_WS
    await ws.accept()
    ACTIVE_WS = ws
    log("📡", "ESP32 connected")

    rx_state = (0, 0)
    pcm_buf = []
    recording = False
    current_tts_task = None

    try:
        while True:
            data = await ws.receive()

            if "bytes" in data:
                if recording:
                    adpcm = data["bytes"]
                    pcm, rx_state = adpcm_decode(adpcm, rx_state)
                    pcm_buf.append(pcm)

            elif "text" in data:
                msg = data["text"]
                # Try JSON first (handshake/acks/status)
                try:
                    obj = json.loads(msg)
                    if isinstance(obj, dict):
                        cmd = obj.get("cmd")
                        log("📩 JSON", obj)
                        if cmd == "device_handshake":
                            ACTIVE_WS = ws
                        # Handle OTA ACK/NACK
                        if "ota_ack" in obj or "ota_nack" in obj:
                            handle_ota_ack(obj)
                        continue
                except Exception:
                    pass

                log("📩 TXT", msg)

                if msg == "START":
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
                        current_tts_task = asyncio.create_task(send_wav(ws, REPLY_WAV))
                    else:
                        log("⚠️", "Empty recording ignored")

    except WebSocketDisconnect:
        log("🔌", "Disconnected")
        if ACTIVE_WS is ws:
            ACTIVE_WS = None
        if current_tts_task:
            current_tts_task.cancel()
    except Exception as e:
        log("❌", f"Websocket Error: {e}")


HELP_TEXT = (
    "Commands:\n"
    "  vol <0-100>         -> set volume\n"
    "  bright <0-100>      -> set brightness\n"
    "  name <text>         -> set device_name\n"
    "  status              -> request_status\n"
    "  reboot              -> reboot device\n"
    "  ble                 -> open BLE config mode\n"
    "  ota <bin> [chk]     -> stream OTA firmware (reads .checksum if not provided)\n"
    "  wifi <ssid> <pass>  -> set_wifi (ESP replies not_supported)\n"
    "  wsurl <ws-url>      -> set_ws_url (reserved/not implemented)\n"
    "  help                -> show this help\n"
    "  quit                -> stop server\n"
)


def build_json(cmd: str, *args: str) -> dict:
    c = cmd.lower()
    if c == "vol" and args:
        return {"cmd": "set_volume", "volume": int(args[0])}
    if c == "bright" and args:
        return {"cmd": "set_brightness", "brightness": int(args[0])}
    if c == "name" and args:
        return {"cmd": "set_device_name", "device_name": " ".join(args)}
    if c == "status":
        return {"cmd": "request_status"}
    if c == "reboot":
        return {"cmd": "reboot"}
    if c == "ble":
        return {"cmd": "request_ble_config"}
    if c == "wifi" and args:
        ssid = args[0]
        password = args[1] if len(args) > 1 else ""
        return {"cmd": "set_wifi", "ssid": ssid, "password": password}
    if c == "wsurl" and args:
        return {"cmd": "set_ws_url", "url": args[0]}
    if c == "ota" and args:
        bin_path = args[0]
        checksum = args[1] if len(args) > 1 else None
        return {"cmd": "__ota__", "bin": bin_path, "checksum": checksum}
    raise ValueError("Unknown or invalid command")


async def console_loop(server: uvicorn.Server):
    global ACTIVE_WS
    print(HELP_TEXT, end="")
    loop = asyncio.get_event_loop()
    while not server.should_exit:
        try:
            line = await loop.run_in_executor(None, sys.stdin.readline)
            if not line:
                await asyncio.sleep(0.05)
                continue
            line = line.strip()
            if not line:
                continue

            parts = line.split()
            cmd, *args = parts
            if cmd.lower() in ("help", "h", "?"):
                print(HELP_TEXT, end="")
                continue
            if cmd.lower() in ("quit", "exit"):
                server.should_exit = True
                break

            if ACTIVE_WS is None:
                log("ℹ️", "No device connected. Waiting for WS client...")
                continue

            try:
                payload = build_json(cmd, *args)
            except Exception as e:
                log("⚠️", f"{e}. Type 'help' for usage.")
                continue

            # OTA command is handled separately (streaming binary)
            if payload.get("cmd") == "__ota__":
                bin_path = payload.get("bin")
                checksum = payload.get("checksum")
                await handle_ota_command(ACTIVE_WS, bin_path, checksum)
                continue

            try:
                await ACTIVE_WS.send_text(json.dumps(payload))
                log("➡️", f"Sent {payload}")
            except Exception as e:
                log("❌", f"Send failed: {e}")
        except Exception as e:
            log("❌", f"Console error: {e}")
            await asyncio.sleep(0.1)


async def main():
    log("🚀", "Server starting at ws://0.0.0.0:8000/ws")
    config = uvicorn.Config(app, host="0.0.0.0", port=8000, loop="asyncio")
    server = uvicorn.Server(config)
    await asyncio.gather(server.serve(), console_loop(server))


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
