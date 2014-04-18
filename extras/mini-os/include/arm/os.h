#ifndef _OS_H_
#define _OS_H_

#ifndef __ASSEMBLY__

#include <mini-os/hypervisor.h>
#include <mini-os/types.h>
#include <mini-os/compiler.h>
#include <xen/xen.h>

void arch_fini(void);
void timer_handler(evtchn_port_t port, struct pt_regs *regs, void *ign);

extern void *device_tree;

#define BUG() while(1){asm volatile (".word 0xe7f000f0\n");} /* Undefined instruction; will call our fault handler. */

#define smp_processor_id() 0

#define barrier() __asm__ __volatile__("": : :"memory")

extern shared_info_t *HYPERVISOR_shared_info;

// disable interrupts
static inline void local_irq_disable(void) {
    __asm__ __volatile__("cpsid i":::"memory");
}

// enable interrupts
static inline void local_irq_enable(void) {
    __asm__ __volatile__("cpsie i":::"memory");
}

#define local_irq_save(x) { \
    __asm__ __volatile__("mrs %0, cpsr;cpsid i":"=r"(x)::"memory");    \
}

#define local_irq_restore(x) {    \
    __asm__ __volatile__("msr cpsr_c, %0"::"r"(x):"memory");    \
}

#define local_save_flags(x)    { \
    __asm__ __volatile__("mrs %0, cpsr":"=r"(x)::"memory");    \
}

static inline int irqs_disabled(void) {
    int x;
    local_save_flags(x);
    return x & 0x80;
}

/* We probably only need "dmb" here, but we'll start by being paranoid. */
#define mb() __asm__("dsb":::"memory");
#define rmb() __asm__("dsb":::"memory");
#define wmb() __asm__("dsb":::"memory");

/************************** arm *******************************/
#ifdef __INSIDE_MINIOS__
#if defined (__arm__)
#define xchg(ptr,v) __atomic_exchange_n(ptr, v, __ATOMIC_SEQ_CST)

/**
 * test_and_clear_bit - Clear a bit and return its old value
 * @nr: Bit to clear
 * @addr: Address to count from
 *
 * Note that @nr may be almost arbitrarily large; this function is not
 * restricted to acting on a single-word quantity.
 *
 * This operation is atomic.
 * If you need a memory barrier, use synch_test_and_clear_bit instead.
 */
static __inline__ int test_and_clear_bit(int nr, volatile void * addr)
{
    uint8_t *byte = ((uint8_t *)addr) + (nr >> 3);
    uint8_t bit = 1 << (nr & 7);
    uint8_t orig;

    orig = __atomic_fetch_and(byte, ~bit, __ATOMIC_RELAXED);

    return (orig & bit) != 0;
}

/**
 * Atomically set a bit and return the old value.
 * Similar to test_and_clear_bit.
 */
static __inline__ int test_and_set_bit(int nr, volatile void *base)
{
    uint8_t *byte = ((uint8_t *)base) + (nr >> 3);
    uint8_t bit = 1 << (nr & 7);
    uint8_t orig;

    orig = __atomic_fetch_or(byte, bit, __ATOMIC_RELAXED);

    return (orig & bit) != 0;
}

/**
 * Test whether a bit is set. */
static __inline__ int test_bit(int nr, const volatile unsigned long *addr)
{
    const uint8_t *ptr = (const uint8_t *) addr;
    return ((1 << (nr & 7)) & (ptr[nr >> 3])) != 0;
}

/**
 * Atomically set a bit in memory (like test_and_set_bit but discards result).
 */
static __inline__ void set_bit(int nr, volatile unsigned long *addr)
{
    test_and_set_bit(nr, addr);
}

/**
 * Atomically clear a bit in memory (like test_and_clear_bit but discards result).
 */
static __inline__ void clear_bit(int nr, volatile unsigned long *addr)
{
    test_and_clear_bit(nr, addr);
}

/**
 * __ffs - find first (lowest) set bit in word.
 * @word: The word to search
 *
 * Undefined if no bit exists, so code should check against 0 first.
 */
static __inline__ unsigned long __ffs(unsigned long word)
{
    int clz;

    /* xxxxx10000 = word
     * xxxxx01111 = word - 1
     * 0000011111 = word ^ (word - 1)
     *      4     = 31 - clz(word ^ (word - 1))
     */

    __asm__ (
        "sub r0, %[word], #1\n"
        "eor r0, r0, %[word]\n"
        "clz %[clz], r0\n":
        /* Outputs: */
        [clz] "=r"(clz):
        /* Inputs: */
        [word] "r"(word):
        /* Clobbers: */
        "r0");

    return 31 - clz;
}

#else /* ifdef __arm__ */
#error "Unsupported architecture"
#endif
#endif /* ifdef __INSIDE_MINIOS */

/********************* common arm32 and arm64  ****************************/

/* If *ptr == old, then store new there (and return new).
 * Otherwise, return the old value.
 * Atomic. */
#define synch_cmpxchg(ptr, old, new) \
({ __typeof__(*ptr) stored = old; \
   __atomic_compare_exchange_n(ptr, &stored, new, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? new : old; \
})

/* As test_and_clear_bit, but using __ATOMIC_SEQ_CST */
static __inline__ int synch_test_and_clear_bit(int nr, volatile void *addr)
{
    uint8_t *byte = ((uint8_t *)addr) + (nr >> 3);
    uint8_t bit = 1 << (nr & 7);
    uint8_t orig;

    orig = __atomic_fetch_and(byte, ~bit, __ATOMIC_SEQ_CST);

    return (orig & bit) != 0;
}

/* As test_and_set_bit, but using __ATOMIC_SEQ_CST */
static __inline__ int synch_test_and_set_bit(int nr, volatile void *base)
{
    uint8_t *byte = ((uint8_t *)base) + (nr >> 3);
    uint8_t bit = 1 << (nr & 7);
    uint8_t orig;

    orig = __atomic_fetch_or(byte, bit, __ATOMIC_SEQ_CST);

    return (orig & bit) != 0;
}

/* As set_bit, but using __ATOMIC_SEQ_CST */
static __inline__ void synch_set_bit(int nr, volatile void *addr)
{
    synch_test_and_set_bit(nr, addr);
}

/* As clear_bit, but using __ATOMIC_SEQ_CST */
static __inline__ void synch_clear_bit(int nr, volatile void *addr)
{
    synch_test_and_clear_bit(nr, addr);
}

/* As test_bit, but with a following memory barrier. */
static __inline__ int synch_test_bit(int nr, volatile void *addr)
{
    int result;
    result = test_bit(nr, addr);
    barrier();
    return result;
}

#endif /* not assembly */

#endif
