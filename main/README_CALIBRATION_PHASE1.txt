FROST Bottle Calibration and Consumption Tracking

This document covers only the files and logic used for:

HX711 scale reading

bottle calibration

bottle removal/return detection through the shared IR sensor

water-consumption calculation

daily hydration-total storage

the separate five-second consumption display

The reminder engine, Pomodoro, healing, BLE configuration and audio features are outside the scope of this document.

1. Related files

File

Responsibility

hx711.hpp / hx711.cpp

Low-level HX711 communication using DOUT GPIO5 and SCK GPIO6. Reads signed 24-bit ADC values.

scale.hpp / scale.cpp

Converts HX711 raw values into grams using the saved tare offset and scale factor. Holds the active ScaleCalibration.

bottle_calibration.hpp / bottle_calibration.cpp

Runs the non-blocking empty/full bottle learning sequence, validates stable measurements, saves calibration to NVS and initializes the consumption baseline.

consumption_tracker.hpp / consumption_tracker.cpp

Detects bottle return, waits five seconds, obtains a stable weight, applies the Arduino consumption/refill logic, saves the daily total and controls the consumption-result screen timeout.

display.hpp / display.cpp

Provides the calibration wizard screens and the separate consumption screen.

main.cpp

Initializes all modules, filters the shared IR state, routes the dock state to calibration and tracking, disables tracking during calibration and applies display priority.

CMakeLists.txt

Adds the HX711, scale, calibration and consumption source files to the ESP-IDF component build.

2. Hardware assumptions

HX711

DOUT = GPIO5
SCK  = GPIO6

The current scale conversion factor is:

DEFAULT_SCALE_CALIB_G_PER_UNIT = 0.023125f;

The calibration process determines the raw zero offset. The conversion factor converts the raw offset-adjusted ADC value into grams.

Shared IR input

main.cpp converts the electrical IR level into a logical dock state:

true  = bottle docked / placed back
false = bottle removed

The exact electrical polarity is controlled by:

SHARED_IR_ACTIVE_LOW

The tracker never directly interprets the raw GPIO level. It receives only the filtered logical value through:

consumption_tracker_set_docked(dock_reported_state);

A 1200 ms stable-state filter is applied in main.cpp. This prevents short acknowledgement or Pomodoro gestures on the shared GPIO from being mistaken for a bottle removal or return.

3. Module dependency flow

Shared IR GPIO
    |
    v
main.cpp dock-state filter
    |
    +------------------------------+
    |                              |
    v                              v
bottle_calibration_set_docked()    consumption_tracker_set_docked()
    |                              |
    |                              +--> removed -> docked edge
    |                                   |
    |                                   v
    |                              5-second settling delay
    |                                   |
    |                                   v
    |                              stable scale measurement
    |                                   |
    |                                   v
    |                              consumption/refill calculation
    |                                   |
    |                                   v
    |                              NVS daily update
    |                                   |
    |                                   v
    |                              5-second result screen
    |
    v
scale.hpp / scale.cpp
    |
    v
hx711.hpp / hx711.cpp

Both calibration and consumption tracking use the same scale module. For this reason, consumption tracking is disabled while calibration is active.

4. Startup flow

The relevant initialization order in main.cpp is:

1. Initialize NVS
2. Initialize HX711/scale
3. Initialize bottle calibration
4. Load saved bottle calibration, when available
5. Initialize consumption tracker
6. Initialize display
7. Initialize and filter the shared IR dock state

Typical calls:

scale_init();
bottle_calibration_init();
consumption_tracker_init();

Saved calibration loading

bottle_calibration_init() calls the internal calibration loader. When valid calibration data exists, it:

restores the tare offset;

restores the grams-per-raw-unit factor;

restores empty bottle weight;

restores full bottle weight;

restores calculated capacity;

passes the capacity to the consumption tracker as its initial remaining-water baseline.

consumption_tracker_set_initial_remaining(loaded.capacity_ml);

Consumption tracker initialization

consumption_tracker_init():

loads the saved daily total and daily goal;

resets the daily total if the saved date is not today;

initializes the remaining-water baseline from the learned bottle capacity when calibration is valid;

enables the non-blocking tracking state machine.

5. Bottle calibration flow

Calibration starts through:

bottle_calibration_start();

It is a non-blocking state machine. bottle_calibration_update() must be called repeatedly from the main loop.

Calibration states

IDLE
  |
  v
WAIT_REMOVE_FOR_TARE
  |
  | bottle removed
  v
TARING
  |
  | stable raw reading accepted
  v
WAIT_EMPTY_BOTTLE
  |
  | empty bottle docked
  v
MEASURE_EMPTY
  |
  | stable empty-bottle weight accepted
  v
WAIT_REMOVE_TO_FILL
  |
  | bottle removed
  v
WAIT_FULL_BOTTLE
  |
  | full bottle docked
  v
MEASURE_FULL
  |
  | stable full-bottle weight accepted
  v
COMPLETE

Error and cancellation paths:

Any measurement failure or invalid value -> ERROR
BLE/user cancellation                    -> CANCELLED

The final COMPLETE, ERROR or CANCELLED screen remains visible for three seconds before the module returns to IDLE.

Stable-reading method

The calibration module does not wait for an absolutely unchanging ADC value. Instead it accepts a controlled stable window:

Sample interval       = 80 ms
Window size           = 20 samples
Trim                   = lowest 10% and highest 10%
Maximum trimmed range = 8 g
Maximum mean drift    = 2.5 g
Required stable time  = 1400 ms
Per-step timeout      = 30 seconds

The algorithm is:

1. Collect 20 readings.
2. Sort the readings.
3. Remove the lowest 10% and highest 10%.
4. Calculate the trimmed mean.
5. Calculate the trimmed range.
6. Compare the current mean with the previous mean.
7. Require both range and drift to remain within tolerance for 1400 ms.
8. Accept the trimmed mean as the stable value.

Calibration calculations

Tare

The empty dock raw ADC value becomes:

offset_raw = stable_raw;

Empty bottle

The stable bottle-only reading becomes:

empty_bottle_g = stable_empty_weight_g;

Minimum accepted empty bottle weight:

5 g

Full bottle and capacity

capacity_ml = full_bottle_g - empty_bottle_g;

The implementation assumes approximately:

1 gram of water = 1 millilitre

Minimum accepted capacity:

50 ml

After successful full-bottle calibration:

working_calibration.valid = true;
scale_set_calibration(working_calibration);
consumption_tracker_set_initial_remaining(capacity);

This last call is important. It makes the first drink after calibration compare against the learned full capacity.

Calibration NVS data

Namespace:

bottle_cal

Key:

values

Stored fields:

version
offset_raw
grams_per_raw_unit
empty_bottle_g
full_bottle_g
capacity_ml

6. Consumption tracking flow

Consumption tracking is independent of hydration reminders. Any normal bottle removal and return can produce a consumption event.

Trigger condition

The tracker acts only on a complete logical transition:

Bottle removed  -> docked=false
Bottle returned -> docked=true

The actual calculation starts only on:

false -> true

That means bottle returned to the dock.

Tracker states

IDLE
  |
  | removed -> docked transition
  v
SETTLING
  |
  | mandatory 5 seconds completed
  v
MEASURING
  |
  | stable HX711 weight accepted
  v
IDLE

If the bottle is removed again while settling or measuring, the pending measurement is cancelled and the tracker returns to IDLE.

Mandatory settling delay

After the bottle is placed back:

BOTTLE_SETTLING_DELAY_MS = 5000;

This delay lets the bottle, platform and load cell settle mechanically before stable sampling begins.

Stable consumption measurement

After the five-second delay, the tracker uses:

Sample interval       = 80 ms
Samples               = 20
Maximum trimmed range = 8 g
Maximum mean drift    = 2.5 g
Required stable time  = 1400 ms
Measurement timeout   = 15 seconds

This is non-blocking. consumption_tracker_update() continues checking progress during each main-loop iteration.

7. Arduino-style consumption calculation

After a stable total bottle weight is measured:

water_ml = total_grams - empty_bottle_g;

The value is limited to:

0.0f ... capacity_ml + 20.0f

The extra 20 ml is a noise allowance around the calibrated full capacity.

The change is calculated as:

consumed_ml = last_remaining_ml - water_ml;

Classification rules

Valid consumption

consumed_ml >= 10.0f && consumed_ml <= capacity_ml

Actions:

1. Round consumed amount to the nearest millilitre.
2. Save it as the last consumed amount.
3. Add it to today's total.
4. Save today's total to NVS.
5. Trigger the separate consumption screen.

Refill

consumed_ml < -20.0f

A negative change means the new remaining amount is larger than the previous amount. The tracker treats this as a refill.

Actions:

1. Do not add anything to daily consumption.
2. Do not show the consumption screen.
3. Update the remaining-water baseline.

Small or insignificant change

Changes that are neither valid consumption nor refill are ignored.

Example:

Small load-cell movement
Bottle repositioning
Minor evaporation or vibration
Sensor noise below the threshold

The result screen is not shown.

Baseline update

After every accepted measurement classification, the Arduino-style behavior is preserved:

last_remaining_ml = water_ml;

If no valid baseline exists, the first accepted measurement only establishes the baseline and does not count consumption.

8. Daily hydration storage

Namespace:

hydration

Key:

daily

Stored fields:

version
yyyymmdd
consumed_ml
goal_ml

Default daily goal:

3000 ml

At startup:

the saved goal is restored;

the saved consumed amount is restored only when its date matches today;

otherwise the daily consumed amount is reset to zero.

During runtime, the tracker also checks for a date change and resets:

daily_consumed_ml = 0;
last_consumed_ml = 0;

The date reset requires valid system/RTC time. A year earlier than 2025 is treated as invalid time and does not force a date-based reset.

9. Separate consumption screen

The screen is independent of the hydration-reminder screen.

It is shown only after valid consumption is detected.

Duration:

CONSUMPTION_SCREEN_DURATION_MS = 5000;

Displayed data:

Consumed
<last consumed ml>

Today
<daily consumed ml> / <daily goal ml>

Example:

Consumed
245 ml

Today
820 / 3000 ml

Firmware API:

display_show_consumption_screen(
    last_consumed_ml,
    daily_consumed_ml,
    daily_goal_ml
);

At present, display.cpp uses the existing hydration artwork as the background. A dedicated 240x240 background can replace it later without changing the tracking or calculation logic.

10. Display priority

The relevant display priority in main.cpp is:

1. Active reminder
2. Consumption result screen
3. Pomodoro screen
4. Home clock

Therefore:

an active reminder remains above the consumption screen;

after the reminder clears, the still-active consumption screen can render until its five-second timeout expires;

otherwise the device returns to Pomodoro or the home clock.

Calibration is different: calibration owns the full display and suspends the normal display flow until calibration finishes.

11. Calibration and consumption interaction

Calibration and consumption tracking must not run at the same time because both use bottle transitions and HX711 readings.

main.cpp enforces this:

const bool calibration_active = bottle_calibration_is_active();
consumption_tracker_set_enabled(!calibration_active);

While calibration is active:

Consumption state is reset.
Pending measurements are cancelled.
The consumption result screen is cleared.
Dock-state synchronization is reset.

When calibration finishes, the tracker re-synchronizes with the current filtered dock state before acting on a future transition. This prevents the calibration bottle placements from being counted as drinks.

12. Main-loop integration

The relevant repeated order is:

update_shared_ir_dock_state();

bottle_calibration_update();

const bool calibration_active = bottle_calibration_is_active();
consumption_tracker_set_enabled(!calibration_active);
consumption_tracker_set_docked(dock_reported_state);
consumption_tracker_update();

Then display selection checks:

if (!reminder_engine_has_active_reminder())
{
    if (consumption_tracker_screen_active())
    {
        consumption_tracker_render_screen();
    }
    else if (pomodoro_is_running())
    {
        pomodoro_render_if_needed();
    }
    else
    {
        display_show_home_clock(now);
    }
}

Recommended loop period:

20 ms

The calibration and consumption modules are state machines, so they must be updated frequently and must not be replaced with long blocking delays.

13. Public APIs

Calibration

esp_err_t bottle_calibration_init();
bool bottle_calibration_start();
void bottle_calibration_cancel();
void bottle_calibration_set_docked(bool docked);
void bottle_calibration_update();
bool bottle_calibration_is_active();
BottleCalibrationState bottle_calibration_get_state();
void bottle_calibration_get_status(char* destination, std::size_t size);

Consumption tracker

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

Display functions used by these modules

void display_show_calibration_remove_bottle();
void display_show_calibration_measuring(const char* title, const char* subtitle);
void display_show_calibration_place_empty();
void display_show_calibration_fill_bottle();
void display_show_calibration_place_full();
void display_show_calibration_complete(float capacity_ml);
void display_show_calibration_error(const char* message);

void display_show_consumption_screen(
    uint32_t consumed_ml,
    uint32_t daily_consumed_ml,
    uint32_t daily_goal_ml
);

14. End-to-end runtime examples

Normal drink

Bottle docked with 900 ml remaining
        |
Bottle removed
        |
User drinks water
        |
Bottle returned
        |
IR filtered state changes false -> true
        |
Wait 5 seconds
        |
Collect stable total weight
        |
Calculated remaining = 650 ml
        |
Consumed = 900 - 650 = 250 ml
        |
Daily total increases by 250 ml
        |
Save daily value to NVS
        |
Show consumption screen for 5 seconds
        |
Return to reminder, Pomodoro or home display priority

Refill

Previous remaining = 300 ml
        |
Bottle removed and filled
        |
Bottle returned
        |
Wait 5 seconds and measure
        |
New remaining = 950 ml
        |
Change = 300 - 950 = -650 ml
        |
Refill detected
        |
Daily total unchanged
        |
No consumption screen
        |
Baseline becomes 950 ml

No meaningful change

Previous remaining = 700 ml
New remaining      = 695 ml
Difference         = 5 ml

Since 5 ml is below the 10 ml consumption threshold:

No daily-total update
No result screen
Baseline updated to the new measured remaining value

15. Build integration

The component build must include:

"hx711.cpp"
"scale.cpp"
"bottle_calibration.cpp"
"consumption_tracker.cpp"

Required ESP-IDF components include:

driver
esp_timer
nvs_flash

The complete build entry is already present in the supplied CMakeLists.txt.

16. Important implementation notes

Do not connect consumption calculation only to hydration reminders. It is intentionally triggered by bottle removal and return.

Keep the mandatory five-second settling delay after bottle return.

Keep the result screen visible for five seconds.

Do not count refill as consumption.

Do not show the result screen for refill or insignificant changes.

Keep consumption tracking disabled during calibration.

Always feed the tracker the filtered logical dock state, not the raw GPIO level.

Keep calibration and tracking non-blocking by calling their update functions repeatedly.

A valid calibration is required before meaningful consumption calculation can occur.

Replace the temporary hydration background only in display.cpp; no calculation changes are required.