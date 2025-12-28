# Tài liệu kiến trúc PTalk (đồng bộ với mã nguồn)

## Tổng quan
Tài liệu này mô tả kiến trúc firmware hiện tại của PTalk, đồng bộ trực tiếp với mã trong `src/`. Mục tiêu: tách biệt trách nhiệm, an toàn luồng (thread-safe), dễ mở rộng và dễ test.

---

## 1. Quản lý trạng thái (StateManager)

**Mô hình:** Publish–Subscribe (thread-safe)

### Các kiểu trạng thái (đúng như `src/system/StateTypes.hpp`)
- InteractionState: IDLE, TRIGGERED, LISTENING, PROCESSING, SPEAKING, CANCELLING, MUTED, SLEEPING
- InputSource: VAD, BUTTON, WAKEWORD, SERVER_COMMAND, SYSTEM, UNKNOWN
- ConnectivityState: OFFLINE, CONNECTING_WIFI, WIFI_PORTAL, CONNECTING_WS, ONLINE
- SystemState: BOOTING, RUNNING, ERROR, MAINTENANCE, UPDATING_FIRMWARE, FACTORY_RESETTING
- PowerState: NORMAL, CHARGING, FULL_BATTERY, CRITICAL, ERROR
- EmotionState: NEUTRAL, HAPPY, SAD, ANGRY, CONFUSED, EXCITED, CALM, THINKING

### Đặc điểm chính
- Thread-safe: dùng std::mutex và thực hiện copy callback **trong lock** rồi gọi chúng **ngoài lock** để tránh deadlock và trạng thái vòng lặp.
- Mỗi subscribe trả về một subscription id (int) để unsubscribe an toàn.
- Callback Interaction có thêm `InputSource` để biết nguồn kích hoạt.

Ví dụ đăng ký:
```cpp
int id = StateManager::instance().subscribeInteraction(
    [](state::InteractionState s, state::InputSource src){ /* ... */ });
StateManager::instance().unsubscribeInteraction(id);
```

---

## 2. Chu kỳ sống (Lifecycle) của các Manager
Tất cả manager tuân theo cùng một pattern:
```cpp
bool init();
void start();
void stop();
```

### Các manager chính (hiện có trong mã)
- AppController: bộ điều phối trung tâm, xử lý AppEvent qua hàng đợi FreeRTOS, subscription đến StateManager để thực thi control logic (ví dụ TRIGGERED→LISTENING). File: `src/AppController.{hpp,cpp}`.
- DisplayManager: xử lý UI/animation, **subscribe** trực tiếp tới nhiều state. Không phụ thuộc vào AppController để vẽ UI. File: `src/system/DisplayManager.{hpp,cpp}`.
- AudioManager: quản lý capture/playback, codec, sử dụng stream buffer để trao đổi dữ liệu với NetworkManager. **Subscribe** InteractionState. File: `src/system/AudioManager.{hpp,cpp}`.
- NetworkManager: điều phối WiFi + WebSocket, quản lý retry/portal/OTA streaming, publish ConnectivityState. File: `src/system/NetworkManager.{hpp,cpp}`.
- PowerManager: sampling ADC, smoothing %, publish PowerState; có hook để cập nhật DisplayManager battery%. File: `src/system/PowerManager.{hpp,cpp}`.
- OTAUpdater: ghi/validate firmware chunks (AppController điều khiển flow). File: `src/system/OTAUpdater.{hpp,cpp}`.
- TouchInput: wrapper input, post AppEvent vào AppController.

Lưu ý: DeviceProfile (src/config/DeviceProfile.cpp) là nơi wiring / dependency-injection các manager và driver.

---

## 3. Thứ tự khởi động (quan trọng)
AppController phải tạo task (AppControllerTask) trước khi các module khác có thể gửi state change.

Thứ tự khởi động thực tế trong `AppController::start()`:
1) Tạo AppControllerTask (core 1) → đảm bảo queue và task sẵn sàng
2) PowerManager::start() (sample pin sớm)
3) DisplayManager::startLoop() (hiển thị trạng thái portal / boot)
4) NetworkManager::start() (bỏ qua nếu pin CRITICAL)
5) AudioManager::start() (bỏ qua nếu pin CRITICAL)
6) TouchInput::start()

---

## 4. Tắt (Shutdown) — ngược lại với khởi động
Trong `AppController::stop()` tắt các module theo thứ tự reverse để tránh dangling references:
- NetworkManager.stop() (stop portal, close WS)
- AudioManager.stop()
- DisplayManager.stopLoop()
- PowerManager.stop()

---

## 5. Luồng thông điệp (Message Flow)

### State-driven (reactive)
- Phần cứng (Power/Touched/WS) gọi StateManager.setXxx()
- StateManager sao chép callback và gọi chúng ngoài lock
- Thông thường: AppController nhận message qua queue (dành cho control logic), DisplayManager và AudioManager đăng ký trực tiếp để xử lý UI/audio

### Event-driven (deterministic)
- Các sự kiện ứng dụng (AppEvent) được post vào AppController::postEvent()
- AppControllerTask xử lý tuần tự, map event -> state/actions (ví dụ USER_BUTTON → setInteractionState(LISTENING, BUTTON))

Kết luận: State changes đa hướng và đồng thời; AppEvents có thứ tự tuần tự để giữ tính deterministic.

---

## 6. Cách xử lý lỗi
- Thiết kế nhất quán: các hàm init() trả về bool và log lỗi (ESP_LOGE/ESP_LOGI)
- Một số state ERROR tồn tại để recovery (SystemState::ERROR, PowerState::ERROR)

---

## 7. Cấu hình task (thông tin từ mã nguồn)
| Task | Priority | Stack | Core | Ghi chú |
|---|---:|---:|---:|---|
| AppControllerTask | 4 | 4096 | 1 | Task xử lý state/event trung tâm
| DisplayLoop | 3 | 4096 | 1 | UI loop (~30 FPS)
| AudioMicTask | 5 | 4096 | 0 | Capture MIC (Core 0)
| AudioCodecTask | 4 | 8192 | 0 | Decode/encode (stack lớn)
| AudioSpkTask | 3 | 4096 | 1 | Speaker playback (Core 1)
| NetworkLoop | 5 | 8192 | tskNO_AFFINITY | WiFi/WebSocket loop
| wifi_retry | 5 | 4096 | Any | Fallback portal task
| PowerTimer | timer | - | - | Periodic sampling

Lưu ý: các giá trị lấy trực tiếp từ việc tạo task trong mã.

---

## 8. Dependency injection — nơi cấu hình (DeviceProfile)
`src/config/DeviceProfile.cpp`:
- Tạo các manager/driver (DisplayDriver, I2S mic/spk, Codec)
- Ghi đăng ký asset (emotions, icons)
- Gán buffer uplink/downlink: NetworkManager.setMicBuffer(audio.getMicEncodedBuffer()) và NetworkManager sẽ feed binary → AudioManager downlink
- Gọi `app.attachModules(...)` để gắn các module vào AppController

Lợi ích: dễ test (mock các manager), rõ ownership, dễ chỉnh cấu hình board-specific.

---

## 9. An toàn luồng (Thread safety)
- StateManager dùng mutex + copy callbacks
- AudioManager dùng stream buffer (FreeRTOS) để tránh chia sẻ bộ nhớ không an toàn
- Mỗi manager sở hữu task riêng, tránh dùng chung mutex toàn cục
- AppController dùng queue (FreeRTOS) để serialize công việc cross-module

---

## 10. State machine ví dụ: Interaction
- IDLE → (USER_BUTTON / WAKEWORD / SERVER_FORCE_LISTEN) → TRIGGERED
- TRIGGERED → (AppController auto) → LISTENING
- LISTENING → (server processing) → PROCESSING
- PROCESSING → SPEAKING → (playback end) → IDLE

AppController xử lý chuyển TRIGGERED→LISTENING và xử lý các AppEvent tương ứng.

---

## 11. OTA flow (thực tế trong mã)
1) AppEvent::OTA_BEGIN → AppController set SystemState::UPDATING_FIRMWARE
2) NetworkManager stream binary chunks → NetworkManager.onFirmwareChunk → OTAUpdater.writeChunk
3) NetworkManager thông báo hoàn tất → OTAUpdater.finishUpdate() → AppController post OTA_FINISHED → reboot
4) DisplayManager subscribe SystemState để hiển thị tiến trình

---

## 12. Battery critical flow
- PowerManager đánh giá và publish PowerState::CRITICAL khi battery <= config.critical_percent
- AppController::onPowerStateChanged() gọi enterSleep(): stop network/audio/display, tắt backlight, cấu hình timer wakeup và esp_deep_sleep_start()
- Khi wake, boot lại và PowerManager sampleNow() để quyết định tiếp tục boot hay ngủ lại

---

## 13. Emotion system (server-driven)
- NetworkManager parseEmotionCode(code) và gọi StateManager::setEmotionState()
- Mapping thực tế theo mã:
  - "00"/empty → NEUTRAL
  - "01" → HAPPY
  - "02" → ANGRY
  - "03" → EXCITED
  - "10" → SAD
  - "12" → CONFUSED
  - "13" → CALM
  - "99" → THINKING
  (Xem `NetworkManager::parseEmotionCode` để biết mapping chính xác)

- DisplayManager subscribe EmotionState và phát animation tương ứng (assets ở `src/assets/emotions/`), register bằng DeviceProfile.
- Để tạo asset: dùng `scripts/convert_assets.py` (GIF → C++ header).

---

## 14. Testing & Observability
- Thiết kế để dễ mock: AppController và StateManager dễ test unit bằng mock objects
- Logs (ESP_LOG*) được dùng rộng rãi để debug runtime behavior
- Ví dụ unit test mô phỏng AppEvent và kiểm tra state transition

---

## 15. Tham khảo nhanh trong mã nguồn
- `src/system/StateManager.{hpp,cpp}`
- `src/system/StateTypes.hpp`
- `src/AppController.{hpp,cpp}`
- `src/config/DeviceProfile.{cpp}`
- `src/system/{DisplayManager,AudioManager,NetworkManager,PowerManager,OTAUpdater}.{hpp,cpp}`

---

Ghi chú: Tài liệu này được cập nhật để phản ánh trạng thái hiện hành của repository (mã nguồn trong `src/`). Nếu bạn muốn tôi đưa thêm sơ đồ, ví dụ UML, hoặc tóm tắt ngắn hơn bằng tiếng Anh, cho biết mình sẽ bổ sung tiếp.

































