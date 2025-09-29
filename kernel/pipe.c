#include <lib/libc.h>
#include <sys/pipe.h>
#include <sys/task.h>

#include "private/error.h"
#include "private/utils.h"

/* Minimum and maximum pipe sizes */
#define PIPE_MIN_SIZE 4
#define PIPE_MAX_SIZE 32768

#define __maybe_unused __attribute__((__unused__))

/* Enhanced validation with comprehensive integrity checks */
static inline bool pipe_is_valid(const pipe_t *p)
{
    return p && p->magic == PIPE_MAGIC && p->buf && p->mask > 0 &&
           ispowerof2(p->mask + 1) && p->used <= (p->mask + 1) &&
           p->head <= p->mask && p->tail <= p->mask;
}

/* empty/full checks using the used counter */
static inline bool pipe_is_empty(const pipe_t *p)
{
    return p->used == 0;
}

static inline bool pipe_is_full(const pipe_t *p)
{
    return p->used == (p->mask + 1);
}

/* Get available space for writing */
static inline uint16_t pipe_free_space_internal(const pipe_t *p)
{
    return (p->mask + 1) - p->used;
}

static inline __attribute__((__unused__)) char pipe_get_byte(pipe_t *p)
{
    char val = p->buf[p->head];
    p->head = (p->head + 1) & p->mask;
    p->used--;
    return val;
}

static inline __attribute__((__unused__)) void pipe_put_byte(pipe_t *p, char c)
{
    p->buf[p->tail] = c;
    p->tail = (p->tail + 1) & p->mask;
    p->used++;
}

/* bulk read operation within critical section */
static uint16_t pipe_bulk_read(pipe_t *p, char *dst, uint16_t max_bytes)
{
    uint16_t bytes_read = 0;
    uint16_t available = p->used;
    uint16_t to_read = min(max_bytes, available);

    while (bytes_read < to_read) {
        /* Calculate contiguous bytes until wrap */
        uint16_t head_to_end = (p->mask + 1) - p->head;
        uint16_t chunk_size = min(to_read - bytes_read, head_to_end);

        /* Copy contiguous chunk */
        memcpy(dst + bytes_read, p->buf + p->head, chunk_size);

        /* Update indices */
        p->head = (p->head + chunk_size) & p->mask;
        p->used -= chunk_size;
        bytes_read += chunk_size;
    }

    return bytes_read;
}

/* bulk write operation within critical section */
static uint16_t pipe_bulk_write(pipe_t *p, const char *src, uint16_t max_bytes)
{
    uint16_t bytes_written = 0;
    uint16_t available_space = pipe_free_space_internal(p);
    uint16_t to_write = min(max_bytes, available_space);

    while (bytes_written < to_write) {
        /* Calculate contiguous bytes until wrap */
        uint16_t tail_to_end = (p->mask + 1) - p->tail;
        uint16_t chunk_size = min(to_write - bytes_written, tail_to_end);

        /* Copy contiguous chunk */
        memcpy(p->buf + p->tail, src + bytes_written, chunk_size);

        /* Update indices */
        p->tail = (p->tail + chunk_size) & p->mask;
        p->used += chunk_size;
        bytes_written += chunk_size;
    }

    return bytes_written;
}

/* Invalidate pipe during destruction to prevent reuse */
static inline void pipe_invalidate(pipe_t *p)
{
    if (p) {
        p->magic = 0xDEADBEEF;
        p->mask = 0;
        p->used = UINT16_MAX; /* Invalid state */
    }
}

pipe_t *mo_pipe_create(uint16_t size)
{
    /* Input validation and size adjustment */
    if (unlikely(size < PIPE_MIN_SIZE))
        size = PIPE_MIN_SIZE;
    if (unlikely(size > PIPE_MAX_SIZE))
        size = PIPE_MAX_SIZE;

    /* Round up to next power of 2 for efficient masking */
    if (!ispowerof2(size))
        size = nextpowerof2(size);

    /* Allocate pipe structure */
    pipe_t *p = malloc(sizeof(pipe_t));
    if (unlikely(!p))
        return NULL;

    /* Initialize to safe state first */
    p->buf = NULL;
    p->mask = 0;
    p->head = p->tail = p->used = 0;
    p->magic = 0;

    /* Allocate buffer with alignment for better performance */
    p->buf = malloc(size);
    if (unlikely(!p->buf)) {
        free(p);
        return NULL;
    }

    /* Initialize pipe structure */
    p->mask = size - 1;
    p->head = p->tail = p->used = 0;
    p->magic = PIPE_MAGIC; /* Mark as valid last to prevent races */

    return p;
}

int32_t mo_pipe_destroy(pipe_t *p)
{
    if (!p)
        return ERR_OK; /* Destroying NULL is no-op */

    if (unlikely(!pipe_is_valid(p)))
        return ERR_FAIL;

    /* Invalidate structure to prevent further use */
    pipe_invalidate(p);

    /* Free resources */
    if (p->buf) {
        free(p->buf);
        p->buf = NULL;
    }
    free(p);

    return ERR_OK;
}

void mo_pipe_flush(pipe_t *p)
{
    if (unlikely(!pipe_is_valid(p)))
        return;

    CRITICAL_ENTER();
    p->head = p->tail = p->used = 0;
    CRITICAL_LEAVE();
}

int32_t mo_pipe_size(pipe_t *p)
{
    if (unlikely(!pipe_is_valid(p)))
        return -1;

    /* Volatile read is atomic for 16-bit values on RV32I */
    return (int32_t) p->used;
}

int32_t mo_pipe_capacity(pipe_t *p)
{
    if (unlikely(!pipe_is_valid(p)))
        return -1;

    return (int32_t) (p->mask + 1);
}

int32_t mo_pipe_free_space(pipe_t *p)
{
    if (unlikely(!pipe_is_valid(p)))
        return -1;

    CRITICAL_ENTER();
    int32_t free_space = (int32_t) pipe_free_space_internal(p);
    CRITICAL_LEAVE();

    return free_space;
}

/* Blocking operations with proper yielding strategy */
static void pipe_wait_until_readable(pipe_t *p)
{
    while (1) {
        CRITICAL_ENTER();
        if (!pipe_is_empty(p)) {
            CRITICAL_LEAVE();
            return;
        }
        /* Nothing to read – drop critical section and yield CPU */
        CRITICAL_LEAVE();
        mo_task_wfi(); /* Yield CPU without blocking task state */
    }
}

static void pipe_wait_until_writable(pipe_t *p)
{
    while (1) {
        CRITICAL_ENTER();
        if (!pipe_is_full(p)) {
            CRITICAL_LEAVE();
            return;
        }
        /* Buffer full – yield until space is available */
        CRITICAL_LEAVE();
        mo_task_wfi(); /* Yield CPU without blocking task state */
    }
}

/* Blocking read with optimized bulk operations */
int32_t mo_pipe_read(pipe_t *p, char *dst, uint16_t len)
{
    if (unlikely(!pipe_is_valid(p) || !dst || !len))
        return ERR_FAIL;

    uint16_t bytes_read = 0;

    while (bytes_read < len) {
        /* Wait for data to become available */
        pipe_wait_until_readable(p);

        /* Read as much as possible in one critical section */
        CRITICAL_ENTER();
        uint16_t chunk = pipe_bulk_read(p, dst + bytes_read, len - bytes_read);
        CRITICAL_LEAVE();

        bytes_read += chunk;

        /* If we still need more data, the next iteration will wait properly */
    }

    return (int32_t) bytes_read;
}

/* Blocking write with optimized bulk operations */
int32_t mo_pipe_write(pipe_t *p, const char *src, uint16_t len)
{
    if (unlikely(!pipe_is_valid(p) || !src || len == 0))
        return ERR_FAIL;

    uint16_t bytes_written = 0;

    while (bytes_written < len) {
        /* Wait for space to become available */
        pipe_wait_until_writable(p);

        /* Write as much as possible in one critical section */
        CRITICAL_ENTER();
        uint16_t chunk =
            pipe_bulk_write(p, src + bytes_written, len - bytes_written);
        CRITICAL_LEAVE();

        bytes_written += chunk;

        /* If we still need to write more, the next iteration will wait properly
         */
    }

    return (int32_t) bytes_written;
}

/* Non-blocking read with optimized bulk operations */
int32_t mo_pipe_nbread(pipe_t *p, char *dst, uint16_t len)
{
    if (unlikely(!pipe_is_valid(p) || !dst || len == 0))
        return ERR_FAIL;

    uint16_t bytes_read;

    CRITICAL_ENTER();
    bytes_read = pipe_bulk_read(p, dst, len);
    CRITICAL_LEAVE();

    return (int32_t) bytes_read;
}

/* Non-blocking write with optimized bulk operations */
int32_t mo_pipe_nbwrite(pipe_t *p, const char *src, uint16_t len)
{
    if (unlikely(!pipe_is_valid(p) || !src || len == 0))
        return ERR_FAIL;

    uint16_t bytes_written;

    CRITICAL_ENTER();
    bytes_written = pipe_bulk_write(p, src, len);
    CRITICAL_LEAVE();

    return (int32_t) bytes_written;
}
