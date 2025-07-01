#include <lib/libc.h>
#include <stdarg.h>
#include <spinlock.h>

#include "private/stdio.h"
#include "private/utils.h"

/* Checks for any zero byte in a 32-bit word. */
static inline int byte_is_zero(uint32_t v)
{
    /* bitwise check for zero bytes. */
    return ((v - 0x01010101u) & ~v & 0x80808080u) != 0;
}

/* Checks if a 32-bit word @w matches the pattern @pat in all bytes. */
static inline int byte_is_match(uint32_t w, uint32_t pat)
{
    uint32_t t = w ^ pat; /* t will be zero for matching bytes. */
    /* Similar logic to byte_is_zero, but applied to the XORed result. */
    return ((t - 0x01010101u) & ~t & 0x80808080u) != 0;
}

/* strlen that scans by words whenever possible for efficiency. */
size_t strlen(const char *s)
{
    const char *p = s;

    /* Align pointer to word boundary (4 bytes) */
    while ((uint32_t) p & 3) {
        if (!*p) /* If null terminator is found byte-by-byte */
            return (size_t) (p - s);
        p++;
    }

    /* Word scan: Iterate through 32-bit words as long as no byte is zero. */
    const uint32_t *w = (const uint32_t *) p;
    while (!byte_is_zero(*w))
        w++;

    /* Final byte scan: Within the word that contained the zero byte, find the
     * exact position.
     */
    p = (const char *) w;
    while (*p) /* Scan byte-by-byte until the null terminator. */
        p++;
    return (size_t) (p - s); /* Return total length. */
}

void *memcpy(void *dst, const void *src, uint32_t len)
{
    uint8_t *d8 = dst;
    const uint8_t *s8 = src;

    /* If source and destination are aligned to the same word boundary */
    if (((uint32_t) d8 & 3) == ((uint32_t) s8 & 3)) {
        /* Copy initial bytes until destination is word-aligned. */
        uint32_t bound = ALIGN4(d8);
        while (len && (uint32_t) d8 < bound) {
            *d8++ = *s8++;
            len--;
        }

        /* Word-aligned copy */
        uint32_t *d32 = (uint32_t *) d8;
        const uint32_t *s32 = (const uint32_t *) s8;
        while (len >= 4) {
            *d32++ = *s32++;
            len -= 4;
        }

        /* Back to byte copy for any remaining bytes */
        d8 = (uint8_t *) d32;
        s8 = (const uint8_t *) s32;
    }

    /* Byte-by-byte copy for any remaining bytes or if not word-aligned */
    while (len--)
        *d8++ = *s8++;
    return dst;
}

void *memmove(void *dst, const void *src, uint32_t len)
{
    /* If no overlap, use memcpy */
    if (dst <= src || (uintptr_t) dst >= (uintptr_t) src + len)
        return memcpy(dst, src, len);

    /* Otherwise, copy backwards to handle overlap */
    uint8_t *d8 = (uint8_t *) dst + len;
    const uint8_t *s8 = (const uint8_t *) src + len;

    /* If source and destination are aligned to the same word boundary */
    if (((uint32_t) d8 & 3) == ((uint32_t) s8 & 3)) {
        /* Copy initial bytes backwards until destination is word-aligned. */
        uint32_t bound = ALIGN4(d8);
        while (len && (uint32_t) d8 > bound) {
            *--d8 = *--s8;
            len--;
        }

        /* Word-aligned copy backwards */
        uint32_t *d32 = (uint32_t *) d8;
        const uint32_t *s32 = (const uint32_t *) s8;
        while (len >= 4) {
            *--d32 = *--s32;
            len -= 4;
        }

        /* Back to byte copy for any remaining bytes */
        d8 = (uint8_t *) d32;
        s8 = (const uint8_t *) s32;
    }

    /* Byte-by-byte copy backwards for any remaining bytes */
    while (len--)
        *--d8 = *--s8;
    return dst;
}

void *memset(void *dst, int32_t c, uint32_t len)
{
    uint8_t *d8 = dst;
    /* Create a 32-bit word filled with the character 'c' */
    uint32_t word = (uint8_t) c;
    word |= word << 8;
    word |= word << 16;

    /* Copy initial bytes until destination is word-aligned. */
    uint32_t bound = ALIGN4(d8);
    while (len && (uint32_t) d8 < bound) {
        *d8++ = (uint8_t) c;
        len--;
    }

    /* Word-aligned fill */
    uint32_t *d32 = (uint32_t *) d8;
    while (len >= 4) {
        *d32++ = word;
        len -= 4;
    }

    /* Byte-by-byte fill for any remaining bytes */
    d8 = (uint8_t *) d32;
    while (len--)
        *d8++ = (uint8_t) c;
    return dst;
}

char *strcpy(char *dst, const char *src)
{
    char *ret = dst;
    while ((*dst++ = *src++) != 0)
        ;
    return ret;
}

char *strncpy(char *dst, const char *src, int32_t n)
{
    char *ret = dst;
    /* Copy up to @n characters or until the null terminator. */
    while (n && (*dst++ = *src++) != 0)
        n--;

    /* Pad with null bytes if @n is not exhausted and source was shorter. */
    while (n--)
        *dst++ = 0;

    return ret;
}

char *strcat(char *dst, const char *src)
{
    char *ret = dst;
    /* Advance to the end of the destination string. */
    while (*dst)
        dst++;

    /* Append the source string. */
    while ((*dst++ = *src++) != 0)
        ;

    return ret;
}

char *strncat(char *dst, const char *src, int32_t n)
{
    char *ret = dst;

    /* Advance to the end of the destination string. */
    while (*dst)
        dst++;

    /* Copy up to @n bytes from src, or until a null terminator is found. */
    while (n-- && (*dst = *src++) != 0)
        dst++;

    /* Ensure the result is null-terminated. */
    *dst = 0;
    return ret;
}

/* Check if two words are equal
 * Return true if XOR is zero (all bytes match)
 */
static inline int equal_word(uint32_t a, uint32_t b)
{
    return (a ^ b) == 0;
}

/* Word-oriented string comparison. */
int32_t strcmp(const char *s1, const char *s2)
{
    /* Align pointers to word boundary */
    while (((uint32_t) s1 & 3) && *s1 && *s1 == *s2) {
        s1++;
        s2++;
    }

    /* If alignment causes one to hit null or inequality first */
    if (((uint32_t) s1 & 3) == ((uint32_t) s2 & 3)) {
        const uint32_t *w1 = (const uint32_t *) s1;
        const uint32_t *w2 = (const uint32_t *) s2;

        /* Word comparison loop */
        for (;; ++w1, ++w2) {
            uint32_t v1 = *w1;
            uint32_t v2 = *w2;

            /* Exit if words differ or if a zero byte is found in either word */
            if (!equal_word(v1, v2) || byte_is_zero(v1)) {
                s1 = (const char *) w1;
                s2 = (const char *) w2;
                break;
            }
        }
    }

    /* Final byte comparison until null terminator or difference */
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }

    /* Return the difference between the first differing bytes */
    return (int32_t) ((unsigned char) *s1 - (unsigned char) *s2);
}

/* Word-oriented string comparison, up to 'n' characters. */
int32_t strncmp(const char *s1, const char *s2, int32_t n)
{
    if (n == 0) /* If n is 0, strings are considered equal. */
        return 0;

    /* Align pointers to word boundary */
    while (((uint32_t) s1 & 3) && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }

    /* If alignment causes one to hit null or inequality, or n becomes 0 */
    if (n == 0 || *s1 != *s2)
        goto tail;

    /* Word comparison loop */
    if (((uint32_t) s1 & 3) == ((uint32_t) s2 & 3)) {
        const uint32_t *w1 = (const uint32_t *) s1;
        const uint32_t *w2 = (const uint32_t *) s2;

        /* Compare words as long as n >= 4 and bytes within words match */
        while (n >= 4) {
            uint32_t v1 = *w1;
            uint32_t v2 = *w2;

            /* Exit if words differ or if a zero byte is found */
            if (!equal_word(v1, v2) || byte_is_zero(v1))
                break;

            w1++;
            w2++;
            n -= 4;
        }
        s1 = (const char *) w1;
        s2 = (const char *) w2;
    }

tail: /* Fallback for byte comparison or if word comparison was skipped */
    while (n && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }

    /* Return difference, or 0 if n reached 0 */
    return n ? (int32_t) ((unsigned char) *s1 - (unsigned char) *s2) : 0;
}

/* Locates the first occurrence of a character 'c' in string 's'. */
char *strchr(const char *s, int32_t c)
{
    uint8_t ch = (uint8_t) c;
    /* Create a 32-bit pattern for byte matching */
    uint32_t pat = 0x01010101u * ch;

    /* Byte-by-byte scan until word-aligned */
    while (((uint32_t) s & 3)) {
        if (*s == ch || *s == 0) /* Found char or end of string */
            return (*s == ch) ? (char *) s : 0;
        s++;
    }

    /* Word scan: Iterate through words, checking for character match or zero
     * byte.
     */
    const uint32_t *w = (const uint32_t *) s;
    for (;; ++w) {
        uint32_t v = *w;
        /* Exit if word contains zero or matches the pattern */
        if (byte_is_zero(v) || byte_is_match(v, pat)) {
            s = (const char *) w;  /* Reset to start of word */
            while (*s && *s != ch) /* Byte scan within the word */
                s++;

            return (*s == ch) ? (char *) s : 0; /* Return if found */
        }
    }
}

/* Locates the first occurrence of any character from @set in string @s. */
char *strpbrk(const char *s, const char *set)
{
    /* Build a 256-bit bitmap (eight 32-bit words) for characters in 'set'. */
    uint32_t map[8] = {0}; /* Initialize bitmap to all zeros. */
    while (*set) {
        uint8_t ch = (uint8_t) *set++;
        map[ch >> 5] |=
            1u << (ch & 31); /* Set the bit corresponding to the character. */
    }

    /* Scan the string @s */
    while (*s) {
        uint8_t ch = (uint8_t) *s;
        /* Check if the character's bit is set in the bitmap. */
        if (map[ch >> 5] & (1u << (ch & 31)))
            return (char *) s; /* Found a character from @set. */
        s++;
    }

    return 0;
}

/* Splits string 's' into tokens by delimiters in @delim. */
char *strsep(char **pp, const char *delim)
{
    char *p = *pp;
    if (!p)
        return 0;

    /* Find the first delimiter character in the current string segment. */
    char *q = strpbrk(p, delim);
    if (q) {
        *q = 0;
        *pp = q + 1;
    } else
        *pp = 0;
    return p;
}

/* Classic non-re-entrant tokenizer. Uses a static buffer for state. */
char *strtok(char *s, const char *delim)
{
    static char *last;
    if (s == 0)
        s = last;
    if (!s)
        return 0;

    /* Skip leading delimiters. */
    while (*s && strpbrk(&*s, delim) == &*s)
        *s++ = 0;
    if (*s == 0) {
        last = 0;
        return 0;
    }

    char *tok = s;
    /* Advance to next delimiter or end of string. */
    while (*s && !strpbrk(&*s, delim))
        s++;
    if (*s) {
        *s++ = 0;
        last = s;
    } else
        last = 0;
    return tok;
}

/* Re-entrant version of strtok. */
char *strtok_r(char *s, const char *delim, char **save)
{
    if (s == 0)
        s = *save;
    if (!s)
        return 0;

    /* Skip leading delimiters. */
    while (*s && strpbrk(&*s, delim) == &*s)
        *s++ = 0;
    if (*s == 0) {
        *save = 0;
        return 0;
    }

    char *tok = s;
    /* Advance to the next delimiter or end of string. */
    while (*s && !strpbrk(&*s, delim))
        s++;
    if (*s) {
        *s++ = 0;
        *save = s;
    } else
        *save = 0;
    return tok;
}

/* Converts string @s to an integer. */
int32_t strtol(const char *s, char **end, int32_t base)
{
    int32_t i;
    uint32_t ch, value = 0, neg = 0;

    /* Handle optional sign. */
    if (s[0] == '-') {
        neg = 1;
        ++s;
    }

    /* Handle common base prefixes (0x for hex). */
    if (s[0] == '0' && s[1] == 'x') {
        base = 16;
        s += 2;
    }

    /* Convert digits based on the specified base. */
    for (i = 0; i <= 8; ++i) {
        ch = *s++;
        if ('0' <= ch && ch <= '9')
            ch -= '0';
        else if ('A' <= ch && ch <= 'Z')
            ch = ch - 'A' + 10;
        else if ('a' <= ch && ch <= 'z')
            ch = ch - 'a' + 10;
        else
            break;
        value = value * base + ch;
    }

    if (end)
        *end = (char *) s - 1;
    if (neg)
        value = -(int32_t) value;

    return value;
}

/* Base-10 string conversion without division or multiplication */
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

/* Handle signed integers */
static char *__str_base10_signed(int32_t value, char *buffer, int *length)
{
    if (value < 0) {
        buffer[0] = '-';
        __str_base10((uint32_t) (-value), buffer + 1, length);
        (*length)++;
        return buffer;
    }
    return __str_base10((uint32_t) value, buffer, length);
}

/* Converts string @s to an integer. */
int32_t atoi(const char *s)
{
    int32_t n, f;

    n = 0;
    f = 0; /* Flag for sign. */

    /* Skip leading whitespace and handle optional sign. */
    for (;; s++) {
        switch (*s) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            continue; /* Skip whitespace. */
        case '-':
            f++; /* Set negative flag. */
            __attribute__((fallthrough));
        case '+':
            s++; /* Skip '+' sign. */
        }
        break;
    }

    /* Convert digits to integer. */
    while (*s >= '0' && *s <= '9')
        n = n * 10 + *s++ - '0';

    return (f ? -n : n);
}

/* Converts integer @i to an ASCII string @s in the given @base. */
void itoa(int32_t i, char *s, int32_t base)
{
    char c;
    char *p = s;
    char *q = s;
    uint32_t h;
    int32_t len;

    if (base == 16) { /* Hexadecimal conversion */
        h = (uint32_t) i;
        do {
            *q++ = '0' + (h % base);
        } while (h /= base); /* Continue until number becomes 0. */

        if ((i >= 0) && (i < 16)) /* Special case for small positive numbers */
            *q++ = '0';

        /* Reverse the string (digits are collected in reverse order). */
        for (*q = 0; p <= --q; p++) {
            /* Convert digit character if needed (e.g., 'a'-'f'). */
            (*p > '9') ? (c = *p + 39) : (c = *p); /* ASCII 'a' is '0'+39 */
            /* Swap characters. */
            (*q > '9') ? (*p = *q + 39) : (*p = *q);
            *q = c;
        }
    } else if (base == 10) { /* Decimal conversion */
        __str_base10_signed(i, s, &len);

        /* Reverse the string. */
        q = s + len;
        for (*q = 0; p <= --q; p++) {
            c = *p;
            *p = *q;
            *q = c;
        }
    } else { /* Other bases */
        if (i >= 0) {
            do {
                *q++ = '0' + (i % base);
            } while (i /= base);
        } else {
            *q++ = '-';
            p++;
            do {
                *q++ = '0' - (i % base);
            } while (i /= base);
        }

        /* Reverse the string. */
        for (*q = 0; p <= --q; p++) {
            c = *p;
            *p = *q;
            *q = c;
        }
    }
}

/* Compares two memory blocks byte by byte. */
int32_t memcmp(const void *cs, const void *ct, uint32_t n)
{
    char *r1 = (char *) cs;
    char *r2 = (char *) ct;

    /* Compare bytes until a difference is found or n bytes are processed. */
    while (n && (*r1 == *r2)) {
        ++r1;
        ++r2;
        --n;
    }

    /* Return 0 if all n bytes matched, otherwise the difference of the first
     * mismatching bytes.
     */
    return (n == 0) ? 0 : ((*r1 < *r2) ? -1 : 1);
}

/* Returns the absolute value of an integer. */
int32_t abs(int32_t n)
{
    return n >= 0 ? n : -n;
}

/* Random Number Generation */

/* Global state for the legacy random() function. */
static struct random_data _g_rand_data = {0xBAADF00Du};

/* xorshift32 PRNG step function: updates state and returns next value. */
static inline uint32_t prng_step(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Seeds the global random number generator. Seed 0 is remapped to 1. */
void srand(uint32_t seed)
{
    _g_rand_data.state = seed ? seed : 1U;
}

/* Legacy interface:
 * returns a pseudo-random value in [0, RAND_MAX] using the
 * global state.
 */
int32_t random(void)
{
    return (int32_t) ((prng_step(&_g_rand_data.state) >> 17) & RAND_MAX);
}

/* Re-entrant random number generator.
 * @buf    – pointer to caller-supplied state (must have been seeded).
 * @result – where to store the next value in [0, RAND_MAX].
 * Returns 0 on success, -1 on bad pointer.
 */
int random_r(struct random_data *buf, int32_t *result)
{
    if (!buf || !result)
        return -1;

    if (buf->state == 0)
        buf->state = 1u;

    /* Compute and store the next random value. */
    *result = (int32_t) ((prng_step(&buf->state) >> 17) & RAND_MAX);
    return 0; /* Success. */
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

/* printf() / sprintf() helper functions and implementation */

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


static spinlock_t printf_lock = SPINLOCK_INITIALIZER;
static uint32_t printf_flags = 0;
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
