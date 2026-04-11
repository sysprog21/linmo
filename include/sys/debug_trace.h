#pragma once

#include <types.h>

typedef enum {
    EVENT_TASK_CREATE = 0,
    EVENT_TASK_DESTROY,
    EVENT_TASK_SWITCH,
    EVENT_TASK_SUSPEND,
    EVENT_TASK_RESUME,
    EVENT_TASK_DELAY,
    EVENT_TASK_YIELD,
    EVENT_SEM_WAIT,
    EVENT_SEM_POST,
    EVENT_MUTEX_LOCK,
    EVENT_MUTEX_UNLOCK,
    EVENT_PIPE_READ,
    EVENT_PIPE_WRITE,
    EVENT_MQUEUE_SEND,
    EVENT_MQUEUE_RECV,
    EVENT_ISR_ENTER,
    EVENT_ISR_EXIT,
    EVENT_EXCEPTION,
    EVENT_TYPE_MAX
} debug_event_type_t;

typedef struct {
    uint32_t timestamp;
    uint8_t event_type;
    uint8_t reserved;
    uint16_t task_id;
    uint32_t param1;
    uint32_t param2;
} debug_event_t;

#if CONFIG_DEBUG_TRACE
void debug_trace_event(uint8_t event_type, uint32_t param1, uint32_t param2);
void debug_dump_events(void);
void debug_clear_events(void);
uint32_t debug_trace_count(void);
uint32_t debug_trace_overwrites(void);
int32_t debug_trace_get(uint32_t index, debug_event_t *event);
#else
static inline void debug_trace_event(uint8_t event_type,
                                     uint32_t param1,
                                     uint32_t param2)
{
    (void) event_type;
    (void) param1;
    (void) param2;
}

static inline void debug_dump_events(void) {}

static inline void debug_clear_events(void) {}

static inline uint32_t debug_trace_count(void)
{
    return 0U;
}

static inline uint32_t debug_trace_overwrites(void)
{
    return 0U;
}

static inline int32_t debug_trace_get(uint32_t index, debug_event_t *event)
{
    (void) index;
    (void) event;
    return -1;
}
#endif
