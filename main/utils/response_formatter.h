/**
 * ===========================================================================
 * @file response_formatter.h
 * @brief Format response tiếng Việt tự nhiên cho Telegram
 * ===========================================================================
 */

#ifndef RESPONSE_FORMATTER_H
#define RESPONSE_FORMATTER_H

#include "task_database.h"
#include <stddef.h>

char *format_task_created(const task_record_t *task, char *buffer, size_t buffer_size);

char *format_task_list(const task_index_entry_t **matches, int count, const char *label,
                       char *buffer, size_t buffer_size);

char *format_task_list_short(const task_index_entry_t **matches, int count, const char *label,
                             char *buffer, size_t buffer_size);

char *format_task_count(int count, const char *label,
                        char *buffer, size_t buffer_size);

char *format_task_completed(const task_record_t *task, char *buffer, size_t buffer_size);

char *format_task_deleted(const task_record_t *task, char *buffer, size_t buffer_size);

char *format_task_updated(const task_record_t *task, const char *changes,
                          char *buffer, size_t buffer_size);

char *format_reminder(const task_record_t *task, char *buffer, size_t buffer_size);

char *format_not_found(const char *context, char *buffer, size_t buffer_size);

char *format_error(const char *error_msg, char *buffer, size_t buffer_size);

#endif /* RESPONSE_FORMATTER_H */
