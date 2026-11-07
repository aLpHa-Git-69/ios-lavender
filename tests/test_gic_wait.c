/* Actual pe_fiq.c bounded RWP helper, with a simulated distributor/clock.
 * This tests immediate clear, delayed clear and a permanently stuck RWP.
 * No architectural registers or device MMIO are accessed on the host. */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>

#define GICD_CTLR 0u
#define GICD_CTLR_RWP 0x80000000u

static uint64_t clock_ns;
static unsigned reads, clear_after;
static int panicked;
static jmp_buf panic_jump;

static void
nanoseconds_to_deadline(uint64_t ns, uint64_t *deadline)
{
	*deadline = clock_ns + ns;
}

static uint64_t
mach_absolute_time(void)
{
	clock_ns += 10000000u;
	return clock_ns;
}

static uint32_t
gicd_read32(uint32_t offset)
{
	assert(offset == GICD_CTLR);
	reads++;
	return reads >= clear_after ? 0x13u : 0x80000013u;
}

static _Noreturn void
panic(const char *format, const char *name, uint32_t status)
{
	(void)format;
	(void)name;
	assert(status == 0x80000013u);
	panicked = 1;
	longjmp(panic_jump, 1);
}

#include "gic_wait_impl.h"

static void
run_case(unsigned clear)
{
	clock_ns = 0;
	reads = 0;
	clear_after = clear;
	panicked = 0;
	if (!setjmp(panic_jump)) {
		gicd_wait_rwp();
	}
}

int
main(void)
{
	run_case(1);
	assert(!panicked && reads == 1 && clock_ns == 0);
	run_case(4);
	assert(!panicked && reads == 4 && clock_ns < 1000000000u);
	run_case(UINT32_MAX);
	assert(panicked && clock_ns == 1000000000u);
	assert(reads == 100);
	puts("test_gic_wait: 3 bounded-wait cases passed");
	return 0;
}
