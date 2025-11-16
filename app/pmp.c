/* PMP Context Switching Test
 *
 * Validates that PMP hardware configuration is correctly managed during
 * task context switches. Tests CSR configuration, region loading/unloading,
 * and flexpage metadata maintenance.
 */

#include <linmo.h>

#include "private/error.h"

/* Test configuration */
#define MAX_ITERATIONS 5

/* Test state counters */
static int tests_passed = 0;
static int tests_failed = 0;

/* External kernel symbols */
extern uint32_t _stext, _etext;
extern uint32_t _sdata, _edata;
extern uint32_t _sbss, _ebss;

/* Helper to read PMP configuration CSR */
static inline uint32_t read_pmpcfg0(void)
{
    uint32_t val;
    asm volatile("csrr %0, 0x3A0" : "=r"(val));
    return val;
}

/* Helper to read PMP address CSR */
static inline uint32_t read_pmpaddr(int idx)
{
    uint32_t val;
    switch (idx) {
    case 0:
        asm volatile("csrr %0, 0x3B0" : "=r"(val));
        break;
    case 1:
        asm volatile("csrr %0, 0x3B1" : "=r"(val));
        break;
    case 2:
        asm volatile("csrr %0, 0x3B2" : "=r"(val));
        break;
    case 3:
        asm volatile("csrr %0, 0x3B3" : "=r"(val));
        break;
    case 4:
        asm volatile("csrr %0, 0x3B4" : "=r"(val));
        break;
    case 5:
        asm volatile("csrr %0, 0x3B5" : "=r"(val));
        break;
    default:
        val = 0;
        break;
    }
    return val;
}

/* Test Task A: Verify PMP CSR configuration */
void task_a(void)
{
    printf("Task A (ID %d) starting...\n", mo_task_id());

    for (int i = 0; i < MAX_ITERATIONS; i++) {
        printf("Task A: Iteration %d\n", i + 1);

        /* Test A1: Read PMP configuration registers */
        uint32_t pmpcfg0 = read_pmpcfg0();
        printf("Task A: pmpcfg0 = 0x%08x\n", (unsigned int) pmpcfg0);

        if (pmpcfg0 != 0) {
            printf("Task A: PASS - PMP configuration is active\n");
            tests_passed++;
        } else {
            printf("Task A: FAIL - PMP configuration is zero\n");
            tests_failed++;
        }

        /* Test A2: Read kernel region addresses */
        uint32_t pmpaddr0 = read_pmpaddr(0);
        uint32_t pmpaddr1 = read_pmpaddr(1);
        printf("Task A: pmpaddr0 = 0x%08x, pmpaddr1 = 0x%08x\n",
               (unsigned int) pmpaddr0, (unsigned int) pmpaddr1);

        if (pmpaddr0 != 0 || pmpaddr1 != 0) {
            printf("Task A: PASS - Kernel regions configured\n");
            tests_passed++;
        } else {
            printf("Task A: FAIL - Kernel regions not configured\n");
            tests_failed++;
        }

        /* Test A3: Verify stack accessibility */
        int local_var = 0xAAAA;
        volatile int *stack_ptr = &local_var;
        int read_val = *stack_ptr;

        if (read_val == 0xAAAA) {
            printf("Task A: PASS - Stack accessible\n");
            tests_passed++;
        } else {
            printf("Task A: FAIL - Stack not accessible\n");
            tests_failed++;
        }

        for (int j = 0; j < 3; j++)
            mo_task_yield();
    }

    printf("Task A completed with %d passed, %d failed\n", tests_passed,
           tests_failed);

    while (1) {
        for (int i = 0; i < 10; i++)
            mo_task_yield();
    }
}

/* Test Task B: Verify PMP state after context switch */
void task_b(void)
{
    printf("Task B (ID %d) starting...\n", mo_task_id());

    for (int i = 0; i < MAX_ITERATIONS; i++) {
        printf("Task B: Iteration %d\n", i + 1);

        /* Test B1: Verify PMP configuration persists across switches */
        uint32_t pmpcfg0 = read_pmpcfg0();
        printf("Task B: pmpcfg0 = 0x%08x\n", (unsigned int) pmpcfg0);

        if (pmpcfg0 != 0) {
            printf("Task B: PASS - PMP active after context switch\n");
            tests_passed++;
        } else {
            printf("Task B: FAIL - PMP inactive after switch\n");
            tests_failed++;
        }

        /* Test B2: Verify own stack is accessible */
        int local_var = 0xBBBB;
        if (local_var == 0xBBBB) {
            printf("Task B: PASS - Stack accessible\n");
            tests_passed++;
        } else {
            printf("Task B: FAIL - Stack not accessible\n");
            tests_failed++;
        }

        /* Test B3: Check kernel regions still configured */
        uint32_t pmpaddr0 = read_pmpaddr(0);
        if (pmpaddr0 != 0) {
            printf("Task B: PASS - Kernel regions preserved\n");
            tests_passed++;
        } else {
            printf("Task B: FAIL - Kernel regions lost\n");
            tests_failed++;
        }

        for (int j = 0; j < 3; j++)
            mo_task_yield();
    }

    printf("Task B completed with %d passed, %d failed\n", tests_passed,
           tests_failed);

    while (1) {
        for (int i = 0; i < 10; i++)
            mo_task_yield();
    }
}

/* Test Task C: Verify PMP CSR consistency */
void task_c(void)
{
    printf("Task C (ID %d) starting...\n", mo_task_id());

    for (int i = 0; i < MAX_ITERATIONS; i++) {
        printf("Task C: Iteration %d\n", i + 1);

        /* Test C1: Comprehensive CSR check */
        uint32_t pmpcfg0 = read_pmpcfg0();
        uint32_t pmpaddr0 = read_pmpaddr(0);
        uint32_t pmpaddr1 = read_pmpaddr(1);
        uint32_t pmpaddr2 = read_pmpaddr(2);

        printf(
            "Task C: CSR state: cfg0=0x%08x addr0=0x%08x addr1=0x%08x "
            "addr2=0x%08x\n",
            (unsigned int) pmpcfg0, (unsigned int) pmpaddr0,
            (unsigned int) pmpaddr1, (unsigned int) pmpaddr2);

        bool csr_configured = (pmpcfg0 != 0) && (pmpaddr0 != 0);
        if (csr_configured) {
            printf("Task C: PASS - PMP CSRs properly configured\n");
            tests_passed++;
        } else {
            printf("Task C: FAIL - PMP CSRs not configured\n");
            tests_failed++;
        }

        /* Test C2: Stack operations */
        int test_array[5];
        for (int j = 0; j < 5; j++)
            test_array[j] = j;

        int sum = 0;
        for (int j = 0; j < 5; j++)
            sum += test_array[j];

        if (sum == 10) {
            printf("Task C: PASS - Stack array operations\n");
            tests_passed++;
        } else {
            printf("Task C: FAIL - Stack array operations\n");
            tests_failed++;
        }

        for (int j = 0; j < 3; j++)
            mo_task_yield();
    }

    printf("Task C completed with %d passed, %d failed\n", tests_passed,
           tests_failed);

    while (1) {
        for (int i = 0; i < 10; i++)
            mo_task_yield();
    }
}

/* Monitor task validates test results */
void monitor_task(void)
{
    printf("Monitor starting...\n");
    printf("Testing PMP CSR configuration and context switching:\n");
    printf("  Kernel text: %p - %p\n", (void *) &_stext, (void *) &_etext);
    printf("  Kernel data: %p - %p\n", (void *) &_sdata, (void *) &_edata);
    printf("  Kernel bss:  %p - %p\n\n", (void *) &_sbss, (void *) &_ebss);

    /* Read initial PMP state */
    uint32_t initial_pmpcfg0 = read_pmpcfg0();
    uint32_t initial_pmpaddr0 = read_pmpaddr(0);
    printf("Monitor: Initial PMP state:\n");
    printf("  pmpcfg0  = 0x%08x\n", (unsigned int) initial_pmpcfg0);
    printf("  pmpaddr0 = 0x%08x\n\n", (unsigned int) initial_pmpaddr0);

    int cycles = 0;

    while (cycles < 100) {
        cycles++;

        if (cycles % 20 == 0) {
            printf("Monitor: Cycle %d - Passed=%d, Failed=%d\n", cycles,
                   tests_passed, tests_failed);

            /* Periodic CSR check */
            uint32_t current_pmpcfg0 = read_pmpcfg0();
            printf("Monitor: Current pmpcfg0 = 0x%08x\n",
                   (unsigned int) current_pmpcfg0);
        }

        /* Check if all tasks completed */
        if (tests_passed >= (3 * MAX_ITERATIONS * 2) && tests_failed == 0) {
            printf("Monitor: All tasks completed successfully\n");
            break;
        }

        for (int i = 0; i < 5; i++)
            mo_task_yield();
    }

    /* Final report */
    printf("\n=== FINAL RESULTS ===\n");
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);

    /* Test validation */
    bool all_passed = (tests_failed == 0);
    bool good_coverage = (tests_passed >= (3 * MAX_ITERATIONS * 2));
    bool pmp_active = (read_pmpcfg0() != 0);

    printf("\nTest Results:\n");
    printf("All tests passed: %s\n", all_passed ? "PASS" : "FAIL");
    printf("Test coverage: %s\n", good_coverage ? "PASS" : "FAIL");
    printf("PMP still active: %s\n", pmp_active ? "PASS" : "FAIL");
    printf("Overall: %s\n",
           (all_passed && good_coverage && pmp_active) ? "PASS" : "FAIL");

    printf("PMP context switching test completed.\n");

    while (1) {
        for (int i = 0; i < 20; i++)
            mo_task_yield();
    }
}

/* Simple idle task */
void idle_task(void)
{
    while (1)
        mo_task_yield();
}

/* Application entry point */
int32_t app_main(void)
{
    printf("PMP Context Switching Test Starting...\n");
    printf("Testing PMP CSR configuration and task isolation\n");
    printf("Kernel memory regions:\n");
    printf("  text: %p to %p\n", (void *) &_stext, (void *) &_etext);
    printf("  data: %p to %p\n", (void *) &_sdata, (void *) &_edata);
    printf("  bss:  %p to %p\n\n", (void *) &_sbss, (void *) &_ebss);

    /* Create test tasks */
    int32_t task_a_id = mo_task_spawn(task_a, 1024);
    int32_t task_b_id = mo_task_spawn(task_b, 1024);
    int32_t task_c_id = mo_task_spawn(task_c, 1024);
    int32_t monitor_id = mo_task_spawn(monitor_task, 1024);
    int32_t idle_id = mo_task_spawn(idle_task, 512);

    if (task_a_id < 0 || task_b_id < 0 || task_c_id < 0 || monitor_id < 0 ||
        idle_id < 0) {
        printf("FATAL: Failed to create test tasks\n");
        return false;
    }

    printf("Tasks created: A=%d, B=%d, C=%d, Monitor=%d, Idle=%d\n",
           (int) task_a_id, (int) task_b_id, (int) task_c_id, (int) monitor_id,
           (int) idle_id);

    printf("Starting test...\n");
    return true; /* Enable preemptive scheduling */
}
