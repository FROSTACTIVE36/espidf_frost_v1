#pragma once
#include "esp_err.h"

esp_err_t onboarding_init();
bool onboarding_is_required();
bool onboarding_is_bound();
esp_err_t onboarding_confirm_binding();
esp_err_t onboarding_reset_binding();
