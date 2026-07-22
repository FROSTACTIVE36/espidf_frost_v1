#pragma once

#include <cstdint>

#include "esp_err.h"

/*
 * Independent bottle-consumption tracker.
 *
 * IR meaning is supplied by main.cpp as a filtered docked state:
 *   docked=false -> bottle removed
 *   docked=true  -> bottle placed back
 *
 * On a removed -> docked transition, the tracker waits five seconds,
 * obtains a stable HX711 weight, and applies the Arduino calculation.
 */
esp_err_t consumption_tracker_init();

void consumption_tracker_set_enabled(bool enabled);
void consumption_tracker_set_docked(bool docked);
void consumption_tracker_set_initial_remaining(float remaining_ml);
void consumption_tracker_update();

bool consumption_tracker_screen_active();
void consumption_tracker_render_screen();

uint32_t consumption_tracker_last_consumed_ml();
uint32_t consumption_tracker_daily_consumed_ml();
uint32_t consumption_tracker_daily_goal_ml();

void consumption_tracker_set_daily_goal_ml(uint32_t goal_ml);
void consumption_tracker_reset_daily();