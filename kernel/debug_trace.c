#include <lib/libc.h>
#include <sys/debug_trace.h>
#include <sys/task.h>

#if CONFIG_DEBUG_TRACE

/*
 * Fixed‑stride trace buffer — every event occupies exactly
 * DEBUG_TRACE_EVENT_WORDS words (DEBUG_EVENT_STRUCT_SIZE bytes)
 *
 * The per‑type macros (debug_trace.h) write only the fields that
 * actually carry data; unused param slots are left uninitialized.
 *
 * debug_trace_total is a monotonic event counter.  The buffer holds
 * the last DEBUG_EVENT_BUFFER_SIZE events in circular fashion.
 */
uint32_t debug_trace_buffer[DEBUG_EVENT_BUFFER_SIZE * DEBUG_TRACE_EVENT_WORDS];
volatile uint32_t debug_trace_total;

/* ------------------------------------------------------------------ *
 * Internal helpers
 * ------------------------------------------------------------------ */

static uint32_t retained_count_locked(void)
{
	if (debug_trace_total < DEBUG_EVENT_BUFFER_SIZE)
		return debug_trace_total;
	return DEBUG_EVENT_BUFFER_SIZE;
}

static uint32_t overwrite_count_locked(void)
{
	if (debug_trace_total > DEBUG_EVENT_BUFFER_SIZE)
		return debug_trace_total - DEBUG_EVENT_BUFFER_SIZE;
	return 0U;
}

/*
 * Map a retained-event index (0 = oldest) to its absolute sequence
 * number, or return false when the index is out of range.
 */
static bool retained_sequence(uint32_t index, uint32_t *seq_out)
{
	uint32_t retained = retained_count_locked();

	if (index >= retained)
		return false;

	uint32_t first_seq =
		(debug_trace_total > DEBUG_EVENT_BUFFER_SIZE)
			? debug_trace_total - DEBUG_EVENT_BUFFER_SIZE
			: 0U;

	*seq_out = first_seq + index;
	return true;
}
/* ================================================================== *
 * Pass 4: per-event print functions
 *
 * Generated from trace_events.h with print-context _TR_PROCP_
 * definitions.  _SET / TR_TID / _NO pair actions are no-ops here;
 * only _PRINT emits output.
 * ================================================================== */

#undef _TR_PROCP__NO
#undef _TR_PROCP__SET
#undef _TR_PROCP_TR_TID
#undef _TR_PROCP__PRINT

#define _TR_PROCP__NO(...)       /* */
#define _TR_PROCP__SET(...)      /* */
#define _TR_PROCP_TR_TID          /* */
#define _TR_PROCP__PRINT(...)    int _tr_has_print; printf(__VA_ARGS__);

#define DEFINE_EVENT_CLASS(...)  /* nothing */
#define __field(...)             /* nothing */

#define DEFINE_EVENT(name, cls, prot, proto, ...)                        \
	static void _tr_print_EVENT_##name(uint32_t idx, uint32_t base) { \
		const struct debug_event_cls_##cls *_e =                 \
			(const void *)&debug_trace_buffer[base];         \
		printf("[%u] %14s ts=%u", idx, #name, _e->timestamp);   \
		_TR_FOR_EACH_PAIR(__VA_ARGS__)                           \
		(void)_tr_has_print;                                     \
		printf("\n");                                            \
	}

#include <sys/trace_events.h>

#undef DEFINE_EVENT

/* ================================================================== *
 * Pass 5: print-function pointer table
 *
 * Indexed by EVENT_##name, maps each event type to its
 * _tr_print_EVENT_##name handler.
 * ================================================================== */

static void (*const _tr_print_fn[EVENT_TYPE_MAX])(uint32_t, uint32_t) = {
#define DEFINE_EVENT_CLASS(...)  /* nothing */
#define __field(...)             /* nothing */
#define DEFINE_EVENT(name, ...)  [EVENT_##name] = _tr_print_EVENT_##name,
#include <sys/trace_events.h>
};

#undef DEFINE_EVENT
#undef __field
#undef DEFINE_EVENT_CLASS

static void _tr_print_event(uint32_t idx, uint32_t base)
{
	uint8_t ev = debug_trace_buffer[base] & 0xFFU;

	if (ev < EVENT_TYPE_MAX && _tr_print_fn[ev])
		_tr_print_fn[ev](idx, base);
	else
		printf("[%u] UNKNOWN\n", idx);
}

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

void debug_dump_events(void)
{
	uint32_t count;
	uint32_t overwrites;
	uint32_t first_seq;
	CRITICAL_ENTER();

	count      = retained_count_locked();
	overwrites = overwrite_count_locked();
	first_seq  = (debug_trace_total > DEBUG_EVENT_BUFFER_SIZE)
		? debug_trace_total - DEBUG_EVENT_BUFFER_SIZE
		: 0U;

	printf("debug trace: %u retained event%s, %u overwrite%s\n", count,
	       (count == 1U) ? "" : "s", overwrites,
	       (overwrites == 1U) ? "" : "s");

	for (uint32_t i = 0U; i < count; i++) {
		uint32_t base = DEBUG_TRACE_BUFFER_INDEX(first_seq + i) * DEBUG_TRACE_EVENT_WORDS;

		_tr_print_event(i, base);
	}

	CRITICAL_LEAVE();
}

void debug_clear_events(void)
{
	CRITICAL_ENTER();

	debug_trace_total = 0U;

	CRITICAL_LEAVE();
}

uint32_t debug_trace_count(void)
{
	uint32_t count;
	CRITICAL_ENTER();

	count = retained_count_locked();

	CRITICAL_LEAVE();
	return count;
}

uint32_t debug_trace_overwrites(void)
{
	uint32_t overwrites;
	CRITICAL_ENTER();

	overwrites = overwrite_count_locked();

	CRITICAL_LEAVE();
	return overwrites;
}

/*
 * Fill *event from the i-th retained event (0 = oldest).
 * 
 * debug_event_t is a common-readout struct.  Only event_type and
 * timestamp are always valid.  param1 / param2 (and any future
 * fields) may contain stale data from previously overwritten slots.
 * Likewise, task_id is stale for events that don't emit TR_TID
 * (e.g. ISR_ENTER / ISR_EXIT) — this avoids per-event field
 * zeroing so write cost stays minimal.  Callers must use event_type
 * to know which fields the current event actually defines.
*
 * Returns 0 on success, -1 if index is out of range or event is NULL.
 */
int32_t debug_trace_get(uint32_t index, debug_event_t *event)
{
	uint32_t seq;
	uint32_t base;

	if (!event)
		return -1;

	CRITICAL_ENTER();

	if (!retained_sequence(index, &seq)) {
		CRITICAL_LEAVE();
		return -1;
	}

	base = DEBUG_TRACE_BUFFER_INDEX(seq) * DEBUG_TRACE_EVENT_WORDS;
	*event = *(const debug_event_t *)(const void *)&debug_trace_buffer[base];

	CRITICAL_LEAVE();
	return 0;
}

#endif /* CONFIG_DEBUG_TRACE */
