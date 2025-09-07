#pragma once

#include <hal.h>

/* Spinlock structure */
typedef struct {
    volatile uint32_t lock;
} spinlock_t;

#define SPINLOCK_INITIALIZER { 0 }

/* Save and restore interrupt state */
static inline uint32_t intr_save(void)
{
    uint32_t mstatus_val = read_csr(mstatus);
    _di();
    return mstatus_val;
}

static inline void intr_restore(uint32_t mstatus_val)
{
    write_csr(mstatus, mstatus_val);
}

/* CPU relax */
static inline void cpu_relax(void)
{
    asm volatile("nop");
}

/* Basic spinlock API */
static inline void spin_lock(spinlock_t *lock)
{
    while (__sync_lock_test_and_set(&lock->lock, 1)) {
        while (lock->lock)
            cpu_relax();
    }
}

static inline void spin_unlock(spinlock_t *lock)
{
    __sync_lock_release(&lock->lock);
}

static inline int spin_trylock(spinlock_t *lock)
{
    return (__sync_lock_test_and_set(&lock->lock, 1) == 0);
}

/* IRQ-safe spinlock (no state saving) */
static inline void spin_lock_irq(spinlock_t *lock)
{
    _di();
    spin_lock(lock);
}

static inline void spin_unlock_irq(spinlock_t *lock)
{
    spin_unlock(lock);
    _ei();
}

/* IRQ-safe spinlock (with state saving) */
static inline void spin_lock_irqsave(spinlock_t *lock, uint32_t *flags)
{
    *flags = intr_save();
    spin_lock(lock);
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint32_t flags)
{
    spin_unlock(lock);
    intr_restore(flags);
}
