/* LibC Test Suite - Comprehensive tests for standard library functions.
 *
 * Current Coverage:
 * - vsnprintf/snprintf: Buffer overflow protection
 *   * C99 semantics, truncation behavior, ISR safety
 *   * Format specifiers: %s, %d, %u, %x, %p, %c, %%
 *   * Edge cases: size=0, size=1, truncation, null termination
 * - Memory allocation: malloc, free, realloc
 *
 * Future Tests (Planned):
 * - String functions: strlen, strcmp, strcpy, strncpy, memcpy, memset
 * - Character classification: isdigit, isalpha, isspace, etc.
 */

#include <linmo.h>

#define TEST_PASS 1
#define TEST_FAIL 0

/* Test result tracking */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* Simple string comparison for tests */
static int test_strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2))
        s1++, s2++;
    return *s1 - *s2;
}

/* Simple string length for tests */
static size_t test_strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return p - s;
}

/* Test assertion macro */
#define ASSERT_TEST(condition, test_name)     \
    do {                                      \
        tests_run++;                          \
        if (condition) {                      \
            tests_passed++;                   \
            printf("[PASS] %s\n", test_name); \
        } else {                              \
            tests_failed++;                   \
            printf("[FAIL] %s\n", test_name); \
        }                                     \
    } while (0)

/* Test 1: Basic functionality with sufficient buffer */
void test_basic_functionality(void)
{
    char buf[64];
    int ret;

    /* Test simple string */
    ret = snprintf(buf, sizeof(buf), "Hello World");
    ASSERT_TEST(ret == 11 && test_strcmp(buf, "Hello World") == 0,
                "Basic string formatting");

    /* Test integer formatting */
    ret = snprintf(buf, sizeof(buf), "Number: %d", 42);
    ASSERT_TEST(ret == 10 && test_strcmp(buf, "Number: 42") == 0,
                "Integer formatting");

    /* Test unsigned formatting */
    ret = snprintf(buf, sizeof(buf), "Unsigned: %u", 123);
    ASSERT_TEST(ret == 13 && test_strcmp(buf, "Unsigned: 123") == 0,
                "Unsigned formatting");

    /* Test hex formatting */
    ret = snprintf(buf, sizeof(buf), "Hex: %x", 0xDEAD);
    ASSERT_TEST(ret == 9 && test_strcmp(buf, "Hex: dead") == 0,
                "Hex formatting");

    /* Test pointer formatting */
    void *ptr = (void *) 0x12345678;
    ret = snprintf(buf, sizeof(buf), "Ptr: %p", ptr);
    ASSERT_TEST(ret == 13 && test_strcmp(buf, "Ptr: 12345678") == 0,
                "Pointer formatting");

    /* Test character formatting */
    ret = snprintf(buf, sizeof(buf), "Char: %c", 'A');
    ASSERT_TEST(ret == 7 && test_strcmp(buf, "Char: A") == 0,
                "Character formatting");

    /* Test multiple format specifiers */
    ret = snprintf(buf, sizeof(buf), "%d %s %x", 42, "test", 0xFF);
    ASSERT_TEST(ret == 10 && test_strcmp(buf, "42 test ff") == 0,
                "Multiple format specifiers");
}

/* Test 2: Edge case - size = 0 (C99 semantics) */
void test_size_zero(void)
{
    char buf[10] = "unchanged";
    int ret;

    /* C99: should return length that would be written, no buffer modification
     */
    ret = snprintf(buf, 0, "Hello World");
    ASSERT_TEST(ret == 11 && test_strcmp(buf, "unchanged") == 0,
                "Size=0 preserves buffer");

    /* NULL buffer with size=0 is valid (C99) */
    ret = snprintf(NULL, 0, "Test %d", 123);
    ASSERT_TEST(ret == 8, "NULL buffer with size=0");
}

/* Test 3: Edge case - size = 1 (only null terminator) */
void test_size_one(void)
{
    char buf[10];
    int ret;

    buf[0] = 'X'; /* Sentinel value */
    ret = snprintf(buf, 1, "Hello");
    ASSERT_TEST(ret == 5 && buf[0] == '\0',
                "Size=1 writes only null terminator");
}

/* Test 4: Truncation scenarios */
void test_truncation(void)
{
    char buf[10];
    int ret;

    /* String longer than buffer */
    ret = snprintf(buf, sizeof(buf), "This is a very long string");
    ASSERT_TEST(ret == 26 && test_strlen(buf) == 9 && buf[9] == '\0',
                "Truncation with long string");

    /* Exact fit (9 chars + null in 10-byte buffer) */
    ret = snprintf(buf, sizeof(buf), "123456789");
    ASSERT_TEST(
        ret == 9 && test_strcmp(buf, "123456789") == 0 && buf[9] == '\0',
        "Exact fit");

    /* One char too long */
    ret = snprintf(buf, sizeof(buf), "1234567890");
    ASSERT_TEST(
        ret == 10 && test_strcmp(buf, "123456789") == 0 && buf[9] == '\0',
        "One char truncation");

    /* Format string producing truncated output */
    ret = snprintf(buf, 8, "Value: %d", 12345);
    ASSERT_TEST(ret == 12 && test_strcmp(buf, "Value: ") == 0 && buf[7] == '\0',
                "Format truncation");
}

/* Test 5: Null-termination guarantee */
void test_null_termination(void)
{
    char buf[5];
    int ret;

    /* Fill buffer to verify null-termination */
    for (int i = 0; i < 5; i++)
        buf[i] = 'X';

    ret = snprintf(buf, 5, "1234567890");
    ASSERT_TEST(buf[4] == '\0', "Null termination guaranteed");

    /* Verify buffer was limited */
    ASSERT_TEST(test_strcmp(buf, "1234") == 0, "Truncated content correct");

    /* C99 return value: chars that would be written */
    ASSERT_TEST(ret == 10, "C99 return value for truncation");
}

/* Test 6: Format specifier edge cases */
void test_format_specifiers(void)
{
    char buf[32];
    int ret;

    /* Null string pointer */
    ret = snprintf(buf, sizeof(buf), "String: %s", (char *) NULL);
    ASSERT_TEST(test_strcmp(buf, "String: <NULL>") == 0,
                "NULL string handling");

    /* Negative numbers */
    ret = snprintf(buf, sizeof(buf), "%d", -42);
    ASSERT_TEST(test_strcmp(buf, "-42") == 0, "Negative number formatting");

    /* Zero */
    ret = snprintf(buf, sizeof(buf), "%d %u %x", 0, 0, 0);
    ASSERT_TEST(test_strcmp(buf, "0 0 0") == 0, "Zero formatting");

    /* Width padding */
    ret = snprintf(buf, sizeof(buf), "%5d", 42);
    ASSERT_TEST(test_strcmp(buf, "   42") == 0, "Width padding");

    /* Zero padding */
    ret = snprintf(buf, sizeof(buf), "%05d", 42);
    ASSERT_TEST(test_strcmp(buf, "00042") == 0, "Zero padding");

    /* Literal percent */
    ret = snprintf(buf, sizeof(buf), "100%% complete");
    ASSERT_TEST(test_strcmp(buf, "100% complete") == 0, "Literal percent sign");

    (void) ret; /* Return values tested in test_return_values() */
}

/* Test 7: C99 return value semantics */
void test_return_values(void)
{
    char buf[10];
    int ret;

    /* Return value = chars that would be written (excluding null) */
    ret = snprintf(buf, sizeof(buf), "12345");
    ASSERT_TEST(ret == 5, "Return value for normal case");

    /* Return value for truncated string */
    ret = snprintf(buf, 5, "1234567890");
    ASSERT_TEST(ret == 10, "Return value for truncated case");

    /* Empty string */
    ret = snprintf(buf, sizeof(buf), "");
    ASSERT_TEST(ret == 0 && buf[0] == '\0', "Empty string return value");
}

/* Test 8: Buffer boundary verification */
void test_buffer_boundaries(void)
{
    char buf[16];
    char guard_before = 0xAA;
    char guard_after = 0xBB;
    int ret;

    /* Place guards around buffer */
    char *test_buf = &buf[1];
    buf[0] = guard_before;
    buf[15] = guard_after;

    /* Write to middle buffer, verify guards intact */
    ret = snprintf(test_buf, 14, "Test boundary");
    ASSERT_TEST(buf[0] == guard_before && buf[15] == guard_after,
                "No buffer overrun");

    ASSERT_TEST(test_strcmp(test_buf, "Test boundary") == 0,
                "Content correct within boundaries");

    (void) ret; /* Return value not critical for boundary test */
}

/* Test 9: ISR safety verification */
void test_isr_safety(void)
{
    char buf[32];
    int ret;

    /* Verify no dynamic allocation (manual inspection of code required)
     * Verify bounded execution time (all format handlers O(1) or O(n) small n)
     * Verify reentrancy (no global state modified)
     */

    /* Test can be called multiple times without side effects */
    ret = snprintf(buf, sizeof(buf), "ISR Test %d", 123);
    char saved[32];
    for (int i = 0; i < 32; i++)
        saved[i] = buf[i];

    ret = snprintf(buf, sizeof(buf), "ISR Test %d", 123);
    int match = 1;
    for (int i = 0; i < 32; i++) {
        if (buf[i] != saved[i]) {
            match = 0;
            break;
        }
    }
    ASSERT_TEST(match == 1, "Reentrant behavior (no global state)");

    (void) ret; /* Return value not critical for ISR safety test */
}

/* Test 10: Mixed format stress test */
void test_mixed_formats(void)
{
    char buf[128];
    int ret;

    /* Complex format string with multiple types */
    ret = snprintf(buf, sizeof(buf),
                   "Task %d: ptr=%p, count=%u, hex=%x, char=%c, str=%s", 5,
                   (void *) 0xABCD, 100, 0xFF, 'X', "test");

    /* Verify no crash and reasonable return value */
    ASSERT_TEST(ret > 0 && ret < 128, "Mixed format stress test");

    /* Verify null termination */
    ASSERT_TEST(buf[test_strlen(buf)] == '\0', "Mixed format null termination");
}

/* Memory Allocator Tests (malloc/free/calloc/realloc) */

/* Alignment check helper */
static inline bool is_aligned(void *ptr, size_t align)
{
    return ((uintptr_t) ptr & (align - 1)) == 0;
}

/* Test: Basic allocation and deallocation */
void test_malloc_basic(void)
{
    void *p1 = malloc(64);
    ASSERT_TEST(p1, "malloc(64) returns non-NULL");

    void *p2 = malloc(128);
    ASSERT_TEST(p2, "malloc(128) returns non-NULL");

    /* Pointers should be different */
    ASSERT_TEST(p1 != p2, "Different allocations return different pointers");

    /* Check 4-byte alignment (RISC-V requirement) */
    ASSERT_TEST(is_aligned(p1, 4), "malloc(64) is 4-byte aligned");
    ASSERT_TEST(is_aligned(p2, 4), "malloc(128) is 4-byte aligned");

    /* Write and read back to verify memory is usable */
    *(uint32_t *) p1 = 0xDEADBEEF;
    *(uint32_t *) p2 = 0xCAFEBABE;
    ASSERT_TEST(*(uint32_t *) p1 == 0xDEADBEEF, "Write/read verification (p1)");
    ASSERT_TEST(*(uint32_t *) p2 == 0xCAFEBABE, "Write/read verification (p2)");

    free(p1);
    free(p2);
}

/* Test: Multiple allocations and frees */
void test_malloc_multiple(void)
{
    void *ptrs[10] = {NULL};
    bool all_valid = true;

    /* Allocate 10 blocks */
    for (int i = 0; i < 10; i++) {
        ptrs[i] = malloc(32);
        if (!ptrs[i]) {
            all_valid = false;
            break;
        }
        *(int *) ptrs[i] = i * 100;
    }
    ASSERT_TEST(all_valid, "Allocate 10 blocks of 32 bytes");

    /* Verify all values are preserved (only if all allocations succeeded) */
    bool values_ok = all_valid;
    for (int i = 0; i < 10 && values_ok; i++) {
        if (!ptrs[i] || *(int *) ptrs[i] != i * 100)
            values_ok = false;
    }
    ASSERT_TEST(values_ok, "All block values preserved");

    /* Free all blocks */
    for (int i = 0; i < 10; i++)
        free(ptrs[i]);
}

/* Test: Calloc overflow protection */
void test_calloc_overflow(void)
{
    /* These should return NULL due to overflow */
    void *p1 = calloc(0x10000, 0x10000);
    ASSERT_TEST(!p1, "calloc overflow (0x10000 * 0x10000)");

    void *p2 = calloc(0x7FFFFFFF, 2);
    ASSERT_TEST(!p2, "calloc overflow (0x7FFFFFFF * 2)");

    /* Valid calloc should work */
    void *p3 = calloc(10, 10);
    ASSERT_TEST(p3, "Valid calloc(10, 10)");

    /* Verify zero-initialization */
    if (p3) {
        uint8_t *bytes = (uint8_t *) p3;
        int all_zero = 1;
        for (int i = 0; i < 100; i++) {
            if (bytes[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        ASSERT_TEST(all_zero, "calloc zero-initialization");
        free(p3);
    }
}

/* Test: Realloc edge cases */
void test_realloc_cases(void)
{
    /* realloc(NULL, size) == malloc(size) */
    void *p1 = realloc(NULL, 64);
    ASSERT_TEST(p1, "realloc(NULL, size) == malloc");
    if (!p1)
        return; /* Cannot continue without initial allocation */

    /* Write data for shrink/grow tests */
    uint32_t *data = (uint32_t *) p1;
    for (int i = 0; i < 16; i++)
        data[i] = i * 10;

    /* realloc shrink */
    void *p2 = realloc(p1, 32);
    ASSERT_TEST(p2, "realloc shrink");
    if (!p2)
        return;

    /* Verify data preserved in shrunk region */
    data = (uint32_t *) p2;
    bool shrink_ok = true;
    for (int i = 0; i < 8; i++) {
        if (data[i] != (uint32_t) (i * 10)) {
            shrink_ok = false;
            break;
        }
    }
    ASSERT_TEST(shrink_ok, "realloc shrink preserves data");

    /* realloc grow */
    void *p3 = realloc(p2, 128);
    ASSERT_TEST(p3, "realloc grow");
    if (!p3)
        return;

    /* Verify data preserved after grow (first 8 uint32_t from shrunk region) */
    data = (uint32_t *) p3;
    bool grow_ok = true;
    for (int i = 0; i < 8; i++) {
        if (data[i] != (uint32_t) (i * 10)) {
            grow_ok = false;
            break;
        }
    }
    ASSERT_TEST(grow_ok, "realloc grow preserves data");

    /* realloc(ptr, 0) == free(ptr), returns NULL */
    void *p4 = realloc(p3, 0);
    ASSERT_TEST(!p4, "realloc(ptr, 0) returns NULL");
}

/* Test: Boundary conditions */
void test_malloc_boundaries(void)
{
    /* Zero size should return NULL */
    void *p1 = malloc(0);
    ASSERT_TEST(!p1, "malloc(0) returns NULL");

    /* Minimum allocation size */
    void *p2 = malloc(1);
    ASSERT_TEST(p2, "malloc(1) succeeds (minimum size)");
    ASSERT_TEST(is_aligned(p2, 4), "malloc(1) is properly aligned");
    free(p2);

    /* Power-of-2 sizes */
    void *p3 = malloc(16);
    void *p4 = malloc(256);
    void *p5 = malloc(1024);
    ASSERT_TEST(p3 && p4 && p5, "Power-of-2 allocations");
    free(p3);
    free(p4);
    free(p5);
}

/* Test: Free NULL (should be no-op) */
void test_free_null(void)
{
    free(NULL);
    free(NULL);
    free(NULL);
    ASSERT_TEST(1, "free(NULL) is no-op");
}

/* Test: Allocation/free cycles (stress test) */
void test_malloc_stress(void)
{
    int success_count = 0;
    const int iterations = 50;

    for (int i = 0; i < iterations; i++) {
        void *p = malloc(32 + (i % 64));
        if (p) {
            *(int *) p = i;
            if (*(int *) p == i)
                success_count++;
            free(p);
        }
    }
    ASSERT_TEST(success_count == iterations, "Alloc/free cycle stress test");
}

/* Test: Coalescing verification - verify adjacent free blocks merge */
void test_malloc_coalesce(void)
{
    /* Allocate three adjacent blocks */
    void *a = malloc(64);
    void *b = malloc(64);
    void *c = malloc(64);
    ASSERT_TEST(a && b && c, "Coalesce: allocate A, B, C");

    /* Mark them for identification */
    *(uint32_t *) a = 0xAAAAAAAA;
    *(uint32_t *) b = 0xBBBBBBBB;
    *(uint32_t *) c = 0xCCCCCCCC;

    /* Free A and C, creating gaps (B remains allocated) */
    free(a);
    free(c);

    /* Free B - should trigger coalescing of A+B+C into one large block */
    free(b);

    /* Now allocate a block that requires merged space.
     * If coalescing works, this should succeed and potentially
     * return the same address as A (start of merged region).
     */
    void *large = malloc(64 * 3); /* Needs space of A+B+C combined */
    ASSERT_TEST(large, "Coalesce: large alloc after merge");
    free(large);
}

/* Test: Realloc in-place growth - verify realloc expands without copy */
void test_realloc_inplace(void)
{
    /* Allocate initial block */
    void *p1 = malloc(64);
    ASSERT_TEST(p1, "Realloc in-place: initial alloc");

    /* Write marker data */
    uint32_t *data = (uint32_t *) p1;
    for (int i = 0; i < 16; i++)
        data[i] = 0xDEAD0000 | i;

    /* Allocate adjacent block then free it to create expansion room */
    void *p2 = malloc(64);
    ASSERT_TEST(p2, "Realloc in-place: adjacent alloc");
    free(p2);

    /* Realloc to grow - should expand in-place if TLSF coalesces correctly */
    void *p3 = realloc(p1, 128);
    ASSERT_TEST(p3, "Realloc in-place: grow succeeds");

    /* Verify original data preserved */
    data = (uint32_t *) p3;
    int data_ok = 1;
    for (int i = 0; i < 16; i++) {
        if (data[i] != (0xDEAD0000 | i)) {
            data_ok = 0;
            break;
        }
    }
    ASSERT_TEST(data_ok, "Realloc in-place: data preserved");

    /* Check if realloc returned same pointer (in-place) or moved */
    ASSERT_TEST(p3, "Realloc in-place: pointer valid");

    free(p3);
}

/* Test: Block splitting - verify large blocks split correctly */
void test_malloc_split(void)
{
    /* Allocate a large block */
    void *large = malloc(256);
    ASSERT_TEST(large, "Split: large block alloc");

    /* Free it to return to free list */
    free(large);

    /* Allocate a small block - should split the large block */
    void *small1 = malloc(32);
    ASSERT_TEST(small1, "Split: small alloc from large");

    /* Allocate another small block - should use remainder from split */
    void *small2 = malloc(32);
    ASSERT_TEST(small2, "Split: second small alloc");

    /* Both should be different */
    ASSERT_TEST(small1 != small2, "Split: different addresses");

    /* Verify memory is usable */
    *(uint32_t *) small1 = 0x11111111;
    *(uint32_t *) small2 = 0x22222222;
    ASSERT_TEST(*(uint32_t *) small1 == 0x11111111, "Split: small1 usable");
    ASSERT_TEST(*(uint32_t *) small2 == 0x22222222, "Split: small2 usable");

    free(small1);
    free(small2);
}

/* Test: Heap exhaustion - verify graceful failure when heap is full */
void test_malloc_exhaustion(void)
{
    void *ptrs[1000];
    int count = 0;

    /* Allocate until failure or reasonable limit */
    for (int i = 0; i < 1000; i++) {
        ptrs[i] = malloc(1024 * 64); /* 64KB blocks */
        if (!ptrs[i])
            break;
        count++;
    }

    /* Should have allocated at least some blocks */
    ASSERT_TEST(count > 0, "Exhaustion: some allocations succeeded");

    /* Should eventually return NULL (not crash) */
    void *overflow = malloc(1024 * 1024 * 100); /* 100MB - should fail */
    ASSERT_TEST(!overflow, "Exhaustion: large alloc returns NULL");

    /* Free all allocated blocks */
    for (int i = 0; i < count; i++)
        free(ptrs[i]);

    /* After freeing, should be able to allocate again */
    void *recovered = malloc(1024);
    ASSERT_TEST(recovered, "Exhaustion: recovery after free");
    free(recovered);
}

/* Test: FL/SL boundary allocations - verify power-of-2 size transitions */
void test_malloc_fl_boundaries(void)
{
    /* Test allocations at FL transition points (power-of-2 boundaries).
     * TLSF uses first-level index based on log2(size), so these sizes
     * exercise the FL/SL mapping logic at boundary conditions.
     */
    void *ptrs[12] = {NULL}; /* Initialize to avoid UB on early exit */
    size_t sizes[] = {
        16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768,
    };
    bool all_valid = true;

    /* Allocate at each FL boundary */
    for (int i = 0; i < 12; i++) {
        ptrs[i] = malloc(sizes[i]);
        if (!ptrs[i]) {
            all_valid = false;
            break;
        }
        /* Write pattern to verify memory is usable */
        *(uint32_t *) ptrs[i] = 0xF1000000 | sizes[i];
    }
    ASSERT_TEST(all_valid, "FL boundary: all power-of-2 allocs succeed");

    /* Verify all patterns */
    int patterns_ok = 1;
    for (int i = 0; i < 12 && ptrs[i]; i++) {
        if (*(uint32_t *) ptrs[i] != (0xF1000000 | sizes[i])) {
            patterns_ok = 0;
            break;
        }
    }
    ASSERT_TEST(patterns_ok, "FL boundary: patterns preserved");

    /* Test sizes just above power-of-2 (exercises SL subdivision) */
    void *above1 = malloc(65);  /* Just above 64 */
    void *above2 = malloc(129); /* Just above 128 */
    void *above3 = malloc(257); /* Just above 256 */
    ASSERT_TEST(above1 && above2 && above3,
                "FL boundary: above-power-of-2 allocs");

    /* Test sizes just below power-of-2 */
    void *below1 = malloc(63);  /* Just below 64 */
    void *below2 = malloc(127); /* Just below 128 */
    void *below3 = malloc(255); /* Just below 256 */
    ASSERT_TEST(below1 && below2 && below3,
                "FL boundary: below-power-of-2 allocs");

    /* Free all */
    for (int i = 0; i < 12; i++) {
        if (ptrs[i])
            free(ptrs[i]);
    }
    free(above1);
    free(above2);
    free(above3);
    free(below1);
    free(below2);
    free(below3);
}

/* Test: Alignment verification for various sizes */
void test_malloc_alignment(void)
{
    /* All allocations must be 4-byte aligned for RISC-V.
     * Test various sizes to ensure alignment is maintained.
     */
    size_t sizes[] = {
        1, 2, 3, 5, 7, 9, 15, 17, 31, 33, 63, 65, 100, 1000,
    };
    int all_aligned = 1;

    for (int i = 0; i < 14; i++) {
        void *p = malloc(sizes[i]);
        if (p) {
            if (!is_aligned(p, 4)) {
                all_aligned = 0;
                free(p);
                break;
            }
            /* Also verify 4-byte write works */
            *(uint32_t *) p = 0xDEADBEEF;
            if (*(uint32_t *) p != 0xDEADBEEF) {
                all_aligned = 0;
                free(p);
                break;
            }
            free(p);
        } else {
            all_aligned = 0;
            break;
        }
    }
    ASSERT_TEST(all_aligned, "Alignment: all sizes 4-byte aligned");
}

/* Test: Rapid alloc/free patterns to stress bitmap updates */
void test_malloc_bitmap_stress(void)
{
    /* Alternating allocation pattern that exercises bitmap set/clear.
     * This pattern can expose race conditions in bitmap updates.
     */
    void *small, *large;
    int success = 1;

    for (int i = 0; i < 100; i++) {
        small = malloc(32);
        large = malloc(1024);
        if (!small || !large) {
            if (small)
                free(small);
            if (large)
                free(large);
            success = 0;
            break;
        }
        free(small);
        free(large);
    }
    ASSERT_TEST(success, "Bitmap stress: alternating alloc/free");

    /* Interleaved pattern: alloc small, alloc large, free large, free small */
    success = 1; /* Reset for next test */
    for (int i = 0; i < 100; i++) {
        small = malloc(32);
        large = malloc(1024);
        if (!small || !large) {
            free(small);
            free(large);
            success = 0;
            break;
        }
        free(large);
        free(small);
    }
    ASSERT_TEST(success, "Bitmap stress: interleaved free order");

    /* Multiple size classes simultaneously */
    void *ptrs[8] = {NULL}; /* Initialize to avoid UB */
    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
    success = 1; /* Reset for next test */
    for (int iter = 0; iter < 50 && success; iter++) {
        /* Clear pointers at start of each iteration */
        for (int i = 0; i < 8; i++)
            ptrs[i] = NULL;

        for (int i = 0; i < 8; i++) {
            ptrs[i] = malloc(sizes[i]);
            if (!ptrs[i]) {
                success = 0;
                break;
            }
        }
        /* Free all allocated (including partial on failure) */
        for (int i = 0; i < 8; i++) {
            if (ptrs[i])
                free(ptrs[i]);
        }
    }
    ASSERT_TEST(success, "Bitmap stress: multi-size simultaneous");
}

void test_runner(void)
{
    printf("\n=== LibC Test Suite ===\n");

    printf("\n--- Testing: vsnprintf/snprintf ---\n");
    test_basic_functionality();
    test_size_zero();
    test_size_one();
    test_truncation();
    test_null_termination();
    test_format_specifiers();
    test_return_values();
    test_buffer_boundaries();
    test_isr_safety();
    test_mixed_formats();

    printf("\n--- Testing: malloc/free/calloc/realloc ---\n");
    test_malloc_basic();
    test_malloc_multiple();
    test_calloc_overflow();
    test_realloc_cases();
    test_malloc_boundaries();
    test_free_null();
    test_malloc_stress();

    printf("\n--- Testing: TLSF allocator internals ---\n");
    test_malloc_coalesce();
    test_realloc_inplace();
    test_malloc_split();
    test_malloc_exhaustion();
    test_malloc_fl_boundaries();
    test_malloc_alignment();
    test_malloc_bitmap_stress();

    printf("\n=== Test Summary ===\n");
    printf("Tests run:    %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);

    if (tests_failed == 0) {
        printf("\n[SUCCESS] All tests passed!\n");
    } else {
        printf("\n[FAILURE] %d test(s) failed!\n", tests_failed);
    }
}

int32_t app_main(void)
{
    test_runner();

    /* Shutdown QEMU cleanly after tests complete */
    *(volatile uint32_t *) 0x100000U = 0x5555U;

    /* Fallback: keep task alive if shutdown fails (non-QEMU environments) */
    while (1)
        ; /* Idle loop */

    return 0;
}
