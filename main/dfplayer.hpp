#pragma once

#include <cstdint>
#include "esp_err.h"

esp_err_t dfplayer_init(uint8_t volume = 20);

esp_err_t dfplayer_set_volume(uint8_t volume);
esp_err_t dfplayer_play_track(uint16_t track);
esp_err_t dfplayer_play_advertisement(uint16_t track);
esp_err_t dfplayer_pause();
esp_err_t dfplayer_resume();
esp_err_t dfplayer_stop();

bool dfplayer_is_playing();
void dfplayer_update();