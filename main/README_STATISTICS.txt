FROST USER STATISTICS IMPLEMENTATION
====================================

FILES ADDED
-----------
- user_statistics.hpp
- user_statistics.cpp

FILES MODIFIED
--------------
- reminder_engine.cpp
- consumption_tracker.cpp
- bluetooth.cpp
- CMakeLists.txt

TRACKED
-------
- Hydration: acknowledged, missed, consumed ml
- Stretch: acknowledged, missed
- Eye: acknowledged, missed
- Walk: acknowledged, missed
- Meditation: acknowledged, missed
- Medication per string token/id: acknowledged, missed, snoozed,
  last acknowledgement Unix timestamp, last miss Unix timestamp
- Custom reminder per string token/id: acknowledged, missed,
  last acknowledgement Unix timestamp, last miss Unix timestamp

NOT TRACKED
-----------
- Pomodoro

MISS RULE
---------
A reminder is counted as missed only when:
- require_ack is true, and
- display_ms expires before acknowledgement.

Queue-preview reminders are not counted as missed.

BLE COMMANDS
------------
1. Write STATS:GET (or STATS:TODAY)
2. Read characteristic -> STATS_BEGIN:v1
3. Repeatedly write STATS:NEXT and read the characteristic
4. Stop after STATS_END

Reset today's statistics:
- Write STATS:RESET
- Read -> STATS_RESET_OK

Example stream:
STATS_BEGIN:v1
DAY:20260724
HYD:ML=750,ACK=3,MISS=1
STR:ACK=2,MISS=0
EYE:ACK=1,MISS=1
WALK:ACK=1,MISS=0
MEDIT:ACK=1,MISS=0
MED:medicine_01,A=2,M=1,S=1,LA=1784870000,LM=1784860000
CUSTOM:custom_03,A=1,M=0,LA=1784871000,LM=0
STATS_END

INSTALLATION
------------
Replace the modified files in main/ and add the two new statistics files.
The supplied CMakeLists.txt already includes user_statistics.cpp.

NVS
---
Namespace: statistics
Key: today_v1
Only today's data is stored in this first implementation. It resets automatically
when the RTC/system date changes.