/* libc: standard I/O functions.
 *
 * Default handlers do nothing (or return error codes) so the kernel can run
 * even if the board code forgets to install real console hooks. These hooks
 * allow a consistent I/O interface regardless of the underlying hardware.
 *
 * Thread-safe printing:
 * Uses deferred logging system for thread-safe output in preemptive mode.
 * Messages are enqueued and processed by a dedicated logger task.
 * Falls back to direct output during early boot or if queue is full.
 */

#include <lib/libc.h>
#include <stdarg.h>
#include <sys/logger.h>

#include "private/stdio.h"

/* Ignores output character, returns 0 (success). */
static int stdout_null(int c)
{
    (void) c;
    return 0;
}

/* Returns -1 to indicate no input is available. */
static int stdin_null(void)
{
    return -1;
}

/* Returns 0 to indicate no input is ready. */
static int poll_null(void)
{
    return 0;
}

/* Active hooks, initialized to default no-op handlers.
 * These pointers will be updated by board-specific initialization code.
 */
static int (*stdout_hook)(int) = stdout_null;
static int (*stdin_hook)(void) = stdin_null;
static int (*poll_hook)(void) = poll_null;

/* Hook installers: Register the provided I/O functions. */
void _stdout_install(int (*hook)(int))
{
    stdout_hook = (hook) ? hook : stdout_null;
}

void _stdin_install(int (*hook)(void))
{
    stdin_hook = (hook) ? hook : stdin_null;
}

void _stdpoll_install(int (*hook)(void))
{
    poll_hook = (hook) ? hook : poll_null;
}

/* I/O helpers: Dispatch to the currently installed hooks. */

/* Calls the registered stdout hook to output a character. */
int _putchar(int c)
{
    return stdout_hook(c);
}

/* Calls the registered stdin hook to get a character.
 * This function blocks (busy-waits) until input is available.
 */
int _getchar(void)
{
    int ch;
    while ((ch = stdin_hook()) < 0)
        ; /* Spin loop, effectively waiting for input. */
    return ch;
}

/* Calls the registered poll hook to check for input readiness. */
int _kbhit(void)
{
    return poll_hook();
}

/* Base-10 string conversion without division (shared with ctype.c logic) */
static char *__str_base10(uint32_t value, char *buffer, int *length)
{
    if (value == 0) {
        buffer[0] = '0';
        *length = 1;
        return buffer;
    }
    int pos = 0;

    while (value > 0) {
        uint32_t q, r, t;

        q = (value >> 1) + (value >> 2);
        q += (q >> 4);
        q += (q >> 8);
        q += (q >> 16);
        q >>= 3;
        r = value - (((q << 2) + q) << 1);
        t = ((r + 6) >> 4);
        q += t;
        r -= (((t << 2) + t) << 1);

        buffer[pos++] = '0' + r;
        value = q;
    }
    *length = pos;

    return buffer;
}

/* Divide a number by base, returning remainder and updating number. */
static uint32_t divide(long *n, int base)
{
    uint32_t res;

    res = ((uint32_t) *n) % base;
    *n = (long) (((uint32_t) *n) / base);
    return res;
}

/* Parse an integer string in a given base. */
static int toint(const char **s)
{
    int i = 0;
    /* Convert digits until a non-digit character is found. */
    while (isdigit((int) **s))
        i = i * 10 + *((*s)++) - '0';

    return i;
}

/* Emits a single character and increments the total character count.
 * Bounds-aware version: only writes if within buffer limits.
 * Always increments len to track total chars that would be written (C99).
 */
static inline void printchar_bounded(char **str, char *end, int32_t c, int *len)
{
    if (str) {
        if (*str < end)
            **str = c;
        ++(*str);
    } else if (c) {
        _putchar(c);
    }
    (*len)++;
}

/* Supports: %s %d %u %x %p (no floating point).
 * Returns: Number of chars that would be written (C99 semantics).
 * NOTE: Does NOT include null terminator in return count.
 *
 * Deviations from C99:
 * - Limited format specifier support (no %f, %e, %g, etc.)
 * - No precision or complex width modifiers
 * - Simplified %p format (basic hex without "0x" prefix handling)
 *
 * ISR-Safe: No malloc, no blocking, reentrant, bounded execution time.
 */
int vsnprintf(char *str, size_t size, const char *fmt, va_list args)
{
    char *end = (size > 0) ? (str + size - 1) : str;
    const char *s;
    char pad;
    int width;
    int base;
    int sign;
    int i;
    long num;
    int len = 0; /* Total chars that would be written (excluding null) */
    char tmp[32];

    /* C99 semantics: allow NULL str if size is 0 (for size calculation) */
    if (!str && size != 0)
        return -1;

    const char *digits = "0123456789abcdef";

    /* Iterate through the format string */
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            printchar_bounded(&str, end, *fmt, &len);
            continue;
        }
        /* Process format specifier: '%' */
        ++fmt; /* Move past '%' */

        /* Get flags: padding character */
        pad = ' '; /* Default padding is space */
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        /* Get width: minimum field width */
        width = -1;
        if (isdigit(*fmt))
            width = toint(&fmt);

        base = 10; /* Default base for numbers is decimal */
        sign = 0;  /* Default is unsigned */

        /* Handle format specifiers */
        switch (*fmt) {
        case 'c': /* Character */
            printchar_bounded(&str, end, (char) va_arg(args, int), &len);
            continue;
        case 's': /* String */
            s = va_arg(args, char *);
            if (s == 0) /* Handle NULL string */
                s = "<NULL>";

            /* Print string, respecting width */
            for (; *s && width != 0; s++, width--)
                printchar_bounded(&str, end, *s, &len);

            /* Pad if necessary */
            while (width-- > 0)
                printchar_bounded(&str, end, pad, &len);
            continue;
        case 'l': /* Long integer modifier */
            fmt++;
            num = va_arg(args, long);
            break;
        case 'X':
        case 'x':
            base = 16;
            num = va_arg(args, long);
            break;
        case 'd': /* Signed Decimal */
            sign = 1;
            __attribute__((fallthrough));
        case 'u': /* Unsigned Decimal */
            num = va_arg(args, int);
            break;
        case 'p': /* Pointer address (hex) */
            base = 16;
            num = va_arg(args, size_t);
            width = sizeof(size_t) * 2; /* 2 hex digits per byte */
            break;
        case '%': /* Literal '%' */
            printchar_bounded(&str, end, '%', &len);
            continue;
        default: /* Unknown format specifier, ignore */
            continue;
        }

        /* Handle sign for signed integers */
        if (sign && num < 0) {
            num = -num;
            printchar_bounded(&str, end, '-', &len);
            width--;
        }

        /* Convert number to string (in reverse order) */
        i = 0;
        if (num == 0)
            tmp[i++] = '0';
        else if (base == 10)
            __str_base10(num, tmp, &i);
        else {
            while (num != 0)
                tmp[i++] = digits[divide(&num, base)];
        }

        /* Pad with leading characters if width is specified */
        width -= i;
        while (width-- > 0)
            printchar_bounded(&str, end, pad, &len);

        /* Print the number string in correct order */
        while (i-- > 0)
            printchar_bounded(&str, end, tmp[i], &len);
    }

    /* Always null-terminate within bounds (C99 requirement) */
    if (size > 0) {
        if (str <= end)
            *str = '\0';
        else
            *end = '\0';
    }

    /* Return total chars that would be written (C99 semantics),
     * NOT including the null terminator.
     */
    return len;
}

/* Formatted output to stdout.
 * Uses a fixed stack buffer - very long output will be truncated.
 * Thread-safe: Uses deferred logging via logger task.
 * Falls back to direct output during early boot, queue full, or after flush.
 *
 * Flush-aware behavior: After mo_logger_flush(), printf() outputs directly
 * to UART (direct_mode flag set), ensuring ordered output for multi-line
 * reports. Call mo_logger_async_resume() to re-enable async logging.
 */
int32_t printf(const char *fmt, ...)
{
    char buf[256]; /* Stack buffer for formatted output */
    va_list args;

    va_start(args, fmt);
    int32_t len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    /* Handle vsnprintf error (negative return indicates encoding error) */
    if (len < 0)
        return len;

    /* Try deferred logging only if:
     * 1. Message fits in log entry (avoids silent truncation)
     * 2. Not in direct mode (set by mo_logger_flush)
     * 3. Enqueue succeeds (queue not full)
     */
    if (len <= LOG_ENTRY_SZ - 1 && !mo_logger_direct_mode() &&
        mo_logger_enqueue(buf, len) == 0)
        return len; /* Successfully enqueued */

    /* Direct output: early boot, direct mode (post-flush), queue full, or too
     * long.
     */
    char *p = buf;
    while (*p)
        _putchar(*p++);

    return len;
}

/* Formatted output to a bounded string buffer (C99).
 * Guarantees null termination if size > 0.
 * Returns total chars that would be written (excluding null terminator).
 */
int32_t snprintf(char *str, size_t size, const char *fmt, ...)
{
    va_list args;
    int32_t v;

    va_start(args, fmt);
    v = vsnprintf(str, size, fmt, args);
    va_end(args);
    return v;
}

/* Writes a string to stdout, followed by a newline.
 * Thread-safe: Uses deferred logging via logger task.
 * Falls back to direct output during early boot, queue full, or after flush.
 * Same flush-aware behavior as printf() for ordered multi-line output.
 */
int32_t puts(const char *str)
{
    char buf[256]; /* Buffer for string + newline */
    int len = 0;

    /* Copy string to buffer */
    while (*str && len < 254)
        buf[len++] = *str++;
    buf[len++] = '\n';
    buf[len] = '\0';

    /* Try deferred logging only if not in direct mode */
    if (len <= LOG_ENTRY_SZ - 1 && !mo_logger_direct_mode() &&
        mo_logger_enqueue(buf, len) == 0)
        return 0; /* Successfully enqueued */

    /* Direct output: early boot, direct mode (post-flush), queue full, or too
     * long.
     */
    char *p = buf;
    while (*p)
        _putchar(*p++);

    return 0;
}

/* Reads a single character from stdin. */
int getchar(void)
{
    return _getchar(); /* Use HAL's getchar implementation. */
}

/* Reads a line from stdin.
 * FIXME: no buffer overflow protection */
char *gets(char *s)
{
    int32_t c;
    char *cs = s;

    /* Read characters until newline or end of input. */
    while ((c = _getchar()) != '\n' && c >= 0)
        *cs++ = c;

    /* If input ended unexpectedly and nothing was read, return null. */
    if (c < 0 && cs == s)
        return 0;

    *cs++ = '\0';

    return s;
}

/* Reads up to 'n' characters from stdin into buffer 's'. */
char *fgets(char *s, int n, void *f)
{
    int ch;
    char *p = s;

    /* Read characters until 'n-1' are read, or newline, or EOF. */
    while (n > 1) {
        ch = _getchar();
        *p++ = ch;
        n--;
        if (ch == '\n')
            break;
    }
    if (n)
        *p = '\0';

    return s;
}

/* Reads a line from stdin, with a buffer size limit. */
char *getline(char *s)
{
    int32_t c, i = 0;
    char *cs = s;

    /* Read characters until newline or EOF, or buffer limit is reached. */
    while ((c = _getchar()) != '\n' && c >= 0) {
        if (++i == 80) {
            *cs = '\0';
            break;
        }
        *cs++ = c;
    }
    /* If input ended unexpectedly and nothing was read, return null. */
    if (c < 0 && cs == s)
        return 0;

    *cs++ = '\0';

    return s;
}

/* User mode safe formatted output.
 * Formats string in user space using vsnprintf, then outputs via syscall.
 * This avoids privilege violations from printf's logger mutex operations.
 *
 * Safe for U-mode tasks because:
 * 1. vsnprintf runs in user space (no privileged operations)
 * 2. sys_tputs syscall transitions to M-mode for actual output
 * 3. Kernel handles logger mutex/direct output decision
 */
int32_t umode_printf(const char *fmt, ...)
{
    char buf[256]; /* Stack buffer for formatted output */
    va_list args;

    va_start(args, fmt);
    int32_t len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    /* Handle vsnprintf error */
    if (len < 0)
        return len;

    /* Output via syscall - safe for U-mode */
    extern int sys_tputs(const char *str);
    sys_tputs(buf);

    return len;
}
