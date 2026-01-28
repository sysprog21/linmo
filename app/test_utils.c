/* LibC Test Suite - Comprehensive tests for standard library functions.
 *
 * Current Coverage:
 * - vsnprintf/snprintf: Buffer overflow protection
 *   * C99 semantics, truncation behavior, ISR safety
 *   * Format specifiers: %s, %d, %u, %x, %p, %c, %%
 *   * Edge cases: size=0, size=1, truncation, null termination
 *
 * Future Tests (Planned):
 * - String functions: strlen, strcmp, strcpy, strncpy, memcpy, memset
 * - Memory allocation: malloc, free, realloc
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

/* Test 11: List helpers behavior */
typedef struct {
    int val;
    list_node_t node;
} list_node_item_t;

void test_list_pushback_and_remove(void)
{
    list_t *list = list_create();

    list_node_item_t first = {.node.next = NULL, .val = 1};
    list_node_item_t second = {.node.next = NULL, .val = 2};
    list_node_item_t third = {.node.next = NULL, .val = 3};

    /* Check node push back normally - unlinked and linked */
    list_pushback(list, &first.node);
    ASSERT_TEST(list->length == 1, "Push back first node ");

    list_pushback(list, &second.node);
    list_node_item_t *item =
        container_of(list->head->next, list_node_item_t, node);
    ASSERT_TEST(list->length == 2 && item->val == 1,
                "Push back second node and order preserved ");


    list_pushback(list, &third.node);
    item = container_of(list->head->next->next->next, list_node_item_t, node);
    ASSERT_TEST(list->length == 3 && item->val == 3, "Push back third node ");

    /* Remove second node */
    list_remove(list, &second.node);
    item = container_of(list->head->next, list_node_item_t, node);
    ASSERT_TEST(list->length == 2 && item->val == 1, "Remove second node ");

    /* Remove non-existing node (second time) */

    item = container_of(list_pop(list), list_node_item_t, node);
    ASSERT_TEST(list->length == 1 && item->val == 1, "Pop node ");

    list_clear(list);
    ASSERT_TEST(list_is_empty(list), "List is cleared ");
}

void test_runner(void)
{
    printf("\n=== LibC Test Suite ===\n");
    printf("Testing: vsnprintf/snprintf\n\n");

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
    test_list_pushback_and_remove();

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
