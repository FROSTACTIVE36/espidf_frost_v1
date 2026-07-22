FROST ESP32-S3 REMINDER DEVICE
IMPLEMENTATION README
========================================

Purpose
-------
This document summarizes the firmware features implemented so far, the responsibility of each source file, important runtime flows, audio behavior, display behavior, and the module/class diagrams.

Target
------
MCU: ESP32-S3-N16R8
Framework: ESP-IDF
Display: GC9A01 240x240 circular TFT using LovyanGFX
Audio: DFPlayer Mini using UART1 and BUSY feedback
RTC: DS3231 over I2C
Scale: HX711 load cell interface

Main pins
---------
Display: SCLK=12, MOSI=11, DC=13, CS=10, RST=4
RTC: SDA=8, SCL=9
HX711: DOUT=5, SCK=6
DFPlayer: ESP32 TX1=17, RX1=18, BUSY=14

============================================================
1. IMPLEMENTED FEATURES
============================================================

Reminder engine
---------------
- Interval reminders
- Absolute-time reminders
- Multiple reminder times
- Day masks and fired-date tracking
- Reminder priorities
- Active reminder tracking
- Reminder start and finish callbacks
- Grace, pause and DND support
- Hydration, eye, stretch, walk, cleaning, medication, meditation, healing and custom reminders

Pomodoro
--------
- Focus and break stages
- Configurable focus/break minutes
- Configurable cycle count
- Auto-start focus and break
- Manual start, stop and toggle
- Lap mode with multiple time windows
- Overnight lap support
- Auto-start inside a lap and stop outside it
- Progressive display arc
- Remaining-time rendering
- Redraw caching
- Wait for active reminder before leaving a completed break
- Focus announcement before focus music
- Break announcement before break music
- Stop audio when Pomodoro stops

Healing
-------
- Multiple schedules
- Start/end times
- Overnight schedule handling
- Healing announcement first
- Healing background playlist after announcement
- Stop or restore background when healing ends

Meditation
----------
- Meditation background mode
- Continuous meditation music
- Meditation playlist
- Interruption of another background mode
- Restoration of previous background after meditation

Audio manager
-------------
- DFPlayer initialization
- Configurable volume
- Welcome announcement
- Reminder announcements
- Pomodoro, healing and meditation background modes
- Playlist looping
- Background interruption/restoration
- BUSY-pin playback detection
- Announcement start detection
- Announcement completion detection
- Safety timeout
- JSON-configurable playlists
- Night-mode volume behavior

Display
-------
- GC9A01 circular display
- LovyanGFX device configuration
- PSRAM sprite rendering
- Home clock
- Pomodoro focus/break screens
- Reminder screens
- Medication and custom reminder text
- Text wrapping and centering
- Font-height-based line spacing
- Progressive Pomodoro arc
- Redraw optimization
- Custom reminder uses font_regular
- Medication reminder also uses font_regular

BLE/JSON
--------
- BLE configuration input
- JSON parsing
- Runtime configuration updates
- Reminder, Pomodoro, audio, healing and lap configuration
- Persistent configuration storage

NVS
---
- Reminder settings
- Audio settings
- Pomodoro settings
- Healing settings
- Hydration counters and goal
- Scale calibration and offset
- Bottle empty/full/capacity values

Hydration/scale
---------------
- HX711 integration
- Calibration and offset
- Bottle presence detection
- Daily water tracking
- Goal percentage
- Keep-last-valid-state behavior when HX711 is not ready

RTC/time
--------
- DS3231 initialization
- Clock update
- Absolute scheduling
- Lap scheduling
- Healing scheduling
- Overnight time windows

============================================================
2. AUDIO BEHAVIOR
============================================================

Expected BUSY logic:
- LOW  = DFPlayer is playing
- HIGH = DFPlayer is idle or playback has completed

Announcement-to-background sequence:
1. Stop current background if required
2. Send announcement track command
3. Wait for BUSY to become LOW
4. Continue while BUSY stays LOW
5. Detect BUSY returning HIGH
6. Start the selected background playlist
7. Use timeout fallback if BUSY never changes

Typical current announcement mapping:
- 18: Healing announcement
- 20: Pomodoro focus announcement
- 21: Pomodoro break announcement
- 22: Meditation-related track, depending on final configuration

Recommended separate background tracks:
- Pomodoro: 50, 51, 52
- Healing: 60, 61, 62
- Meditation: 70 or another dedicated playlist

Do not use the same MP3 number as both an announcement and background track unless intentional.


============================================================
3. MODULE / CLASS DIAGRAM
============================================================

High-level architecture
-----------------------

+----------------------+
|       app_main       |
|       main.cpp       |
+----------+-----------+
           |
           v
+----------+-----------+       +-----------------------+
|   Reminder Engine    |------>|       Display         |
| reminder_engine.cpp  |       | display.cpp/.hpp      |
+----------+-----------+       +-----------------------+
           |
           v
+----------+-----------+       +-----------------------+
|    Audio Manager     |------>|      DFPlayer         |
| audio_manager.cpp    |       | dfplayer.cpp/.hpp     |
+----------+-----------+       +-----------+-----------+
           |                               |
           v                               v
+----------------------+       +-----------------------+
| Pomodoro Controller  |       | DFPlayer Mini module  |
| pomodoro.cpp/.hpp    |       | UART + BUSY GPIO14    |
+----------+-----------+       +-----------------------+
           |
           v
+----------------------+
| Pomodoro UI screens  |
+----------------------+

Configuration architecture
--------------------------

+-------------------+       +----------------------+
| BLE Configuration |------>| JSON parser/config   |
+-------------------+       +----------+-----------+
                                       |
                                       v
                         +-------------+-------------+
                         | Runtime module settings   |
                         +-------------+-------------+
                                       |
                                       v
                         +---------------------------+
                         | NVS persistent storage    |
                         +---------------------------+

Hardware architecture
---------------------

+-------------------+       +----------------------+
| DS3231 RTC        |------>| Scheduler logic      |
+-------------------+       +----------------------+

+-------------------+       +----------------------+
| HX711 Load Cell   |------>| Hydration logic      |
+-------------------+       +----------------------+

Pomodoro state diagram
----------------------

STOPPED
   |
   | pomodoro_start()
   v
 FOCUS
   |
   | focus complete
   v
 BREAK
   |
   | break complete
   v
 FOCUS

Any state -> STOPPED when:
- manual stop
- lap ends
- configuration disables Pomodoro
- invalid state is detected

Audio state diagram
-------------------

NONE
  |
  v
ANNOUNCEMENT_PENDING
(wait for BUSY LOW and then HIGH)
  |
  v
BACKGROUND_PLAYING
  |
  v
INTERRUPTED / PREVIOUS MODE SAVED
  |
  v
RESTORED BACKGROUND


============================================================
4. FILE-BY-FILE RESPONSIBILITIES
============================================================

main.cpp
--------
- app_main entry point
- Initializes modules
- Initializes audio before welcome playback
- Registers reminder callbacks
- Runs reminder, Pomodoro, audio, display, RTC, BLE and scale updates
- Restores audio after reminder completion

pomodoro.cpp
------------
- Pomodoro runtime state
- Focus/break transitions
- Cycle count
- Stage timing
- Lap detection
- Overnight lap support
- Auto-start and auto-stop
- Display update control
- Calls audio_manager_start_pomodoro_focus()
- Calls audio_manager_start_pomodoro_break()
- Calls audio_manager_stop_pomodoro()

pomodoro.hpp
------------
- PomodoroState
- PomodoroConfig
- PomodoroLap
- Public Pomodoro API

audio_manager.cpp
-----------------
- Announcement mapping
- Background-mode state
- Pomodoro playlist
- Healing playlist
- Meditation playlist
- Healing schedule handling
- Playlist looping
- BUSY-based announcement completion
- Safety timeout
- Previous-background restoration
- Welcome audio
- Volume and night mode

audio_manager.hpp
-----------------
Must declare:
- audio_manager_init()
- audio_manager_update()
- audio_manager_play_welcome()
- audio_manager_play_reminder()
- audio_manager_start_pomodoro()
- audio_manager_start_pomodoro_focus()
- audio_manager_start_pomodoro_break()
- audio_manager_stop_pomodoro()
- audio_manager_start_healing()
- audio_manager_stop_healing()
- audio_manager_start_meditation()
- audio_manager_stop_meditation()

dfplayer.cpp / dfplayer.hpp
---------------------------
- UART setup
- DFPlayer command frames
- Checksums
- Play/stop/volume commands
- BUSY GPIO setup
- dfplayer_is_playing()
- Playback state update

display.cpp
-----------
- LovyanGFX hardware class
- GC9A01 setup
- Sprite rendering
- Home screen
- Pomodoro focus/break screens
- Reminder screens
- Text wrapping and centering
- Medication label with font_regular
- Custom label with font_regular
- Progressive arc

display.hpp
-----------
- Public display functions

font_regular.h
--------------
- Embedded font used by medication and custom reminders

reminder_engine.cpp / reminder_engine.hpp
-----------------------------------------
- Reminder definitions
- Interval and absolute scheduling
- Priority handling
- Active reminder state
- Start/finish callbacks
- DND, grace and pause behavior
- Medication and custom reminder configuration

BLE files
---------
- Receive BLE JSON
- Validate input
- Apply configuration
- Return status

JSON/config files
-----------------
- Parse reminders
- Parse Pomodoro and laps
- Parse healing schedules
- Parse audio playlists
- Clamp invalid values
- Apply defaults
- Save valid settings

NVS/storage files
-----------------
- Save and restore configuration and counters

RTC files
---------
- DS3231 I2C setup
- Read/set time
- Synchronize system time

HX711/scale files
-----------------
- Raw reading
- Calibration
- Weight conversion
- Bottle detection
- Water estimation

images/
-------
- Reminder images
- Icons
- Frost logo
- Font headers
- RGB565 image arrays

main/CMakeLists.txt
-------------------
- Lists all source files and include paths

Root CMakeLists.txt
------------------
- ESP-IDF project entry

sdkconfig
---------
- PSRAM, flash, Bluetooth, NVS, UART, I2C and logging options


============================================================
5. IMPORTANT RUNTIME FLOWS
============================================================

Boot flow
---------
1. Initialize NVS
2. Load saved configuration
3. Initialize display
4. Initialize audio manager
5. Play welcome announcement
6. Initialize RTC
7. Initialize reminder engine
8. Initialize Pomodoro
9. Initialize BLE
10. Initialize scale
11. Enter main update loop

Pomodoro focus flow
-------------------
1. Enter FOCUS
2. Initialize timer
3. Play focus announcement
4. Wait for BUSY LOW
5. Wait for BUSY HIGH
6. Start Pomodoro background music
7. Update timer and display
8. At completion, transition to break or stop

Pomodoro break flow
-------------------
1. Stop current Pomodoro music
2. Enter BREAK
3. Play break announcement
4. Wait for BUSY LOW then HIGH
5. Start break background playlist
6. At completion, return to focus or stop

Healing flow
------------
1. RTC enters healing schedule
2. Play healing announcement
3. Wait for BUSY LOW then HIGH
4. Start healing playlist
5. Loop until schedule ends
6. Stop or restore previous mode

Medication flow
---------------
1. Medication time is detected
2. Reminder becomes active
3. Announcement plays
4. Medication screen appears
5. Label uses font_regular
6. Wrapped text is centered
7. Reminder completes
8. Previous audio resumes if required

Custom reminder flow
--------------------
1. Custom time is detected
2. Reminder becomes active
3. Custom screen appears
4. Label uses font_regular
5. Text wraps using actual font height
6. Reminder completes
7. Previous audio resumes if required


============================================================
6. EXAMPLE AUDIO/POMODORO JSON
============================================================

{
  "audio": {
    "volume": 20,
    "pomodoro": {
      "enabled": true,
      "tracks": [50, 51, 52]
    },
    "healing": {
      "enabled": true,
      "tracks": [60, 61, 62]
    },
    "meditation": {
      "enabled": true,
      "tracks": [70]
    },
    "healing_schedules": [
      {
        "enabled": true,
        "start_time": "18:45",
        "end_time": "18:55"
      }
    ]
  },
  "pomodoro": {
    "enabled": true,
    "focus_min": 25,
    "break_min": 5,
    "cycles": 4,
    "auto_start_break": true,
    "auto_start_focus": true,
    "lap_mode_enabled": false
  }
}

Use the exact field names expected by the current parser.


============================================================
7. BUILD AND FLASH
============================================================

idf.py fullclean
idf.py build
idf.py flash
idf.py monitor

Combined:
idf.py build flash monitor

Exit monitor:
Ctrl + ]

After replacing core files, run fullclean before building.


============================================================
8. TEST CHECKLIST
============================================================

Display
-------
[ ] Home screen renders correctly
[ ] Custom reminder uses font_regular
[ ] Medication reminder uses font_regular
[ ] Long text wraps correctly
[ ] Pomodoro arc updates correctly
[ ] Colors display correctly

Pomodoro
--------
[ ] Manual start/stop works
[ ] Focus announcement plays
[ ] Music begins only after focus announcement
[ ] Break announcement plays
[ ] Music begins only after break announcement
[ ] Cycle count is correct
[ ] Auto-start flags work
[ ] Lap mode starts/stops correctly
[ ] Overnight lap works

Healing
-------
[ ] Schedule starts and ends correctly
[ ] Announcement plays first
[ ] Music starts after BUSY HIGH
[ ] Playlist loops
[ ] Overnight schedule works

Meditation
----------
[ ] Continuous music starts
[ ] Meditation stops correctly
[ ] Previous background restores

BLE/JSON
--------
[ ] BLE connects
[ ] Valid JSON applies
[ ] Invalid JSON is rejected safely
[ ] Settings persist after reboot

Hardware
--------
[ ] DFPlayer UART works
[ ] BUSY LOW while playing
[ ] BUSY HIGH when finished
[ ] RTC is correct
[ ] HX711 is stable
[ ] Display sprite allocation succeeds


============================================================
9. IMPORTANT INTEGRATION RULES
============================================================

1. audio_manager_init() must run before audio_manager_play_welcome().
2. pomodoro.cpp must include audio_manager.hpp.
3. audio_manager.hpp must declare focus, break and stop functions.
4. audio_manager.cpp must implement those functions.
5. display.cpp must include images/font_regular.h.
6. Medication and custom screens must load and unload font_regular.
7. BUSY must be wired to GPIO14 with common ground.
8. Announcement and background tracks should use different numbers.
9. audio_manager_update() must run frequently.
10. pomodoro_update() must continue while audio is active.
11. A 10-second BUSY timeout prevents permanent blocking.
12. The final pin configuration in source code is the source of truth.

============================================================
10. SUMMARY
============================================================

The firmware currently includes:
- Modular ESP32-S3 architecture
- Interval and absolute reminders
- Medication and custom reminders using font_regular
- Pomodoro focus/break management
- Scheduled lap mode
- Healing schedules
- Meditation music mode
- DFPlayer announcements
- BUSY-controlled announcement-to-music transitions
- Background playlists and restoration
- Circular display UI
- RTC scheduling
- HX711 hydration tracking
- BLE JSON configuration
- NVS persistence

End of README