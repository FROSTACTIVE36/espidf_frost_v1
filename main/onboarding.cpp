#include "onboarding.hpp"
#include <cstdint>
#include "esp_log.h"
#include "nvs.h"

namespace {
static const char* TAG = "FROST_ONBOARDING";
static constexpr char NVS_NAMESPACE[] = "frost_bind";
static constexpr char NVS_KEY_BOUND[] = "bound";
bool initialized = false;
bool bound = false;

esp_err_t save_bound(bool value)
{
    nvs_handle_t h=0;
    esp_err_t err=nvs_open(NVS_NAMESPACE,NVS_READWRITE,&h);
    if(err!=ESP_OK) return err;
    err=nvs_set_u8(h,NVS_KEY_BOUND,value?1U:0U);
    if(err==ESP_OK) err=nvs_commit(h);
    nvs_close(h);
    if(err==ESP_OK) bound=value;
    ESP_LOGI(TAG,"Binding state: %s", bound?"BOUND":"UNBOUND");
    return err;
}
}

esp_err_t onboarding_init()
{
    nvs_handle_t h=0;
    esp_err_t err=nvs_open(NVS_NAMESPACE,NVS_READONLY,&h);
    if(err==ESP_ERR_NVS_NOT_FOUND){ initialized=true; bound=false; return ESP_OK; }
    if(err!=ESP_OK) return err;
    uint8_t value=0;
    err=nvs_get_u8(h,NVS_KEY_BOUND,&value);
    nvs_close(h);
    if(err==ESP_ERR_NVS_NOT_FOUND){ initialized=true; bound=false; return ESP_OK; }
    if(err!=ESP_OK) return err;
    bound=value!=0;
    initialized=true;
    ESP_LOGI(TAG,"Loaded binding state: %s", bound?"BOUND":"UNBOUND");
    return ESP_OK;
}

bool onboarding_is_required(){ return !initialized || !bound; }
bool onboarding_is_bound(){ return initialized && bound; }
esp_err_t onboarding_confirm_binding(){ return initialized?save_bound(true):ESP_ERR_INVALID_STATE; }
esp_err_t onboarding_reset_binding(){ return initialized?save_bound(false):ESP_ERR_INVALID_STATE; }
