#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
VAD Server với RAG Integration
- Tích hợp RAG (Milvus + BGE-M3) cho câu hỏi kiến thức
- WebSocket streaming audio
- Button-based recording (Start/End)
"""

import os
import asyncio
import wave
from datetime import datetime
from collections import deque
from pathlib import Path
from typing import Optional, List
import numpy as np
import inspect
import soundfile as sf
import torch
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request
from fastapi.responses import PlainTextResponse, JSONResponse
from dotenv import load_dotenv

# =========================================================
# 0) LOAD ENV & ENABLE RAG
# =========================================================
load_dotenv()

# 🔥 BẬT RAG TRƯỚC KHI IMPORT SETTINGS
from settings import llm_settings as cfg
cfg.RAG_ENABLED = True  # BẬT RAG

INPUT_WAV_PATH = Path(os.getenv("INPUT_WAV_PATH", "audio_files/input.wav")).resolve()

# =========================================================
# 1) ENV & PATCHES
# =========================================================

def _patch_torchaudio_load():
    try:
        import torchaudio
        import torch as _torch
        import numpy as _np
        import soundfile as _sf
        _orig = getattr(torchaudio, "load", None)
        if not callable(_orig):
            return

        def _safe_load(path, *args, **kwargs):
            try:
                return _orig(path, *args, **kwargs)
            except Exception:
                data, sr = _sf.read(path, dtype="float32", always_2d=True)
                data = _torch.from_numpy(_np.ascontiguousarray(data.T))
                return data, sr
        torchaudio.load = _safe_load
    except Exception:
        pass

os.environ.setdefault("TTS_BACKEND", os.getenv("TTS_BACKEND", "2"))
os.environ.setdefault("F5_NATIVE", os.getenv("F5_NATIVE", "1"))
os.environ.setdefault("HF_HOME", "/home/devops/.cache/huggingface")
os.environ.setdefault("HUGGINGFACE_HUB_CACHE", os.path.join(os.environ["HF_HOME"], "hub"))
os.environ.setdefault("HF_HUB_OFFLINE", "0")
os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")

_patch_torchaudio_load()

# =========================================================
# 2) CẤU HÌNH AUDIO/VAD
# =========================================================
SAMPLE_RATE = 16000
CHANNELS = 1
VAD_CHUNK_SIZE = 512
VAD_SPEECH_THRESHOLD = 0.4
VAD_SILENCE_FRAMES_TRIGGER = 1
VAD_SILENCE_FRAMES_END = 30
VAD_BUFFER_FRAMES = 15

# IMA ADPCM tables
index_table = [ -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 ]
step_table = [7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767]

def adpcm_bytes_to_pcm_int16(adpcm_bytes: bytes, state: dict = None, gain: float = 1.0):
    """ADPCM decoder"""
    import numpy as _np
    if state is None: state = {'predictor': 0, 'index': 0}
    predictor = int(state.get('predictor', 0))
    index = int(state.get('index', 0))
    if index < 0: index = 0
    if index > 88: index = 88
    step = step_table[index]
    out_samples = []
    for b in adpcm_bytes:
        for shift in (0, 4):
            nibble = (b >> shift) & 0x0F
            sign = nibble & 8
            delta = nibble & 7
            diffq = step >> 3
            if delta & 4: diffq += step
            if delta & 2: diffq += (step >> 1)
            if delta & 1: diffq += (step >> 2)
            if sign: predictor -= diffq
            else: predictor += diffq
            if predictor > 32767: predictor = 32767
            if predictor < -32768: predictor = -32768
            index += index_table[nibble]
            if index < 0: index = 0
            if index > 88: index = 88
            step = step_table[index]
            sample = int(predictor * gain)
            if sample > 32767: sample = 32767
            if sample < -32768: sample = -32768
            out_samples.append(sample)
    state['predictor'] = predictor
    state['index'] = index
    return _np.array(out_samples, dtype=_np.int16)

def adpcm_encode_pcm_int16_to_bytes(pcm_int16: np.ndarray, state: dict = None, gain: float = 1.0) -> bytes:
    """ADPCM encoder"""
    if state is None: state = {'predictor': 0, 'index': 0}
    predictor = int(state.get('predictor', 0))
    index = int(state.get('index', 0))
    if index < 0: index = 0
    if index > 88: index = 88
    step = step_table[index]
    out_bytes = bytearray()
    out_byte = 0
    high_nibble = False
    pcm = pcm_int16.astype(np.int32)
    for s in pcm:
        s = int(s * gain)
        if s > 32767: s = 32767
        if s < -32768: s = -32768
        diff = s - predictor
        sign = 0
        if diff < 0:
            sign = 8
            diff = -diff
        delta = 0
        tempStep = step
        if diff >= tempStep:
            delta |= 4
            diff -= tempStep
        tempStep >>= 1
        if diff >= tempStep:
            delta |= 2
            diff -= tempStep
        tempStep >>= 1
        if diff >= tempStep:
            delta |= 1
        nibble = delta | sign
        diffq = step >> 3
        if delta & 4: diffq += step
        if delta & 2: diffq += (step >> 1)
        if delta & 1: diffq += (step >> 2)
        if sign: predictor -= diffq
        else: predictor += diffq
        if predictor > 32767: predictor = 32767
        if predictor < -32768: predictor = -32768
        index += index_table[nibble]
        if index < 0: index = 0
        if index > 88: index = 88
        step = step_table[index]
        if not high_nibble:
            out_byte = nibble & 0x0F
            high_nibble = True
        else:
            out_byte |= ((nibble & 0x0F) << 4)
            out_bytes.append(out_byte & 0xFF)
            high_nibble = False
    if high_nibble:
        out_bytes.append(out_byte & 0xFF)
    state['predictor'] = predictor
    state['index'] = index
    return bytes(out_bytes)

# =========================================================
# 3) INIT PIPELINE VỚI RAG
# =========================================================
from modules.pipeline import VoiceAssistantPipeline

app = FastAPI(
    title="AVIS Voice Assistant with RAG",
    description="Voice Assistant Server với RAG (Milvus + BGE-M3)",
    version="2.0"
)

_pipeline: Optional[VoiceAssistantPipeline] = None
_vad_model = None
_vad_utils = None

# 🔥 THỐNG KÊ RAG
_stats = {
    "total_queries": 0,
    "total_time": 0.0,
    "rag_queries": 0,
}

def save_audio_to_wav(audio_data: bytes) -> str:
    """Lưu audio bytes thành file WAV"""
    out_path = INPUT_WAV_PATH
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = out_path.with_suffix(".tmp")
    with wave.open(tmp_path.as_posix(), 'wb') as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(audio_data)
    os.replace(tmp_path, out_path)
    return str(out_path)

@app.on_event("startup")
async def on_startup():
    global _pipeline, _vad_model, _vad_utils
    import time
    
    print("\n" + "="*60)
    print("🚀 STARTUP - Voice Assistant Server với RAG")
    print("="*60)
    
    # 1. Hiển thị cấu hình
    print(f"\n📋 CẤU HÌNH:")
    print(f"   🤖 LLM Backend: {cfg.LLM_BACKEND}")
    print(f"   📚 RAG: {'✅ BẬT' if cfg.RAG_ENABLED else '❌ TẮT'}")
    print(f"   📚 RAG Mode: {cfg.RAG_MODE if cfg.RAG_ENABLED else 'N/A'}")
    
    if cfg.LLM_BACKEND in ("auto", "host"):
        print(f"   🖥️ vLLM Host: {cfg.VLLM_BASE_URL}")
        print(f"   🤖 vLLM Model: {cfg.VLLM_MODEL}")
    
    if cfg.LLM_BACKEND in ("auto", "api"):
        if cfg.OPENAI_API_KEY and cfg.OPENAI_API_KEY != "sk-proj-your_actual_key_here":
            print(f"   ✅ OpenAI API Key: Configured")
            print(f"   🤖 OpenAI Model: {cfg.OPENAI_MODEL}")
        else:
            print("   ⚠️ OpenAI API Key: Not configured")

    # 2. Init Pipeline (bao gồm LLMEngine với RAG)
    print(f"\n⏳ Đang khởi tạo Pipeline + RAG Engine...")
    start_init = time.time()
    
    try:
        _pipeline = VoiceAssistantPipeline()
        init_time = time.time() - start_init
        print(f"   ✅ Pipeline initialized in {init_time:.2f}s")
        
        # Kiểm tra RAG Engine
        if hasattr(_pipeline, 'llm_engine') and hasattr(_pipeline.llm_engine, '_rag_engine'):
            if _pipeline.llm_engine._rag_engine:
                print(f"   ✅ RAG Engine (Milvus) ready!")
            else:
                print(f"   ⚠️ RAG Engine not initialized")
        
    except Exception as e:
        print(f"   ❌ Pipeline init failed: {e}")
        import traceback
        traceback.print_exc()
        raise e

    # 3. Init VAD
    print(f"\n⏳ Đang tải Silero VAD...")
    try:
        torch.set_num_threads(1)
        _vad_model, _vad_utils = torch.hub.load(
            repo_or_dir='snakers4/silero-vad',
            model='silero_vad',
            force_reload=False,
            onnx=False,
            trust_repo=True
        )
        print(f"   ✅ Silero VAD model loaded")
    except Exception as e:
        print(f"   ❌ Error loading Silero VAD: {e}")
        import traceback
        traceback.print_exc()

    print("\n" + "="*60)
    print("✅ Server ready! Listening on ws://0.0.0.0:8000/ws")
    print("="*60)
    print("\n💡 GỢI Ý CÂU HỎI TEST RAG:")
    print("   - Âm i là gì?")
    print("   - Chữ cái đầu tiên là gì?")
    print("   - Bác Hồ sinh năm nào?")
    print("   - Việt Nam có bao nhiêu vùng biển?")
    print("   - Chủ tịch nước Việt Nam là ai?")
    print("   - Thủ tướng Việt Nam là ai?")
    print("="*60 + "\n")

@app.get("/")
async def root():
    """Root endpoint - Server info"""
    return JSONResponse({
        "service": "AVIS Voice Assistant with RAG",
        "version": "2.0",
        "status": "Running ✅",
        "features": {
            "rag": "Milvus + BGE-M3 + Reranker",
            "llm": cfg.VLLM_MODEL,
            "vad": "Silero VAD",
            "tts": "F5-TTS",
            "stt": "Sherpa-ONNX"
        },
        "server_info": {
            "llm_backend": cfg.LLM_BACKEND,
            "rag_enabled": cfg.RAG_ENABLED,
            "rag_mode": cfg.RAG_MODE if cfg.RAG_ENABLED else "none"
        },
        "endpoints": {
            "health": "GET /health - Health check",
            "stats": "GET /stats - RAG statistics",
            "websocket": "WS /ws - Voice assistant WebSocket"
        },
        "usage": {
            "websocket": "ws://YOUR_IP:8000/ws",
            "protocol": "Send device_id first, then 'Start'/'End' text or audio binary"
        }
    })

@app.get("/health")
async def health_check():
    """Health check endpoint"""
    rag_status = "unknown"
    if _pipeline and hasattr(_pipeline, 'llm_engine'):
        if hasattr(_pipeline.llm_engine, '_rag_engine') and _pipeline.llm_engine._rag_engine:
            rag_status = "connected"
        else:
            rag_status = "not_initialized"
    
    return JSONResponse({
        "status": "ok",
        "llm_backend": cfg.LLM_BACKEND,
        "rag_enabled": cfg.RAG_ENABLED,
        "rag_status": rag_status,
        "rag_mode": cfg.RAG_MODE if cfg.RAG_ENABLED else "none",
        "pipeline_ready": _pipeline is not None,
        "vad_ready": _vad_model is not None,
    })

@app.get("/stats")
async def get_stats():
    """RAG Statistics endpoint"""
    avg_time = 0.0
    if _stats["total_queries"] > 0:
        avg_time = _stats["total_time"] / _stats["total_queries"]
    
    return JSONResponse({
        "total_queries": _stats["total_queries"],
        "total_time_seconds": round(_stats["total_time"], 2),
        "average_time_seconds": round(avg_time, 2),
        "rag_queries": _stats["rag_queries"],
    })

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    import time as time_module
    
    await websocket.accept()
    
    # 1. Nhận device_id đầu tiên
    message = await websocket.receive()
    device_id = message.get("text", "unknown")
    client = getattr(websocket, "client", None)
    print(f"\n🔌 Client connected: {getattr(client, 'host', 'unknown')} | Device: {device_id}")
    print(f"   📚 RAG: {'✅ Enabled' if cfg.RAG_ENABLED else '❌ Disabled'}")

    if _pipeline is None or _vad_model is None:
        await websocket.close(code=1011, reason="Server not ready")
        return

    # Reset VAD khi client kết nối
    try:
        if hasattr(_vad_model, 'reset_states'):
            _vad_model.reset_states()
            print("   ✨ VAD states reset on connection")
    except Exception as e:
        print(f"   ⚠️ Failed to reset VAD states: {e}")
    
    get_speech_timestamps, save_audio, read_audio, VADIterator, collect_chunks = _vad_utils

    # Init States
    adpcm_dec_state = {'predictor': 0, 'index': 0}
    adpcm_enc_state = {'predictor': 0, 'index': 0}
    pcm_acc = np.zeros(0, dtype=np.int16)

    # State cho Start/End button
    is_button_active = False
    is_speaking = False
    silence_counter = 0
    speech_trigger_counter = 0
    is_processing = False
    
    pre_buffer = deque(maxlen=VAD_BUFFER_FRAMES)
    speech_buffer: List[bytes] = []
    
    # Session ID cho RAG context
    session_id = f"device_{device_id}_{int(time_module.time())}"
    
    try:
        while True:
            try:
                message = await websocket.receive()
                
                # 1. Text control
                if "text" in message:
                    text_data = message["text"]
                    
                    # XỬ LÝ "Start" - Bắt đầu thu âm
                    if text_data == "Start":
                        print("\n🎙️ [BTN] Start pressed - Begin recording")
                        is_button_active = True
                        # Reset states
                        adpcm_dec_state = {'predictor': 0, 'index': 0}
                        if hasattr(_vad_model, 'reset_states'):
                            _vad_model.reset_states()
                        speech_buffer.clear()
                        pre_buffer.clear()
                        is_speaking = False
                        silence_counter = 0
                        speech_trigger_counter = 0
                        await websocket.send_text("LISTENING")
                        continue
                    
                    # XỬ LÝ "End" - Kết thúc thu âm và xử lý
                    elif text_data == "End":
                        print("🛑 [BTN] End pressed - Process recording")
                        is_button_active = False
                        
                        # Nếu có audio trong buffer thì xử lý
                        if len(speech_buffer) > 0:
                            is_processing = True
                            await websocket.send_text("PROCESSING_START")
                            
                            full_audio = b"".join(speech_buffer)
                            input_path = save_audio_to_wav(full_audio)
                            
                            print(f"🎤 Processing {len(speech_buffer)} frames...")
                            
                            # 🔥 GỌI PIPELINE VỚI RAG
                            query_start = time_module.time()
                            try:
                                result = await _pipeline.process(
                                    audio_input_path=input_path,
                                    session_id=session_id  # Dùng session_id để giữ context
                                )
                                
                                query_time = time_module.time() - query_start
                                
                                # Cập nhật thống kê
                                _stats["total_queries"] += 1
                                _stats["total_time"] += query_time
                                
                                # Lấy emotion từ result
                                output_emoji = result.get("emotion_details", "neutral")
                                response_text = result.get("response_text", "")
                                
                                await websocket.send_text(output_emoji)
                                
                                print(f"💬 Response: {response_text[:100]}..." if len(response_text) > 100 else f"💬 Response: {response_text}")
                                print(f"⏱️ Query time: {query_time:.2f}s | Emotion: {output_emoji}")

                                # Stream audio response
                                output_audio_path = result.get("output_audio")
                                if output_audio_path and os.path.exists(output_audio_path):
                                    wav, sr = sf.read(str(output_audio_path), dtype='float32', always_2d=True)
                                    if wav.shape[1] > 1: wav = wav.mean(axis=1)
                                    else: wav = wav[:, 0]
                                    
                                    if sr != SAMPLE_RATE:
                                        new_len = int(len(wav) * SAMPLE_RATE / sr)
                                        wav = np.interp(np.linspace(0, 1, new_len), np.linspace(0, 1, len(wav)), wav).astype('float32')
                                    
                                    wav = np.clip(wav, -1.0, 1.0)
                                    pcm_out = (wav * 32767.0).astype(np.int16)

                                    # Reset encoder state
                                    adpcm_enc_state['predictor'] = 0
                                    adpcm_enc_state['index'] = 0

                                    # Stream audio
                                    chunk_samples = 1024
                                    idx = 0
                                    total = len(pcm_out)
                                    chunk_dt = chunk_samples / SAMPLE_RATE
                                    
                                    while idx < total:
                                        end = min(idx + chunk_samples, total)
                                        chunk = pcm_out[idx:end]
                                        if len(chunk) < chunk_samples:
                                            chunk = np.pad(chunk, (0, chunk_samples - len(chunk)), 'constant')
                                        
                                        b = adpcm_encode_pcm_int16_to_bytes(chunk, state=adpcm_enc_state)
                                        await websocket.send_bytes(b)
                                        idx += chunk_samples
                                        await asyncio.sleep(chunk_dt)
                                    
                                    print("🔊 Audio streaming complete")

                            except Exception as e:
                                print(f"❌ Pipeline Error: {e}")
                                import traceback
                                traceback.print_exc()
                            finally:
                                try:    
                                    await asyncio.sleep(0.5)
                                    adpcm_dec_state = {'predictor': 0, 'index': 0}
                                    adpcm_enc_state = {'predictor': 0, 'index': 0}
                                    await websocket.send_text("TTS_END")
                                except: pass
                                
                                # Reset VAD
                                try:
                                    if hasattr(_vad_model, 'reset_states'):
                                        _vad_model.reset_states()
                                        print("✨ VAD states reset after turn")
                                except Exception: pass

                            # Reset flags
                            is_speaking = False
                            silence_counter = 0
                            speech_trigger_counter = 0
                            speech_buffer.clear()
                            pre_buffer.clear()
                            is_processing = False
                        else:
                            print("⚠️ Button released but no audio recorded")
                        
                        continue
                    
                    # Các command khác
                    elif text_data == "RESET_ADPCM" or text_data == "SYNC_START":
                        adpcm_dec_state = {'predictor': 0, 'index': 0}
                        if hasattr(_vad_model, 'reset_states'):
                            _vad_model.reset_states()
                        print("♻️ Manual Reset/Sync from Client")
                    
                    # 🔥 THÊM: Command để clear session (reset context)
                    elif text_data == "CLEAR_SESSION":
                        session_id = f"device_{device_id}_{int(time_module.time())}"
                        print(f"🔄 Session cleared, new session: {session_id}")
                        await websocket.send_text("SESSION_CLEARED")
                    
                    continue

                # 2. Audio bytes - CHỈ XỬ LÝ KHI BUTTON ĐANG ACTIVE
                if "bytes" in message:
                    data = message["bytes"]
                else:
                    continue

            except WebSocketDisconnect:
                print("🔌 Client disconnected")
                break

            # BỎ QUA AUDIO NẾU BUTTON KHÔNG ACTIVE hoặc đang processing
            if not is_button_active or is_processing or not data:
                continue

            # DECODE ADPCM
            try:
                pcm_block = adpcm_bytes_to_pcm_int16(data, state=adpcm_dec_state, gain=2.5)
            except Exception:
                continue

            pcm_acc = np.concatenate((pcm_acc, pcm_block))

            # VAD LOGIC - COLLECT AUDIO KHI BUTTON ACTIVE
            frame_size = 512
            while pcm_acc.shape[0] >= frame_size:
                frame = pcm_acc[:frame_size]
                pcm_acc = pcm_acc[frame_size:]

                frame_bytes = frame.tobytes()
                
                # Collect audio khi button active
                speech_buffer.append(frame_bytes)
                
                # Optional: VAD để debug
                audio_float = frame.astype('float32') / 32768.0
                audio_tensor = torch.from_numpy(audio_float).float()
                with torch.no_grad():
                    try:
                        speech_prob = _vad_model(audio_tensor, SAMPLE_RATE).item()
                        # Chỉ print mỗi 10 frames để giảm spam
                        if len(speech_buffer) % 10 == 0:
                            print(f"VAD: {speech_prob:.2f} | Frames: {len(speech_buffer)}")
                    except:
                        pass

    except Exception as e:
        print(f"❌ WS Error: {e}")
        import traceback
        traceback.print_exc()
    finally:
        # Log stats khi client disconnect
        print(f"\n📊 Session Stats for {device_id}:")
        print(f"   Total queries: {_stats['total_queries']}")
        if _stats['total_queries'] > 0:
            avg = _stats['total_time'] / _stats['total_queries']
            print(f"   Average time: {avg:.2f}s")

if __name__ == "__main__":
    import uvicorn
    print("\n" + "="*60)
    print("🚀 Starting AVIS Voice Assistant Server with RAG")
    print("="*60)
    uvicorn.run(app, host="0.0.0.0", port=8000)