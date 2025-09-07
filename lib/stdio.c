/* libc: standard I/O functions.
 *
 * Default handlers do nothing (or return error codes) so the kernel can run
 * even if the board code forgets to install real console hooks. These hooks
 * allow a consistent I/O interface regardless of the underlying hardware.
 */

#include <lib/libc.h>
#include <stdarg.h>
#include <spinlock.h>

#include "private/stdio.h"

static spinlock_t printf_lock = SPINLOCK_INITIALIZER;
static uint32_t printf_flags = 0;

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

/* Emits a single character and increments the total character count. */
static inline void printchar(char **str, int32_t c, int *len)
{
    if (str) {
        **str = c;
        ++(*str);
    } else if (c) {
        _putchar(c);
    }
    (*len)++;
}

/* Main formatted string output function. */
static int vsprintf(char **buf, const char *fmt, va_list args)
{
    char **p = buf;
    const char *str;
    char pad;
    int width;
    int base;
    int sign;
    int i;
    long num;
    int len = 0;
    char tmp[32];

    /* The digits string for number conversion. */
    const char *digits = "0123456789abcdef";

    /* Iterate through the format string. */
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            printchar(p, *fmt, &len);
            continue;
        }
        /* Process format specifier: '%' */
        ++fmt; /* Move past '%'. */

        /* Get flags: padding character. */
        pad = ' '; /* Default padding is space. */
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        /* Get width: minimum field width. */
        width = -1;
        if (isdigit(*fmt))
            width = toint(&fmt);

        base = 10; /* Default base for numbers is decimal. */
        sign = 0;  /* Default is unsigned. */

        /* Handle format specifiers. */
        switch (*fmt) {
        case 'c': /* Character */
            printchar(p, (char) va_arg(args, int), &len);
            continue;
        case 's': /* String */
            str = va_arg(args, char *);
            if (str == 0) /* Handle NULL string. */
                str = "<NULL>";

            /* Print string, respecting width. */
            for (; *str && width != 0; str++, width--)
                printchar(p, *str, &len);

            /* Pad if necessary. */
            while (width-- > 0)
                printchar(p, pad, &len);
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
            width = sizeof(size_t);
            break;
        default: /* Unknown format specifier, ignore. */
            continue;
        }

        /* Handle sign for signed integers. */
        if (sign && num < 0) {
            num = -num;
            printchar(p, '-', &len);
            width--;
        }

        /* Convert number to string (in reverse order). */
        i = 0;
        if (num == 0)
            tmp[i++] = '0';
        else if (base == 10)
            __str_base10(num, tmp, &i);
        else {
            while (num != 0)
                tmp[i++] = digits[divide(&num, base)];
        }

        /* Pad with leading characters if width is specified. */
        width -= i;
        while (width-- > 0)
            printchar(p, pad, &len);

        /* Print the number string in correct order. */
        while (i-- > 0)
            printchar(p, tmp[i], &len);
    }
    printchar(p, '\0', &len);

    return len;
}

/* Formatted output to stdout. */
int32_t printf(const char *fmt, ...)
{
    va_list args;
    int32_t v;

    spin_lock_irqsave(&printf_lock, &printf_flags);
    va_start(args, fmt);
    v = vsprintf(0, fmt, args);
    va_end(args);
    spin_unlock_irqrestore(&printf_lock, printf_flags);
    return v;
}

/* Formatted output to a string. */
int32_t sprintf(char *out, const char *fmt, ...)
{
    va_list args;
    int32_t v;

    va_start(args, fmt);
    v = vsprintf(&out, fmt, args);
    va_end(args);
    return v;
}

/* Writes a string to stdout, followed by a newline. */
int32_t puts(const char *str)
{
    while (*str)
        _putchar(*str++);
    _putchar('\n');

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
