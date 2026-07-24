#pragma once

#include <types.h>

/*
 * Tracing system — 5-pass X-macro architecture
 *
 * trace_events.h is #include'd five times with different macro
 * definitions to generate:
 *
 *   Pass 1 -> debug_event_type_t enum
 *   Pass 2 -> event-class struct definitions
 *   Pass 3 -> always_inline write functions (_tr_do_EVENT_##name)
 *   Pass 4 -> per-event print functions (debug_trace.c)
 *   Pass 5 -> print-function pointer table  (debug_trace.c)
 *
 * Users define events in trace_events.h using:
 *
 *   DEFINE_EVENT_CLASS(name,  __field(type, fieldname) ...)
 *   DEFINE_EVENT(name, class, prot, proto,  pairs...)
 *
 *   Pairs: _NO(field) skip, _SET(field,expr) store, TR_TID task id
 */

#if CONFIG_DEBUG_TRACE

/* ═══════════════════════════════════════════════════════════════════ *
 *   Pass 1: enum
 * ═══════════════════════════════════════════════════════════════════ */

#define DEFINE_EVENT_CLASS(...)  /* nothing */
#define __field(...)             /* nothing */
#define DEFINE_EVENT(name, ...)  EVENT_##name,

typedef enum {
#include <sys/trace_events.h>
	EVENT_TYPE_MAX
} debug_event_type_t;

#undef DEFINE_EVENT_CLASS
#undef __field
#undef DEFINE_EVENT

/*
 * Reader-side event struct — used by debug_trace_get().
 * Each class has its own packed layout (DEBUG_EVENT_STRUCT_SIZE
 * bytes); this struct provides a common projection for reading.
 */
typedef struct {
	uint8_t  event_type;
	uint8_t  reserved;
	uint16_t task_id;
	uint32_t timestamp;
	uint32_t param1;
	uint32_t param2;
} debug_event_t;
STATIC_ASSERT(sizeof(debug_event_t) == DEBUG_EVENT_STRUCT_SIZE,
              "debug_event_t size mismatch");

#include <hal.h>
#include <sys/task.h>

#define DEBUG_TRACE_EVENT_WORDS ((DEBUG_EVENT_STRUCT_SIZE) / sizeof(uint32_t))
STATIC_ASSERT((DEBUG_EVENT_STRUCT_SIZE) % 4 == 0,
              "DEBUG_EVENT_STRUCT_SIZE must be a multiple of 4");

extern uint32_t debug_trace_buffer[DEBUG_EVENT_BUFFER_SIZE * DEBUG_TRACE_EVENT_WORDS];
extern volatile uint32_t debug_trace_total;

#if (DEBUG_EVENT_BUFFER_SIZE & (DEBUG_EVENT_BUFFER_SIZE - 1)) == 0
#define DEBUG_TRACE_BUFFER_INDEX(sequence) \
	((uint32_t) (sequence) & (DEBUG_EVENT_BUFFER_SIZE - 1U))
#else
#define DEBUG_TRACE_BUFFER_INDEX(sequence) \
	((uint32_t) (sequence) % DEBUG_EVENT_BUFFER_SIZE)
#endif

#define DEBUG_TRACE_CURRENT_TASK_ID()                      \
	(likely(kcb && kcb->task_current && kcb->task_current->data) \
	     ? ((tcb_t *) kcb->task_current->data)->id         \
	     : 0U)


/* ═══════════════════════════════════════════════════════════════════ *
 *   Pass 2: class struct definitions
 *
 *   STATIC_ASSERT catches wrong field ordering at compile time:
 *   if fields are not declared in descending-size order (u32→u16→u8)
 *   the compiler inserts padding → struct exceeds size → compile error.
 *   aligned(4) ensures native lw/lh/lb access without packed overhead.
 * ═══════════════════════════════════════════════════════════════════ */

#define DEFINE_EVENT(...)        /* nothing */
#define __field(type, name_)     type name_;

#define DEFINE_EVENT_CLASS(name, ...)                                    \
	struct debug_event_cls_##name {                                  \
		uint8_t  event_type;                                     \
		uint8_t  reserved;                                       \
		uint16_t task_id;                                        \
		uint32_t timestamp;                                      \
		union {                                                  \
			struct { __VA_ARGS__ };                          \
			struct { uint32_t _param1; uint32_t _param2; };  \
		};                                                       \
	} __attribute__((aligned(4)));                               \
	STATIC_ASSERT(sizeof(struct debug_event_cls_##name) == DEBUG_EVENT_STRUCT_SIZE, \
	              "event class size mismatch");

#include <sys/trace_events.h>

#undef DEFINE_EVENT
#undef __field
#undef DEFINE_EVENT_CLASS

/* ═══════════════════════════════════════════════════════════════════ *
 *   Interrupt protection helpers
 * ═══════════════════════════════════════════════════════════════════ */

/* csrrci/csrsi replace hal_interrupt_set — 1 CSR insn
 * per direction instead of 3.  Saves ~4-5 cycles per SAFE event. */
#define TR_SAFE_ENTER()                                             \
	uint32_t _tr_s;                                             \
	asm volatile("csrrci %0, mstatus, 8" : "=r"(_tr_s) : : "memory")
#define TR_SAFE_LEAVE()                                             \
	do {                                                        \
		if ((_tr_s) & 8)                                    \
			asm volatile("csrsi mstatus, 8" ::: "memory"); \
	} while (0)
#define TR_RAW_ENTER()   do {} while (0)
#define TR_RAW_LEAVE()   do {} while (0)

/* ═══════════════════════════════════════════════════════════════════ *
 *   Pair processing — up to 11 pairs per event
 *
 *   Each pair is a single macro invocation:
 *     _NO(field)           -> skip store
 *     _SET(field, expr)    -> _ev->field = (expr)
 *     TR_TID               -> store current task id (always at _ev->task_id)
 *
 *   _TR_UNPACK_PAIR uses paste to dispatch on the pair token
 *   (_TR_PROCP_##_NO  etc.) so arbitrary _SET expressions work.
 *
 *   Pad‑to‑11 sentinels ensure any event works without counting.
 *   Extra arguments beyond 11 are silently discarded via the trailing `, ...`.
 * ═══════════════════════════════════════════════════════════════════ */

#define _TR_SKIP_PAIR  _NO(_TR_SKP)

#define _TR_FOR_EACH_PAIR(...)                                           \
	_TR_FOR_EACH_11(__VA_ARGS__,                                     \
		_TR_SKIP_PAIR, _TR_SKIP_PAIR, _TR_SKIP_PAIR,             \
		_TR_SKIP_PAIR, _TR_SKIP_PAIR, _TR_SKIP_PAIR,             \
		_TR_SKIP_PAIR, _TR_SKIP_PAIR, _TR_SKIP_PAIR,             \
		_TR_SKIP_PAIR, _TR_SKIP_PAIR)

/*
 *  With DEBUG_EVENT_STRUCT_SIZE = 16 bytes (4 words):
 *     8 B  framework fields (event_type, reserved, task_id, timestamp)
 *     8 B  remaining for user __field entries
 *
 *  The 8 remaining bytes can hold up to 8 __field entries
 *  (e.g. eight uint8_t).  A DEFINE_EVENT invocation then has at most:
 *     8  _SET   for the __field entries
 *     1  _SET   for the reserved byte       (framework, optional store)
 *     1  TR_TID (or _SET(task_id, ...))     (per-event store)
 *     1  _PRINT                            (per-event print format)
 *    ---------
 *    11  pairs total
 *
 *  Extra arguments beyond 11 are silently discarded
 *  via the trailing `, ...`.
 *
 *  If DEBUG_EVENT_STRUCT_SIZE grows (e.g. to 8 words),
 *  expand _TR_FOR_EACH_11 to a larger variant.
 */
#define _TR_FOR_EACH_11(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, ...) \
	_TR_UNPACK_PAIR(p1)  _TR_UNPACK_PAIR(p2)  _TR_UNPACK_PAIR(p3)   \
	_TR_UNPACK_PAIR(p4)  _TR_UNPACK_PAIR(p5)  _TR_UNPACK_PAIR(p6)   \
	_TR_UNPACK_PAIR(p7)  _TR_UNPACK_PAIR(p8)  _TR_UNPACK_PAIR(p9)   \
	_TR_UNPACK_PAIR(p10) _TR_UNPACK_PAIR(p11)

/* _TR_UNPACK_PAIR(_NO(field))  ->  _TR_PROCP_##_NO  ->  _TR_PROCP__NO(field)  */
#define _TR_UNPACK_PAIR(pair)  _TR_PROCP_##pair

#define _TR_PROCP__NO(f)         /* skip */
#define _TR_PROCP__SET(f, e)     _ev->f = (e);
#define _TR_PROCP_TR_TID          _ev->task_id = (uint16_t)DEBUG_TRACE_CURRENT_TASK_ID();
#define _TR_PROCP__PRINT(...)    /* ignored in write pass */

/* ═══════════════════════════════════════════════════════════════════ *
 *   Pass 3: per-event static inline write functions
 *
 *   One function per event, always_inline.  The proto argument
 *   provides the typed parameter list so callers pass arguments
 *   directly — no positional extraction needed.
 * ═══════════════════════════════════════════════════════════════════ */

#define DEFINE_EVENT_CLASS(...)  /* nothing */
#define __field(...)             /* nothing */

#define DEFINE_EVENT(name, cls, prot, proto, ...)                        \
	__attribute__((always_inline)) static inline void                 \
	_tr_do_EVENT_##name proto                                       \
	{                                                                \
		struct debug_event_cls_##cls *_ev;                       \
		uint32_t _total, _idx;                                   \
		TR_##prot##_ENTER();                                     \
		_total = debug_trace_total;                              \
		_idx   = DEBUG_TRACE_BUFFER_INDEX(_total);               \
		_ev    = (void *)&debug_trace_buffer[_idx * DEBUG_TRACE_EVENT_WORDS]; \
		_ev->event_type = (uint8_t)(EVENT_##name);               \
		_TR_FOR_EACH_PAIR(__VA_ARGS__)                           \
		_ev->timestamp  = (uint32_t)(likely(kcb)                \
			? kcb->ticks : 0U);                              \
		debug_trace_total = _total + 1;                          \
		TR_##prot##_LEAVE();                                     \
	}

#include <sys/trace_events.h>

#undef DEFINE_EVENT
#undef __field
#undef DEFINE_EVENT_CLASS

/* ═══════════════════════════════════════════════════════════════════ *
 *   Public API macro
 *
 *   DEBUG_TRACE_EVENT(EVENT_xxx, args...)
 *
 *   Args are forwarded directly — the per‑event function signature
 *   provides compile‑time type checking.
 * ═══════════════════════════════════════════════════════════════════ */

#define DEBUG_TRACE_EVENT(ev, ...) \
	_tr_do_##ev(__VA_ARGS__)


void debug_dump_events(void);
void debug_clear_events(void);
uint32_t debug_trace_count(void);
uint32_t debug_trace_overwrites(void);
int32_t debug_trace_get(uint32_t index, debug_event_t *event);

#else /* !CONFIG_DEBUG_TRACE */

#define DEBUG_TRACE_EVENT(...) do {} while (0)

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

static inline int32_t debug_trace_get(uint32_t index, void *event)
{
	return 0U;
}

#endif /* CONFIG_DEBUG_TRACE */
