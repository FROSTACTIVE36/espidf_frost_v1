#pragma once

#include <cstddef>

bool reminder_config_parse_and_apply(
    const char* json_text,
    char* error_message,
    std::size_t error_message_size
); 