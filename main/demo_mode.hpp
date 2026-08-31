#pragma once

/*
 * FROST development/demo mode.
 *
 * The feature is intentionally self-contained. Production builds can disable
 * all entry points from main.cpp with:
 *
 *     #define FROST_ENABLE_DEMO_MODE 0
 */

void demo_mode_init();
void demo_mode_start();
void demo_mode_stop();
void demo_mode_update();
bool demo_mode_is_active();
