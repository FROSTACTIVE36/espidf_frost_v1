# FROST ESP32-S3 Firmware

This repository contains the firmware for the FROST ESP32-S3 reminder, hydration, Pomodoro, healing and meditation device.

The code is implemented as an ESP-IDF project with a main firmware component in `main/`, a graphical display library in `components/LovyanGFX/`, and JSON support in `managed_components/espressif__cjson/`.

## Target hardware

- MCU: ESP32-S3-N16R8
- Display: GC9A01 240x240 circular TFT via LovyanGFX
- Audio: DFPlayer Mini over UART1 plus BUSY feedback pin
- RTC: DS3231 over I2C
- Scale: HX711 load cell interface
- IR sensor: shared input used for reminder acknowledgement, Pomodoro gestures, and dock detection

## High-level features

- Reminder engine with interval and absolute-time reminders
- Medication and custom reminder screens
- Pomodoro focus/break timer with announcement and background music
- Healing schedule support with announcement + playlist
- Meditation music mode
- Shared IR input for acknowledgement, Pomodoro double-tap, and dock-state detection
- Bottle calibration and water consumption tracking using HX711
- Daily hydration statistics and BLE statistics export
- Bluetooth JSON configuration and persistent settings in NVS
- OTA manager and Wi-Fi helper modules
- Circular display UI with font handling and artwork

## Repository structure

- `CMakeLists.txt` - Root ESP-IDF project entry point
- `main/` - Firmware source files, headers, display image assets, and design documentation
- `components/LovyanGFX/` - LovyanGFX display library and examples
- `managed_components/espressif__cjson/` - cJSON component for JSON parsing
- `partitions.csv` - Partition table description
- `sdkconfig.old` - Backup SDK configuration

## Main module responsibilities

### `main/main.cpp`
- Application entry point and `app_main`
- Initializes NVS, display, audio, RTC, reminder engine, Pomodoro, BLE, Wi-Fi, OTA, scale, calibration, and consumption tracker
- Implements the main update loop with a repeat period around 20 ms
- Decides current system state using priority rules: calibration > OTA > active reminder > consumption result screen > Pomodoro > idle/home clock
- Handles diagnostics and system state transitions

### `main/acknowledgement_input.cpp` / `main/acknowledgement_input.hpp`
- Reads the shared IR input on a dedicated GPIO
- Detects acknowledgement gestures for active reminders
- Detects Pomodoro double-tap gestures when no reminder is active
- Provides a filtered dock state for bottle calibration and consumption tracking
- Uses time-based filtering to distinguish gestures from dock events

### `main/audio_manager.cpp` / `main/audio_manager.hpp`
- Manages the DFPlayer audio experience
- Plays announcements and then starts the appropriate background playlist
- Supports Pomodoro focus and break playlists, healing playlists, and meditation music
- Handles volume, night mode, track looping, and mode interruption/restoration
- Exposes functions for reminder announcements, Pomodoro mode, healing, and meditation

### `main/dfplayer.cpp` / `main/dfplayer.hpp`
- Configures UART and the DFPlayer control protocol
- Sends commands for play, stop, volume, and track control
- Reads the BUSY GPIO to detect active playback and completion
- Provides a low-level playback state interface used by the audio manager

### `main/display.cpp` / `main/display.hpp`
- Configures the GC9A01 circular TFT using LovyanGFX
- Renders the home clock, Pomodoro screens, reminder screens, hydration screens, calibration screens, and consumption result screens
- Uses fonts, sprites and image assets to draw clean UI screens
- Implements text wrapping and centering, including support for `font_regular` on medication and custom reminders

### `main/pomodoro.cpp` / `main/pomodoro.hpp`
- Implements Pomodoro runtime state machine and transitions
- Controls focus and break durations, cycle count, lap mode, overnight support, and auto-start behavior
- Integrates with the audio manager for announcements and music start/stop
- Coordinates display updates during Pomodoro execution

### `main/reminder_engine.cpp` / `main/reminder_engine.hpp` / `main/reminder_types.hpp`
- Implements reminder definitions, scheduling and priority rules
- Supports interval reminders, absolute-time reminders, medication reminders, and custom reminders
- Tracks active reminder state, acknowledgement, grace, pause and DND behavior
- Exposes callback hooks for reminder start and finish events

### `main/config_parser.cpp` / `main/config_parser.hpp`
- Parses incoming JSON from Bluetooth/OTA configuration
- Validates reminder, Pomodoro, healing, audio, and hydration configuration structures
- Applies safe defaults and clamps invalid values
- Supports the JSON schema used by the device configuration flow

### `main/configuration_storage.cpp` / `main/configuration_storage.hpp`
- Loads and saves runtime configuration using NVS
- Persists reminder settings, audio settings, Pomodoro settings, healing schedules, hydration goals, and calibration data
- Restores configuration on reboot

### `main/rtc_ds3231.cpp` / `main/rtc_ds3231.hpp`
- Initializes the DS3231 RTC over I2C
- Reads and writes clock time, and synchronizes the device with RTC time
- Provides time queries used by schedulers, reminders, and daily resets

### `main/hx711.cpp` / `main/hx711.hpp`
- Implements low-level HX711 communication using DOUT and SCK pins
- Reads signed 24-bit scale data with timing and settling control

### `main/scale.cpp` / `main/scale.hpp`
- Converts HX711 raw ADC values into grams using calibration offset and scale factor
- Exposes current weight and bottle detection calculations to higher-level modules

### `main/bottle_calibration.cpp` / `main/bottle_calibration.hpp`
- Implements a non-blocking bottle calibration state machine
- Learns tare offset, empty bottle weight, full bottle weight, and capacity
- Validates stable readings and saves calibration data to NVS
- Prevents consumption tracking from running while calibration is active

### `main/consumption_tracker.cpp` / `main/consumption_tracker.hpp`
- Tracks water consumption based on bottle removal and return events
- Uses a five-second settling delay and stable weight measurement before computing consumption
- Classifies events as consumed water, refill, or noise and updates daily totals
- Displays a separate consumption result screen after valid consumption
- Stores daily hydration totals and resets data when the RTC date changes

### `main/user_statistics.cpp` / `main/user_statistics.hpp`
- Tracks user actions and reminder acknowledgement/miss statistics
- Supports hydration, stretch, eye, walk, meditation, medication and custom reminder statistics
- Exposes BLE commands to request or reset today's statistics

### `main/statistics_history.cpp` / `main/statistics_history.hpp`
- Provides history and persistence support for statistics data
- Works with user statistics to store daily values and reset with date changes

### `main/bluetooth.cpp` / `main/bluetooth.hpp`
- Handles BLE connectivity and characteristic updates
- Receives JSON configuration strings and status commands
- Sends back responses and status results over BLE
- Supports configuration writes and statistics queries

### `main/wifi_manager.cpp` / `main/wifi_manager.hpp`
- Provides Wi-Fi helper functions for connectivity
- Supports network state management in the firmware

### `main/ota_manager.cpp` / `main/ota_manager.hpp`
- Manages over-the-air update state and progress
- Integrates with the main loop to keep OTA operations non-blocking

### `main/action_log.cpp` / `main/action_log.hpp`
- Records important actions and status changes for diagnostics and troubleshooting

### `main/images/`
- Stores display artwork, icons, and RGB565 image arrays used by the UI

## Implementation and runtime flow

### Boot flow
1. Initialize NVS and load persistent configuration.
2. Initialize scale and HX711 hardware.
3. Initialize bottle calibration and consumption tracker.
4. Initialize the display and render the welcome screen.
5. Initialize the audio manager and play the welcome announcement.
6. Initialize RTC and synchronize time.
7. Initialize the reminder engine, Pomodoro, Bluetooth, Wi-Fi and OTA modules.
8. Enter the repetitive main update loop.

### Main loop flow

The firmware loops through the following steps on every iteration:

1. Update shared IR state and filter the dock change.
2. Update bottle calibration state machine.
3. Enable or disable consumption tracking based on calibration activity.
4. Pass dock state into the consumption tracker.
5. Update the consumption tracker.
6. Update reminder engine and Pomodoro state.
7. Update the audio manager and DFPlayer state.
8. Update Bluetooth, Wi-Fi, RTC, OTA, and statistics modules.
9. Choose the highest-priority UI screen to render:
   - active reminder screen
   - consumption result screen
   - Pomodoro screen
   - home clock screen

### Shared IR input flow

A single IR sensor input is divided into three logical behaviors:

- Reminder acknowledgement when a reminder is active.
- Pomodoro double-tap gesture when no reminder is active.
- Docked/undocked state detection for bottle calibration and consumption tracking.

A time-based filter separates quick acknowledgement and tap gestures from dock state changes.

### Audio flow

Audio behavior is managed as two phases:

1. Announcement phase: play a short voice or prompt track.
2. Background phase: start a playlist after the announcement finishes.

The device uses the BUSY pin from the DFPlayer to detect when playback is active and when it completes. A safety timeout prevents permanent blocking in case of missing BUSY transitions.

### Bottle calibration and consumption flow

- Calibration is a non-blocking sequence that learns tare, empty bottle, full bottle, and capacity.
- Calibration uses stable weight sampling with trimmed averages and drift checks.
- During calibration, consumption tracking is disabled.
- Consumption tracking triggers on a `removed -> docked` transition and waits five seconds for the bottle to settle.
- After stable sampling, the firmware computes remaining water and classifies the change as consumption, refill, or noise.
- Valid consumption is added to todays total and shown on a result screen.
- Refill updates the baseline without counting as consumption.

### System state priorities

When deciding what the firmware should display and execute, the system follows this priority order:

1. Calibration
2. OTA
3. Active reminder
4. Consumption result screen
5. Pomodoro
6. Idle / home clock

## Configuration and persistence

- The device accepts JSON configuration over Bluetooth.
- The JSON parser validates reminders, Pomodoro settings, healing schedules, audio playlists, hydration goals, and more.
- Valid settings are stored in NVS and restored at boot.
- Daily hydration and statistics data is stored and reset by date.

## Existing design docs

The `main/` directory already contains detailed implementation notes for specific subsystems:

- `main/FROST_Firmware_README.txt` - overall firmware behavior and architecture
- `main/IR_Logic_README.txt` - shared IR input and healing logic
- `main/README_CALIBRATION_PHASE1.txt` - bottle calibration and consumption tracking
- `main/README_STATISTICS.txt` - user statistics and BLE stat protocol

## Build and flash

From the project root, use the ESP-IDF toolchain and run:

```sh
idf.py fullclean
idf.py build
idf.py flash
idf.py monitor
```

For normal development, the combined command is:

```sh
idf.py build flash monitor
```

Exit monitor with `Ctrl + ]`.

## Notes

- The current code base is organized as an ESP-IDF component.
- The `main/CMakeLists.txt` registers all source files and configures include paths.
- The device logic is intentional about separating gesture recognition, dock-state filtering, and stable sensor measurement to avoid false triggers.
- When modifying display or calibration code, the flow documented in `main/README_CALIBRATION_PHASE1.txt` should be followed.

## Contact

Refer to the in-source documentation and module README files for deeper subsystem details.
