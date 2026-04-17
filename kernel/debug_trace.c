#include <lib/libc.h>
#include <sys/debug_trace.h>
#include <sys/task.h>

#if CONFIG_DEBUG_TRACE

static debug_event_t event_buffer[DEBUG_EVENT_BUFFER_SIZE];
static uint32_t event_write_index;
static uint32_t event_count;
static uint32_t event_overwrites;

typedef struct {
    int32_t saved_mie;
} debug_trace_lock_t;

static inline debug_trace_lock_t debug_trace_enter(void)
{
    debug_trace_lock_t lock = {.saved_mie = _di()};

    return lock;
}

static inline void debug_trace_leave(debug_trace_lock_t lock)
{
    hal_interrupt_set(lock.saved_mie);
}

static const char *debug_trace_event_name(uint8_t event_type)
{
    static const char *const event_names[EVENT_TYPE_MAX] = {
        [EVENT_TASK_CREATE] = "TASK_CREATE",
        [EVENT_TASK_DESTROY] = "TASK_DESTROY",
        [EVENT_TASK_SWITCH] = "TASK_SWITCH",
        [EVENT_TASK_SUSPEND] = "TASK_SUSPEND",
        [EVENT_TASK_RESUME] = "TASK_RESUME",
        [EVENT_TASK_DELAY] = "TASK_DELAY",
        [EVENT_TASK_YIELD] = "TASK_YIELD",
        [EVENT_SEM_WAIT] = "SEM_WAIT",
        [EVENT_SEM_POST] = "SEM_POST",
        [EVENT_MUTEX_LOCK] = "MUTEX_LOCK",
        [EVENT_MUTEX_UNLOCK] = "MUTEX_UNLOCK",
        [EVENT_PIPE_READ] = "PIPE_READ",
        [EVENT_PIPE_WRITE] = "PIPE_WRITE",
        [EVENT_MQUEUE_SEND] = "MQUEUE_SEND",
        [EVENT_MQUEUE_RECV] = "MQUEUE_RECV",
        [EVENT_ISR_ENTER] = "ISR_ENTER",
        [EVENT_ISR_EXIT] = "ISR_EXIT",
        [EVENT_EXCEPTION] = "EXCEPTION",
    };

    if (event_type >= EVENT_TYPE_MAX)
        return "UNKNOWN";

    return event_names[event_type];
}

void debug_trace_event(uint8_t event_type, uint32_t param1, uint32_t param2)
{
    debug_event_t *event = &event_buffer[event_write_index];
    event->timestamp = kcb ? kcb->ticks : 0;
    event->event_type = event_type;
    event->task_id = (kcb && kcb->task_current && kcb->task_current->data)
                         ? ((tcb_t *) kcb->task_current->data)->id
                         : 0;
    event->reserved = 0;
    event->param1 = param1;
    event->param2 = param2;

    event_write_index = (event_write_index + 1) % DEBUG_EVENT_BUFFER_SIZE;
    if (event_count < DEBUG_EVENT_BUFFER_SIZE) {
        event_count++;
    } else {
        event_overwrites++;
    }
}

void debug_dump_events(void)
{
    uint32_t count;
    uint32_t overwrites;
    debug_trace_lock_t lock = debug_trace_enter();

    count = event_count;
    overwrites = event_overwrites;
    debug_trace_leave(lock);

    printf("debug trace: %u retained event%s, %u overwrite%s\n", count,
           (count == 1) ? "" : "s", overwrites, (overwrites == 1) ? "" : "s");

    for (uint32_t i = 0; i < count; i++) {
        debug_event_t event;

        if (debug_trace_get(i, &event) < 0)
            break;

        printf("[%u] %s ts=%u tid=%u p1=%u p2=%u\n", i,
               debug_trace_event_name(event.event_type), event.timestamp,
               event.task_id, event.param1, event.param2);
    }
}

void debug_clear_events(void)
{
    debug_trace_lock_t lock = debug_trace_enter();

    event_write_index = 0;
    event_count = 0;
    event_overwrites = 0;

    debug_trace_leave(lock);
}

uint32_t debug_trace_count(void)
{
    uint32_t count;
    debug_trace_lock_t lock = debug_trace_enter();

    count = event_count;

    debug_trace_leave(lock);
    return count;
}

uint32_t debug_trace_overwrites(void)
{
    uint32_t overwrites;
    debug_trace_lock_t lock = debug_trace_enter();

    overwrites = event_overwrites;

    debug_trace_leave(lock);
    return overwrites;
}

int32_t debug_trace_get(uint32_t index, debug_event_t *event)
{
    uint32_t count;
    uint32_t start;
    debug_trace_lock_t lock;

    if (!event)
        return -1;

    lock = debug_trace_enter();

    count = event_count;
    if (index >= count) {
        debug_trace_leave(lock);
        return -1;
    }

    start = (count < DEBUG_EVENT_BUFFER_SIZE) ? 0 : event_write_index;
    *event = event_buffer[(start + index) % DEBUG_EVENT_BUFFER_SIZE];
    debug_trace_leave(lock);

    return 0;
}

#endif
