#pragma once

/* Software Timers
 *
 * Provides software timers with callback functionality. Timers can operate
 * in one-shot or auto-reload modes and execute user-defined callbacks upon
 * expiration. All timers are managed by the kernel and serviced during
 * system timer interrupts.
 */

#include <types.h>

/* Timer Operating Modes */
typedef enum {
    TIMER_DISABLED = 0,  /* Timer is created but not running */
    TIMER_ONESHOT = 1,   /* Timer fires once, then disables itself */
    TIMER_AUTORELOAD = 2 /* Timer re-arms itself automatically after firing */
} timer_mode_t;

/* Timer Control Block
 *
 * Internal structure holding the state of a single software timer. This
 * structure is managed entirely by the kernel; user code interacts with
 * timers via their unique ID handles.
 */
typedef struct {
    /* Timing Parameters */
    uint32_t deadline_ticks; /* Expiration time in absolute system ticks */
    uint32_t last_expected_fire_tick; /* Last calculated expected fire time for
                                       * periodic timer
                                       */
    uint32_t period_ms;               /* Reload period in milliseconds */

    /* Timer Identification and State */
    uint16_t id;       /* Unique handle assigned by the kernel */
    uint8_t mode;      /* Current operating mode (from timer_mode_t) */
    uint8_t _reserved; /* Padding for alignment */

    /* Callback Configuration */
    void *(*callback)(void *arg); /* Function to execute upon timer expiry */
    void *arg;                    /* User-defined argument passed to callback */

    /* Embedded node for timer */
    list_node_t t_node;         /* All timer list node*/
    list_node_t t_running_node; /* Running timer list node */
} timer_t;

/* Timer Management Functions */

/* Creates a new software timer.
 *
 * The timer is created in a DISABLED state and must be started with
 * 'mo_timer_start()' before it will begin counting.
 *
 * @callback  : The function to execute upon expiry (cannot be NULL)
 * @period_ms : The timer's period in milliseconds (must be > 0)
 * @arg       : A user-defined argument to be passed to the callback
 *
 * Returns a positive timer ID on success, or a negative error code on failure
 */
int32_t mo_timer_create(void *(*callback)(void *arg),
                        uint32_t period_ms,
                        void *arg);

/* Destroys a software timer and frees its resources.
 *
 * If the timer is active, it will be cancelled before being destroyed.
 * After destruction, the timer ID becomes invalid and should not be used.
 *
 * @id : The ID of the timer to destroy, as returned by 'mo_timer_create'
 *
 * Returns ERR_OK on success, or ERR_FAIL if the ID is not found
 */
int32_t mo_timer_destroy(uint16_t id);

/* Timer Control Functions */

/* Starts or restarts a software timer.
 *
 * This function arms the timer and adds it to the active list. If the timer
 * was already running, its deadline is recalculated and it is rescheduled.
 * The timer will fire after its configured period has elapsed.
 *
 * @id   : The ID of the timer to start
 * @mode : The desired mode (TIMER_ONESHOT or TIMER_AUTORELOAD)
 *
 * Returns ERR_OK on success, or ERR_FAIL if the ID or mode is invalid
 */
int32_t mo_timer_start(uint16_t id, uint8_t mode);

/* Cancels a running software timer.
 *
 * This function disarms the timer and removes it from the active list. The
 * timer object itself is not destroyed and can be restarted later with
 * 'mo_timer_start()'.
 *
 * @id : The ID of the timer to cancel
 *
 * Returns ERR_OK on success, or ERR_FAIL if the timer is not found or not
 * running
 */
int32_t mo_timer_cancel(uint16_t id);

/* Timer Utility Macros */

/* Convert milliseconds to system ticks.
 *
 * F_TIMER is the scheduler tick frequency (in Hz), which must be defined at
 * build-time. This calculation is performed with 64-bit integers to prevent
 * overflow with large millisecond values.
 */
#define MS_TO_TICKS(ms) \
    ((uint32_t) (((uint64_t) (ms) * (uint64_t) (F_TIMER)) / 1000U))
