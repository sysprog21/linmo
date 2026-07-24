/*
 * Event definitions — single source of truth for the tracing system.
 *
 * This file is #include'd *multiple times* by debug_trace.h with
 * different preprocessor state.  Do NOT add #pragma once.
 *
 * ── Fixed-size slot layout (configurable via DEBUG_EVENT_STRUCT_SIZE) ──
 *
 *   0      event_type  uint8_t   framework  (written by framework)
 *   1      reserved    uint8_t   user       (_SET / _NO, optional)
 *   2-3    task_id     uint16_t  TR_TID     (always at this offset)
 *   4-7    timestamp   uint32_t  framework  (written by framework)
 *   8+     __field     user      max (size - 8) bytes — _Static_assert guarded
 *
 *   task_id (offsets 2-3) is always a uint16_t.  TR_TID is a
 *   parameterless pair that stores DEBUG_TRACE_CURRENT_TASK_ID()
 *   into _ev->task_id at the fixed offset.
 *
 * ── Event classes ──
 *
 *   DEFINE_EVENT_CLASS(classname,
 *       __field(type, fieldname)
 *       ...
 *   )
 *
 *   Defines an aligned struct fitting within DEBUG_EVENT_STRUCT_SIZE
 *   bytes, with implicit event_type (uint8_t), reserved (uint8_t),
 *   task_id (uint16_t), and timestamp (uint32_t).
 *
 * ── Events ──
 *
 *   DEFINE_EVENT(name, class, prot, proto,
 *       pairs...
 *   )
 *
 *   name  — EVENT_##name enum value
 *   class — event class defined above (generates struct debug_event_cls_##class)
 *   prot  — SAFE or RAW (interrupt handling around the write)
 *   proto — param list for the generated always_inline write function,
 *           e.g. (uint32_t p1, uint32_t p2) or (void)
 *
 *   Pairs:
 *     _NO(field)               — skip this field
 *     _SET(field, expr)        — store _ev->field = (expr)
 *     TR_TID                   — store task_id at fixed offset (2-3)
 *     _PRINT(fmt_and_args...)  — printf format, raw paste (use _e->field)
 */

/* ══════════════════════════════════════════════════════════════════════ *
 *                        Event classes
 * ══════════════════════════════════════════════════════════════════════ */

DEFINE_EVENT_CLASS(task_cls,
    __field(uint32_t, param1)
    __field(uint32_t, param2)
)

DEFINE_EVENT_CLASS(pipe_cls,
    __field(uint32_t, len)
    __field(uint32_t, addr)
)

DEFINE_EVENT_CLASS(mqueue_cls,
    __field(uint32_t, len)
    __field(uint32_t, data)
)

DEFINE_EVENT_CLASS(sem_cls,
    __field(uint32_t, sem_id)
)

DEFINE_EVENT_CLASS(mutex_cls,
    __field(uint32_t, mutex_id)
)

DEFINE_EVENT_CLASS(isr_cls,
    __field(uint8_t, isr_num)
)

/* ══════════════════════════════════════════════════════════════════════ *
 *                           Events
 * ══════════════════════════════════════════════════════════════════════ */

/* ── task events ── */

DEFINE_EVENT(TASK_CREATE, task_cls, SAFE, (uint32_t p1, uint32_t p2),
    TR_TID,
    _SET(param1, p1),
    _SET(param2, p2),
    _PRINT(" creator=%u tid=%u prio=%u", _e->task_id, _e->param1, _e->param2)
)
DEFINE_EVENT(TASK_DESTROY, task_cls, RAW, (uint32_t p1, uint32_t p2),
    TR_TID,
    _SET(param1, p1),
    _SET(param2, p2),
    _PRINT(" destroyer=%u tid=%u prev=%u", _e->task_id, _e->param1, _e->param2)
)
DEFINE_EVENT(TASK_SWITCH, task_cls, RAW, (uint32_t p1, uint32_t p2),
    _NO(task_id),
    _SET(param1, p1),
    _SET(param2, p2),
    _PRINT(" prev=%u next=%u", _e->param1, _e->param2)
)
DEFINE_EVENT(TASK_DELAY, task_cls, SAFE, (uint32_t p1, uint16_t p2),
    _SET(task_id, p2),
    _SET(param1, p1),
    _NO(param2),
    _PRINT(" tid=%u ticks=%u", _e->task_id, _e->param1)
)
DEFINE_EVENT(TASK_SUSPEND, task_cls, RAW, (uint32_t id),
    TR_TID,
    _SET(param1, id),
    _NO(param2),
    _PRINT(" tid=%u id=%u", _e->task_id, _e->param1)
)
DEFINE_EVENT(TASK_RESUME, task_cls, RAW, (uint32_t id),
    TR_TID,
    _SET(param1, id),
    _NO(param2),
    _PRINT(" tid=%u id=%u", _e->task_id, _e->param1)
)
DEFINE_EVENT(TASK_YIELD, task_cls, SAFE, (void),
    TR_TID,
    _NO(param1),
    _NO(param2),
    _PRINT(" tid=%u", _e->task_id)
)
DEFINE_EVENT(EXCEPTION, task_cls, SAFE, (uint32_t p1, uint32_t p2),
    _NO(task_id),
    _SET(param1, p1),
    _SET(param2, p2),
    _PRINT(" p1=%u p2=%u", _e->param1, _e->param2)
)

/* ── pipe events ── */

DEFINE_EVENT(PIPE_READ, pipe_cls, SAFE, (uint32_t len, uint32_t addr),
    TR_TID,
    _SET(len,  len),
    _SET(addr, addr),
    _PRINT(" tid=%u len=%u addr=0x%x", _e->task_id, _e->len, _e->addr)
)
DEFINE_EVENT(PIPE_WRITE, pipe_cls, SAFE, (uint32_t len, uint32_t addr),
    TR_TID,
    _SET(len,  len),
    _SET(addr, addr),
    _PRINT(" tid=%u len=%u addr=0x%x", _e->task_id, _e->len, _e->addr)
)

/* ── mqueue events ── */

DEFINE_EVENT(MQUEUE_SEND, mqueue_cls, SAFE, (uint32_t len, uint32_t data),
    TR_TID,
    _SET(len,  len),
    _SET(data, data),
    _PRINT(" tid=%u len=%u data=%u", _e->task_id, _e->len, _e->data)
)
DEFINE_EVENT(MQUEUE_RECV, mqueue_cls, SAFE, (uint32_t len, uint32_t data),
    TR_TID,
    _SET(len,  len),
    _SET(data, data),
    _PRINT(" tid=%u len=%u data=%u", _e->task_id, _e->len, _e->data)
)

/* ── semaphore events ── */

DEFINE_EVENT(SEM_WAIT, sem_cls, SAFE, (uint32_t sem_id),
    TR_TID,
    _SET(sem_id, sem_id),
    _PRINT(" tid=%u sem=%u", _e->task_id, _e->sem_id)
)
DEFINE_EVENT(SEM_POST, sem_cls, SAFE, (uint32_t sem_id),
    TR_TID,
    _SET(sem_id, sem_id),
    _PRINT(" tid=%u sem=%u", _e->task_id, _e->sem_id)
)

/* ── mutex events ── */

DEFINE_EVENT(MUTEX_LOCK, mutex_cls, SAFE, (uint32_t mutex_id),
    TR_TID,
    _SET(mutex_id, mutex_id),
    _PRINT(" tid=%u mtx=%u", _e->task_id, _e->mutex_id)
)
DEFINE_EVENT(MUTEX_UNLOCK, mutex_cls, SAFE, (uint32_t mutex_id),
    TR_TID,
    _SET(mutex_id, mutex_id),
    _PRINT(" tid=%u mtx=%u", _e->task_id, _e->mutex_id)
)

/* ── ISR events ── */

DEFINE_EVENT(ISR_ENTER, isr_cls, RAW, (uint8_t num),
    _SET(isr_num, num),
    _PRINT(" isr=%u", _e->isr_num)
)
DEFINE_EVENT(ISR_EXIT, isr_cls, RAW, (uint8_t num),
    _SET(isr_num, num),
    _PRINT(" isr=%u", _e->isr_num)
)

