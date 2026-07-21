#pragma once

#include <cstddef>
#include <ctime>

#include "reminder_types.hpp"

using ReminderTriggerCallback = void (*)(
    ReminderType type,
    int item_index,
    int schedule_index
);

using ReminderFinishedCallback = void (*)(
    ReminderType type,
    int item_index,
    int schedule_index
);

void reminder_engine_init();

void reminder_engine_set_trigger_callback(
    ReminderTriggerCallback callback
);

void reminder_engine_set_finished_callback(
    ReminderFinishedCallback callback
);

bool reminder_engine_set_config(
    ReminderType type,
    const ReminderConfig& config
);

const ReminderConfig* reminder_engine_get_config(
    ReminderType type
);

void reminder_engine_set_meditation_config(
    const MeditationConfig& config
);

const MeditationConfig*
reminder_engine_get_meditation_config();

void reminder_engine_set_medication_config(
    const MedicationConfig& config
);

const MedicationConfig*
reminder_engine_get_medication_config();

void reminder_engine_set_custom_config(
    const CustomReminderConfig& config
);

const CustomReminderConfig*
reminder_engine_get_custom_config();

/*
 * enabled = true:
 *   only medication reminders may become active; all other reminder
 *   schedules are still evaluated and remain queued.
 *
 * enabled = false:
 *   all reminder types may become active normally.
 */
void reminder_engine_set_medication_only_activation(
    bool enabled
);

void reminder_engine_update(
    std::time_t current_time
);

bool reminder_engine_has_active_reminder();

const ActiveReminder*
reminder_engine_get_active_reminder();

ReminderType reminder_engine_get_active_type();

void reminder_engine_acknowledge_active();

std::size_t reminder_engine_get_queue_count();

bool reminder_engine_is_ack_preview_active();

void reminder_engine_snooze_active();

void reminder_engine_cancel_active();