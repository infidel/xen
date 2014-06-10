#include <mini-os/os.h>
#include <mini-os/hypervisor.h>
#include <mini-os/events.h>
#include <mini-os/traps.h>
#include <mini-os/types.h>
#include <mini-os/time.h>
#include <mini-os/lib.h>

//#define VTIMER_DEBUG
#ifdef VTIMER_DEBUG
#define DEBUG(_f, _a...) \
    printk("MINI_OS(file=vtimer.c, line=%d) " _f , __LINE__, ## _a)
#else
#define DEBUG(_f, _a...)    ((void)0)
#endif


/************************************************************************
 * Time functions
 *************************************************************************/

static uint64_t cntvct_at_init;
static uint32_t counter_freq;

/* These are peridically updated in shared_info, and then copied here. */
struct shadow_time_info {
    uint64_t tsc_timestamp;     /* TSC at last update of time vals.  */
    uint64_t system_timestamp;  /* Time, in nanosecs, since boot.    */
    uint32_t tsc_to_nsec_mul;
    uint32_t tsc_to_usec_mul;
    int tsc_shift;
    uint32_t version;
};
static struct timespec shadow_ts;
static uint32_t shadow_ts_version;

static struct shadow_time_info shadow;

#define HANDLE_USEC_OVERFLOW(_tv)          \
    do {                                   \
        while ( (_tv)->tv_usec >= 1000000 ) \
        {                                  \
            (_tv)->tv_usec -= 1000000;      \
            (_tv)->tv_sec++;                \
        }                                  \
    } while ( 0 )

static inline int time_values_up_to_date(void)
{
    struct vcpu_time_info *src = &HYPERVISOR_shared_info->vcpu_info[0].time;

    return (shadow.version == src->version);
}



static void get_time_values_from_xen(void)
{
    struct vcpu_time_info    *src = &HYPERVISOR_shared_info->vcpu_info[0].time;

     do {
        shadow.version = src->version;
        rmb();
        shadow.tsc_timestamp     = src->tsc_timestamp;
        shadow.system_timestamp  = src->system_time;
        shadow.tsc_to_nsec_mul   = src->tsc_to_system_mul;
        shadow.tsc_shift         = src->tsc_shift;
        rmb();
    }
    while ((src->version & 1) | (shadow.version ^ src->version));

    shadow.tsc_to_usec_mul = shadow.tsc_to_nsec_mul / 1000;
}

static inline uint64_t read_virtual_count(void)
{
    uint32_t c_lo, c_hi;
    __asm__ __volatile__("isb;mrrc p15, 1, %0, %1, c14":"=r"(c_lo), "=r"(c_hi));
    return (((uint64_t) c_hi) << 32) + c_lo;
}

/* monotonic_clock(): returns # of nanoseconds passed since time_init()
 *        Note: This function is required to return accurate
 *        time even in the absence of multiple timer ticks.
 */
uint64_t monotonic_clock(void)
{
    uint64_t time = (1000000000 * (read_virtual_count() - cntvct_at_init)) / counter_freq;
    //printk("monotonic_clock: %llu (%llu)\n", time, NSEC_TO_SEC(time));
    return time;
}

static void update_wallclock(void)
{
    shared_info_t *s = HYPERVISOR_shared_info;

    do {
        shadow_ts_version = s->wc_version;
        rmb();
        shadow_ts.tv_sec  = s->wc_sec;
        shadow_ts.tv_nsec = s->wc_nsec;
        rmb();
    }
    while ((s->wc_version & 1) | (shadow_ts_version ^ s->wc_version));
}


int gettimeofday(struct timeval *tv, void *tz)
{
    uint64_t nsec = monotonic_clock();
    nsec += shadow_ts.tv_nsec;


    tv->tv_sec = shadow_ts.tv_sec;
    tv->tv_sec += NSEC_TO_SEC(nsec);
    tv->tv_usec = NSEC_TO_USEC(nsec % 1000000000UL);

    return 0;
}


void block_domain(s_time_t until)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ASSERT(irqs_disabled());
    if(monotonic_clock() < until)
    {
        HYPERVISOR_set_timer_op(until);
        HYPERVISOR_sched_op(SCHEDOP_block, 0);
        local_irq_disable();
    }
}


/*
 * Just a dummy
 */
void timer_handler(evtchn_port_t port, struct pt_regs *regs, void *ign)
{
    DEBUG("Timer kick\n");
    get_time_values_from_xen();
    update_wallclock();
}

#define VTIMER_TICK 0x10000000
void increment_vtimer_compare(uint64_t inc) {
    uint32_t x, y;
    uint64_t value;
    __asm__ __volatile__("mrrc p15, 1, %0, %1, c14\n"
            "isb":"=r"(x), "=r"(y));

    // CompareValue = Counter + VTIMER_TICK
    value = (0xFFFFFFFFFFFFFFFFULL & x) | ((0xFFFFFFFFFFFFFFFFULL & y) << 32);
    DEBUG("Counter: %llx(x=%x and y=%x)\n", value, x, y);
    value += inc;
    DEBUG("New CompareValue : %llx\n", value);
    x = 0xFFFFFFFFULL & value;
    y = (value >> 32) & 0xFFFFFFFF;

    __asm__ __volatile__("mcrr p15, 3, %0, %1, c14\n"
            "isb"::"r"(x), "r"(y));

    __asm__ __volatile__("mov %0, #0x1\n"
                "mcr p15, 0, %0, c14, c3, 1\n" /* Enable timer and unmask the output signal */
                "isb":"=r"(x));
}

static inline void enable_virtual_timer(void) {
#if FIXME
    uint32_t x, y;
    uint64_t value;

    __asm__ __volatile__("ldr %0, =0xffffffff\n"
            "ldr %1, =0xffffffff\n"
            "dsb\n"
            "mcrr p15, 3, %0, %1, c14\n" /* set CompareValue to 0x0000ffff 0000ffff */
            "isb\n"
            "mov %0, #0x1\n"
            "mcr p15, 0, %0, c14, c3, 1\n" /* Enable timer and unmask the output signal */
            "isb":"=r"(x), "=r"(y));
#else
    increment_vtimer_compare(VTIMER_TICK);
#endif
}

evtchn_port_t timer_port = -1;
void init_time(void)
{
    printk("Initialising timer interface\n");

    __asm__ __volatile__("mrc p15, 0, %0, c14, c0, 0":"=r"(counter_freq));
    cntvct_at_init = read_virtual_count();
    printk("Virtual Count register is %llx, freq = %d Hz\n", cntvct_at_init, counter_freq);

    timer_port = bind_virq(VIRQ_TIMER, (evtchn_handler_t)timer_handler, 0);
    if(timer_port == -1)
        BUG();
    unmask_evtchn(timer_port);

    enable_virtual_timer();
}

void fini_time(void)
{
    if(timer_port != -1)
    {
        mask_evtchn(timer_port);
        unbind_evtchn(timer_port);
    }
}
