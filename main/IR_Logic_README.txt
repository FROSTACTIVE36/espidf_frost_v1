FROST Firmware – Shared IR Sensor Logic

Overview

The firmware intentionally uses one IR sensor (GPIO7) for three
independent features:

1.  Reminder acknowledgement
2.  Pomodoro double-tap gesture
3.  Healing dock detection

Instead of dedicating separate sensors, the firmware separates these
behaviors using time-based filtering.

------------------------------------------------------------------------

Hardware

    IR Sensor (GPIO7)
            │
            ├────────► Acknowledgement Input
            ├────────► Pomodoro Double Tap
            └────────► Healing Dock Detection

The GPIO is configured once by the acknowledgement input module and is
read by the healing logic.

------------------------------------------------------------------------

Acknowledgement

When a reminder is active:

-   First IR trigger
-   Reminder is acknowledged immediately
-   No dock-state change occurs

Priority:

    Reminder Active
          │
    IR Trigger
          │
    Acknowledge Reminder

------------------------------------------------------------------------

Pomodoro Gesture

When no reminder is active:

-   First IR trigger starts a timer.
-   Second trigger between 120 ms and 800 ms toggles Pomodoro.
-   Earlier than 120 ms is treated as bounce.
-   After 800 ms the first tap expires.

    Tap #1
       │
    120–800 ms
       │
    Tap #2
       │
    Toggle Pomodoro

------------------------------------------------------------------------

Healing Dock Detection

Healing must not react to quick hand movements.

Therefore a separate stability filter is used.

A dock state is accepted only if it remains unchanged for 1200 ms.

    IR changes
         │
    Start timer
         │
    Stable for 1200 ms?
         │
       Yes
         │
    Update Dock State

This prevents:

-   acknowledgement gestures
-   Pomodoro double taps
-   accidental hand movements

from pausing or resuming healing.

------------------------------------------------------------------------

Healing Conditions

Healing starts only when ALL conditions are true:

-   Healing enabled
-   Current schedule enabled
-   Current weekday allowed
-   Current time inside schedule
-   require_dock satisfied

    Healing Enabled
            │
    Correct Day
            │
    Correct Time
            │
    require_dock ?
            │
          Docked
            │
    Announcement
            │
    Background Music

------------------------------------------------------------------------

require_dock = false

    Schedule Active
          │
    Announcement
          │
    Background Music

The IR sensor is ignored.

------------------------------------------------------------------------

require_dock = true

    Schedule Active
          │
    Docked?
     │          │
    No         Yes
     │          │
    Wait    Announcement
                 │
          Background Music

------------------------------------------------------------------------

During Healing

If the bottle/device is removed:

    Undocked
         │
    Pause DFPlayer

When docked again:

    Docked
        │
    Resume DFPlayer

Playback resumes from the paused position.

------------------------------------------------------------------------

Day Gate

Each schedule may specify:

    "days":["mon","tue","wed"]

Missing or empty days means every day.

Overnight schedules (e.g. 22:00–06:00) continue using the starting day’s
permission after midnight.

------------------------------------------------------------------------

Why a 1200 ms Filter?

Current gesture timings:

-   Debounce: 50 ms
-   Double tap valid: 120–800 ms

Choosing 1200 ms guarantees that all gestures finish before the dock
logic accepts a new state.

------------------------------------------------------------------------

Overall Flow

    GPIO7
     │
     ▼
    Acknowledgement Input
     │
     ├────────► Reminder Active?
     │              │
     │             Yes
     │              │
     │      Acknowledge Reminder
     │
     ▼
    Double Tap Detector
     │
     ├────────► Valid Double Tap?
     │              │
     │             Yes
     │              │
     │      Toggle Pomodoro
     │
     ▼
    1200 ms Stability Filter
     │
     ▼
    Dock State
     │
     ▼
    Healing Scheduler
     │
     ▼
    Announcement
     │
     ▼
    Healing Playlist

------------------------------------------------------------------------

Advantages

-   One IR sensor performs three jobs.
-   No accidental healing pauses.
-   No extra hardware required.
-   Deterministic behavior.
-   Easy to extend in future.