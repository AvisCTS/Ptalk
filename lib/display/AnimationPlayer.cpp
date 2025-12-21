#include "AnimationPlayer.hpp"
#include "DisplayDriver.hpp"
#include "esp_log.h"
#include <cstring>
#include "assets/emotions/emotion_types.hpp"

static const char* TAG = "AnimationPlayer";

// RGB565 colors for black and white
static constexpr uint16_t COLOR_BLACK = 0x0000;
static constexpr uint16_t COLOR_WHITE = 0xFFFF;

AnimationPlayer::AnimationPlayer(Framebuffer* fb, DisplayDriver* drv)
    : fb_(fb), drv_(drv), working_buffer_(nullptr), buffer_size_(0)
{
    if (!fb_ || !drv_) {
        ESP_LOGE(TAG, "AnimationPlayer created with null fb or driver!");
    }
}

AnimationPlayer::~AnimationPlayer()
{
    if (working_buffer_) {
        free(working_buffer_);
        working_buffer_ = nullptr;
    }
}

void AnimationPlayer::setAnimation(const Animation1Bit& anim, int x, int y)
{
    if (!anim.valid()) {
        ESP_LOGW(TAG, "setAnimation: invalid animation");
        stop();
        return;
    }

    current_anim_ = anim;
    pos_x_ = x;
    pos_y_ = y;

    frame_index_ = 0;
    frame_timer_ = 0;
    paused_ = false;
    playing_ = true;

    if (anim.fps == 0) {
        frame_interval_ = 50;  // fallback 20 fps
    } else {
        frame_interval_ = 1000 / anim.fps;
    }

    // Allocate working buffer for RGB565 frame
    size_t required_size = anim.width * anim.height * sizeof(uint16_t);
    if (required_size != buffer_size_) {
        if (working_buffer_) {
            free(working_buffer_);
        }
        working_buffer_ = (uint16_t*)malloc(required_size);
        buffer_size_ = required_size;
        
        if (!working_buffer_) {
            ESP_LOGE(TAG, "Failed to allocate working buffer (%zu bytes)", required_size);
            stop();
            return;
        }
    }

    // Decode base frame (frame 0)
    decode1BitToRGB565(anim.base_frame, anim.width, anim.height);

    ESP_LOGI(TAG, "Animation set: %d frames (%dx%d), fps=%u, loop=%s",
             anim.frame_count, anim.width, anim.height, anim.fps,
             anim.loop ? "true" : "false");
}

void AnimationPlayer::stop()
{
    playing_ = false;
    paused_  = false;
    frame_index_ = 0;
    frame_timer_ = 0;
}

void AnimationPlayer::pause()
{
    paused_ = true;
}

void AnimationPlayer::resume()
{
    paused_ = false;
}

void AnimationPlayer::update(uint32_t dt_ms)
{
    if (!playing_ || paused_ || !current_anim_.valid())
        return;

    frame_timer_ += dt_ms;

    // Chuyển frame theo thời gian
    while (frame_timer_ >= frame_interval_) {
        frame_timer_ -= frame_interval_;
        frame_index_++;

        if (frame_index_ >= (size_t)current_anim_.frame_count) {
            if (current_anim_.loop) {
                // Loop back: decode base frame again
                frame_index_ = 0;
                decode1BitToRGB565(current_anim_.base_frame, 
                                  current_anim_.width, current_anim_.height);
            } else {
                // one-shot animation dừng ở frame cuối
                frame_index_ = current_anim_.frame_count - 1;
                playing_ = false;
                break;
            }
        } else {
            // Apply diff for new frame
            const asset::emotion::FrameInfo& frame_info = current_anim_.frames[frame_index_];
            if (frame_info.diff != nullptr) {
                applyDiffBlock(frame_info.diff);
            }
        }
    }
}

void AnimationPlayer::decode1BitToRGB565(const uint8_t* packed_data, int width, int height)
{
    if (!working_buffer_ || !packed_data) return;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int bit_index = y * width + x;
            int byte_index = bit_index / 8;
            int bit_offset = 7 - (bit_index % 8); // MSB first
            
            bool is_white = (packed_data[byte_index] >> bit_offset) & 1;
            working_buffer_[y * width + x] = is_white ? COLOR_WHITE : COLOR_BLACK;
        }
    }
}

void AnimationPlayer::applyDiffBlock(const asset::emotion::DiffBlock* diff)
{
    if (!diff || !diff->data || !working_buffer_) return;

    int anim_width = current_anim_.width;
    
    for (int dy = 0; dy < diff->height; dy++) {
        for (int dx = 0; dx < diff->width; dx++) {
            int bit_index = dy * diff->width + dx;
            int byte_index = bit_index / 8;
            int bit_offset = 7 - (bit_index % 8); // MSB first
            
            bool is_white = (diff->data[byte_index] >> bit_offset) & 1;
            
            int px = diff->x + dx;
            int py = diff->y + dy;
            
            if (px >= 0 && px < anim_width && py >= 0 && py < current_anim_.height) {
                working_buffer_[py * anim_width + px] = is_white ? COLOR_WHITE : COLOR_BLACK;
            }
        }
    }
}

void AnimationPlayer::render()
{
    if (!playing_ || !fb_ || !current_anim_.valid() || !working_buffer_) return;

    // Draw current decoded frame from working buffer
    fb_->drawBitmap(pos_x_, pos_y_, 
                   current_anim_.width, current_anim_.height, 
                   working_buffer_);
}
