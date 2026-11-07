/* Register-level mock for the CURRENT patched kernel's SDHCI functions.
 * The older test_sdhci_mock remains the historical v4 regression suite.
 * This file supplies hardware/platform behavior, not another PIO driver.
 * All register, card and block data below live in process memory. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC 1000000000ULL
typedef uintptr_t vm_offset_t;

#include "sdhci_mock_regs.h"

static uint8_t registers[0x1000];
static uint32_t bounce[256], card_data[256], written[256];
static struct {
	vm_offset_t dma;
	uint64_t dma_pa, last_cmd_ns, first_block, nblocks;
	uint32_t sec_count, rca;
	char part[64];
	bool writable, ready, cache_valid, write_pending;
	uint8_t cache_ctrl;
} se;
static bool santoni_data_moved, santoni_sdma_active;
static uint32_t santoni_sdma_start;
static bool santoni_pstate_buf_logged, santoni_pstate_tc_logged;
static int santoni_sdhci_lock;

static struct {
	uint64_t now, next_word, word_gap, busy_until, ready_until, flush_busy_ns;
	uint64_t write_busy_ns;
	uint32_t status, response, forced_busy, command6_r1, command13_r1;
	uint32_t last_argument, failure_status;
	unsigned words, total, limit, short_limit, fail_after, stale_words;
	unsigned fifo_reads, fifo_writes, stale_reads, underflows, overruns;
	unsigned commands, command12, command13, flushes, command_pairs, block_pairs;
	unsigned resets, vendor_resets, clock_cycles, core_reads, version_address;
	unsigned status_after_end;
	bool active, reading, complete, no_irq, no_tc, early_tc, late_crc;
	bool stale_edge, stale_stuck, reset_stuck, reset_keeps_fifo;
	bool vendor_works, clock_works, stuck_after_end, flush_stuck, locked;
} m;

static uint64_t
mach_absolute_time(void)
{
	m.now += 1000;
	return m.now;
}

static void
nanoseconds_to_absolutetime(uint64_t ns, uint64_t *absolute)
{
	*absolute = ns;
}

static void
nanoseconds_to_deadline(uint64_t ns, uint64_t *deadline)
{
	*deadline = m.now + ns;
}

static void
absolutetime_to_nanoseconds(uint64_t absolute, uint64_t *ns)
{
	*ns = absolute;
}

static void
delay(unsigned us)
{
	m.now += (uint64_t)us * 1000;
}

static void
lck_mtx_lock(int *lock)
{
	assert(lock == &santoni_sdhci_lock && !m.locked);
	m.locked = true;
}

static void
lck_mtx_unlock(int *lock)
{
	assert(lock == &santoni_sdhci_lock && m.locked);
	m.locked = false;
}

static uint32_t
load32(unsigned address)
{
	uint32_t value;

	assert(address + sizeof(value) <= sizeof(registers));
	memcpy(&value, registers + address, sizeof(value));
	return value;
}

static void
store32(unsigned address, uint32_t value)
{
	assert(address + sizeof(value) <= sizeof(registers));
	memcpy(registers + address, &value, sizeof(value));
}

static bool
fifo_ready(void)
{
	return m.active && m.words < m.limit && m.now >= m.next_word;
}

static void
finish_word(void)
{
	m.words++;
	m.next_word = m.now + m.word_gap;
	if (m.words == m.total || (m.early_tc && m.words == m.limit)) {
		m.complete = true;
		m.active = m.stuck_after_end;
		if (!m.reading && m.write_busy_ns) {
			m.busy_until = m.now + m.write_busy_ns;
			m.ready_until = m.busy_until + 3000000u;
		}
	}
}

static uint32_t
hc_rd(uint32_t offset)
{
	unsigned address = SDHCI_HC_OFFSET + offset;

	if (offset == SDHCI_INT_STATUS) {
		if (m.failure_status && m.words >= m.fail_after && m.active) {
			m.status |= m.failure_status;
		}
		if (m.complete && m.late_crc && ++m.status_after_end >= 2) {
			m.status |= SDHCI_INT_DATA_CRC;
		}
		uint32_t status = m.status;
		if (fifo_ready() && !m.no_irq) {
			status |= m.reading ? SDHCI_INT_BRR : SDHCI_INT_BWR;
		}
		if (m.stale_edge && !fifo_ready()) {
			status |= m.reading ? SDHCI_INT_BRR : SDHCI_INT_BWR;
		}
		if (m.complete && !m.no_tc) {
			status |= SDHCI_INT_TC;
		}
		return status;
	}
	if (offset == SDHCI_PRESENT_STATE) {
		uint32_t ps = m.forced_busy;
		if (!m.flush_stuck && m.now >= m.busy_until) {
			ps |= SDHCI_DATA_0_LVL_MASK;
		}
		if (m.active) {
			ps |= SDHCI_DATA_INHIBIT |
			    (m.reading ? SDHCI_PSTATE_RD_ACTIVE : SDHCI_PSTATE_WR_ACTIVE);
		}
		if (fifo_ready()) {
			ps |= m.reading ? SDHCI_PSTATE_BUF_RD_READY : SDHCI_PSTATE_BUF_WR_READY;
		}
		if (m.stale_words) {
			ps |= SDHCI_PSTATE_BUF_RD_READY;
		}
		return ps;
	}
	if (offset == SDHCI_BUFFER) {
		if (m.stale_words) {
			if (!m.stale_stuck) {
				m.stale_words--;
			}
			m.stale_reads++;
			return 0xdeadbeefu;
		}
		if (!fifo_ready() || !m.reading) {
			m.underflows++;
			return 0;
		}
		assert(m.words < sizeof(card_data) / sizeof(card_data[0]));
		uint32_t value = card_data[m.words];
		m.fifo_reads++;
		finish_word();
		return value;
	}
	if (offset == SDHCI_RESPONSE) {
		return m.response;
	}
	return load32(address);
}

static void
execute_command(uint16_t command, uint16_t mode)
{
	unsigned index = command >> 8;

	m.commands++;
	m.last_argument = load32(SDHCI_HC_OFFSET + SDHCI_ARGUMENT);
	m.response = 0x900u; /* TRAN and READY_FOR_DATA */
	m.status = SDHCI_INT_CC;
	m.complete = false;
	m.status_after_end = 0;
	if (index == 8 || index == 17 || index == 18 || index == 24 || index == 25) {
		uint32_t block = load32(SDHCI_HC_OFFSET + SDHCI_BLOCK_SIZE);
		assert((block & 0xfffu) == 512u);
		assert(mode & SDHCI_TRNS_BLK_CNT_EN);
		m.total = (block >> 16) * 128u;
		assert(m.total > 0 && m.total <= 256u);
		m.limit = m.short_limit ? m.short_limit : m.total;
		if (m.stale_edge) {
			m.limit = 0;
		}
		m.words = 0;
		m.next_word = m.now;
		m.active = true;
		m.reading = index == 8 || index == 17 || index == 18;
		assert(!!(mode & SDHCI_TRNS_READ) == m.reading);
		if (index == 18 || index == 25) {
			assert((mode & (SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12)) ==
			    (SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12));
		}
	} else if (index == 12) {
		m.command12++;
		m.active = false;
		m.forced_busy = 0;
	} else if (index == 6) {
		/* JEDEC write-byte FLUSH_CACHE[32]=1, never CACHE_CTRL[33]. */
		assert(m.last_argument == 0x03200100u);
		assert(m.locked);
		m.flushes++;
		m.busy_until = m.now + m.flush_busy_ns;
		m.ready_until = m.busy_until + 3000000u;
		m.response |= m.command6_r1;
	} else if (index == 13) {
		assert(m.now >= m.busy_until && !m.flush_stuck);
		assert(m.locked);
		m.command13++;
		m.response = m.now < m.ready_until ? 0xe00u : 0x900u;
		m.response |= m.command13_r1;
	} else {
		assert(index == 0);
	}
}

static void
hc_wr(uint32_t offset, uint32_t value)
{
	unsigned address = SDHCI_HC_OFFSET + offset;

	if (offset == SDHCI_INT_STATUS) {
		m.status &= ~value; /* W1C */
		return;
	}
	if (offset == SDHCI_BUFFER) {
		if (!fifo_ready() || m.reading) {
			m.overruns++;
			return;
		}
		written[m.words] = value;
		m.fifo_writes++;
		finish_word();
		return;
	}
	store32(address, value);
	if (offset == SDHCI_BLOCK_SIZE) {
		m.block_pairs++;
	}
	if (offset == SDHCI_TRANSFER_MODE) {
		m.command_pairs++;
		execute_command((uint16_t)(value >> 16), (uint16_t)value);
	}
	if (offset == CORE_VENDOR_SPEC_FUNC2 && (value & CORE_HC_SW_RST_REQ)) {
		m.vendor_resets++;
		if (m.vendor_works) {
			m.forced_busy = 0;
			m.active = false;
			registers[SDHCI_HC_OFFSET + SDHCI_SOFTWARE_RESET] = 0;
			store32(address, value & ~CORE_HC_SW_RST_REQ);
		}
	}
}

static uint16_t
hc_rd16(uint32_t offset)
{
	uint16_t value;
	unsigned address = SDHCI_HC_OFFSET + offset;

	assert(address + sizeof(value) <= sizeof(registers));
	if (offset == SDHCI_HOST_VERSION) {
		m.version_address = address;
	}
	memcpy(&value, registers + address, sizeof(value));
	return value;
}

static void
hc_wr16(uint32_t offset, uint16_t value)
{
	unsigned address = SDHCI_HC_OFFSET + offset;

	assert(address + sizeof(value) <= sizeof(registers));
	if (offset == SDHCI_CLOCK_CONTROL) {
		if (!value) {
			m.clock_cycles++;
		}
		if (value & SDHCI_CLOCK_INT_EN) {
			value |= SDHCI_CLOCK_INT_STABLE;
		}
		if ((value & SDHCI_CLOCK_CARD_EN) && m.clock_works) {
			m.forced_busy = 0;
			m.active = false;
		}
	}
	/* A v5 command does not execute on a lone halfword write. */
	memcpy(registers + address, &value, sizeof(value));
}

static uint8_t
hc_rd8(uint32_t offset)
{
	assert(SDHCI_HC_OFFSET + offset < sizeof(registers));
	return registers[SDHCI_HC_OFFSET + offset];
}

static void
hc_wr8(uint32_t offset, uint8_t value)
{
	unsigned address = SDHCI_HC_OFFSET + offset;

	assert(address < sizeof(registers));
	registers[address] = value;
	if (offset == SDHCI_SOFTWARE_RESET) {
		m.resets++;
		if (!m.reset_stuck) {
			if (m.reset_keeps_fifo && m.reading && m.words < m.limit) {
				m.stale_words = m.limit - m.words;
			}
			m.active = false;
			m.complete = false;
			m.forced_busy = 0;
			m.status = 0;
			registers[address] = 0;
		}
	}
}

static uint32_t __attribute__((unused))
core_rd(uint32_t offset)
{
	m.core_reads++;
	return load32(offset);
}

#include "sdhci_mock_impl.h"

static void
reset_mock(void)
{
	memset(&m, 0, sizeof(m));
	memset(&se, 0, sizeof(se));
	memset(registers, 0, sizeof(registers));
	memset(bounce, 0, sizeof(bounce));
	memset(written, 0, sizeof(written));
	for (unsigned i = 0; i < 256; i++) {
		card_data[i] = 0x5a000000u + i;
	}
	se.dma = (uintptr_t)bounce;
	se.rca = 2;
	se.sec_count = 1000;
	se.first_block = 100;
	se.nblocks = 8;
	se.ready = true;
	strcpy(se.part, "userdata");
	m.vendor_works = true;
	m.clock_works = true;
	santoni_data_moved = santoni_sdma_active = false;
	santoni_pstate_buf_logged = santoni_pstate_tc_logged = false;
	uint16_t version = 0x8002u;
	memcpy(registers + SDHCI_HC_OFFSET + SDHCI_HOST_VERSION, &version, 2);
	store32(CORE_MCI_VERSION, 0x10000044u);
	store32(CORE_GENERICS, 1u);
}

static void
test_layout(void)
{
	reset_mock();
	assert(sdhci_core_check() == 0);
#if defined(ARM64_BOARD_CONFIG_LAVENDER)
	assert(SDHCI_HC_OFFSET == 0 && m.version_address == 0xfeu);
	assert(CORE_VENDOR_SPEC == 0x20cu && CORE_DLL_CONFIG == 0x200u);
	assert(CORE_PWRCTL_STATUS == 0x240u && m.core_reads == 0);
	hc_wr16(SDHCI_HOST_VERSION, 0);
	assert(sdhci_core_check() == ENXIO);
	hc_wr16(SDHCI_HOST_VERSION, 0xffffu);
	assert(sdhci_core_check() == ENXIO);
#else
	assert(SDHCI_HC_OFFSET == 0x900u && m.core_reads == 2);
	store32(CORE_MCI_VERSION, 0);
	assert(sdhci_core_check() == ENXIO);
#endif
	hc_wr(CORE_VENDOR_SPEC3, 0x02626440u);
	sdhci_v5_standard_fifo();
	assert(hc_rd(CORE_VENDOR_SPEC3) == 0x02626040u);
}

static void
test_exact_reads(void)
{
	reset_mock();
	m.no_irq = true;
	m.word_gap = 10000;
	assert(sdhci_read_pio_once(17, 7, 2, bounce) == 0);
	assert(m.fifo_reads == 256 && m.underflows == 0);
	assert(!memcmp(bounce, card_data, sizeof(bounce)));
	assert(m.block_pairs == 1 && m.command_pairs == 1);
	reset_mock();
	m.no_irq = m.no_tc = true;
	assert(sdhci_read_pio_once(18, 7, 1, bounce) == 0);
	assert(m.fifo_reads == 128 && m.underflows == 0);
	assert(!memcmp(bounce, card_data, 512));
	reset_mock();
	memset(card_data, 0, sizeof(card_data));
	assert(sdhci_read_pio_once(17, 7, 1, bounce) == 0); /* zero sectors are legal */
	assert(sdhci_read_pio_once(8, 0, 1, bounce) == EIO); /* zero EXT_CSD is not */
	assert(sdhci_read_pio_once(17, 7, 1, NULL) == EINVAL);
	assert(sdhci_read_pio_once(17, 7, 0, bounce) == EINVAL);
}

static void
test_partial_and_retry(void)
{
	reset_mock();
	m.short_limit = 16;
	m.early_tc = true;
	assert(sdhci_read_pio_once(17, 7, 1, bounce) == EIO);
	assert(m.fifo_reads == 16 && m.underflows == 0 && !santoni_data_moved);
	reset_mock();
	m.failure_status = SDHCI_INT_DATA_CRC; /* no summary ERR bit */
	m.fail_after = 63;
	m.reset_keeps_fifo = true;
	assert(sdhci_read_pio_once(17, 7, 1, bounce) == EIO);
	assert(m.fifo_reads == 63 && m.stale_reads == 65);
	assert(m.underflows == 0 && m.stale_words == 0);
	m.failure_status = 0;
	m.reset_keeps_fifo = false;
	assert(sdhci_read_pio_once(17, 7, 1, bounce) == 0);
	assert(!memcmp(bounce, card_data, 512));
	reset_mock();
	m.no_tc = m.stuck_after_end = m.late_crc = true;
	assert(sdhci_read_pio_once(17, 7, 1, bounce) == EIO);
	assert(m.fifo_reads == 128); /* a late CRC must not become success */
}

static void
test_stale_and_deadline(void)
{
	reset_mock();
	m.stale_words = 7;
	assert(sdhci_read_pio_once(17, 7, 1, bounce) == 0);
	assert(m.stale_reads == 7 && !memcmp(bounce, card_data, 512));
	reset_mock();
	m.stale_edge = true; /* latched BRR, permanently empty FIFO */
	assert(sdhci_read_pio_once(17, 7, 1, bounce) == ETIMEDOUT);
	assert(m.fifo_reads == 0 && m.underflows == 0);
	assert(m.now < 2200000000ULL);
	reset_mock();
	m.stale_words = 1;
	m.stale_stuck = true;
	assert(sdhci_read_pio_once(17, 7, 1, bounce) == ETIMEDOUT);
	assert(m.commands == 0 && m.now <= 51000000u);
	reset_mock();
	m.word_gap = 20000000u;
	assert(sdhci_read_pio_once(17, 7, 1, bounce) == ETIMEDOUT);
	assert(m.fifo_reads < 128 && m.now < 2200000000ULL);
}

static void
test_recovery(void)
{
	reset_mock();
	m.forced_busy = SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT;
	m.reset_stuck = true;
	assert(sdhci_read_pio_once(17, 7, 1, bounce) == 0);
	assert(m.vendor_resets > 0); /* reset timeout must still reach vendor recovery */
	reset_mock();
	m.forced_busy = SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT;
	m.reset_stuck = true;
	m.vendor_works = m.clock_works = false;
	assert(sdhci_read_pio_once(17, 7, 1, bounce) == ETIMEDOUT);
	assert(m.commands == 0 && m.now < 200000000u);
	reset_mock();
	m.forced_busy = SDHCI_DATA_INHIBIT;
	assert(sdhci_stop_transmission() == 0 && m.command12 == 1);
	reset_mock();
	m.no_tc = m.stuck_after_end = true;
	assert(sdhci_read_pio_once(18, 7, 2, bounce) == ETIMEDOUT);
	assert(m.command12 == 1 && m.underflows == 0);
}

static void
test_writes_and_bounds(void)
{
	reset_mock();
	m.no_irq = true;
	m.word_gap = 10000;
	assert(sdhci_write_pio(100, 2, card_data) == 0);
	assert(m.fifo_writes == 256 && m.overruns == 0);
	assert(!memcmp(written, card_data, sizeof(written)));
	reset_mock();
	m.failure_status = SDHCI_INT_DATA_CRC;
	m.fail_after = 31;
	assert(sdhci_write_pio(100, 1, card_data) == EIO);
	assert(m.fifo_writes == 31 && m.overruns == 0);
	reset_mock();
	assert(santoni_sdhci_write_blocks(100, 1, card_data) == EROFS);
	assert(m.commands == 0 && m.fifo_writes == 0);
	se.writable = true;
	assert(santoni_sdhci_write_blocks(99, 1, card_data) == EPERM);
	assert(santoni_sdhci_write_blocks(107, 2, card_data) == EPERM);
	assert(m.commands == 0);
	assert(santoni_sdhci_write_blocks(107, 1, card_data) == 0);
	assert(m.fifo_writes == 128);
}

static void
set_ext_csd(uint8_t cache)
{
	uint8_t *ext = (void *)bounce;
	uint32_t sectors = 1000;

	memset(bounce, 0, sizeof(bounce));
	ext[192] = 8;
	ext[193] = 0xff; /* reserved byte, not CSD_STRUCTURE */
	ext[194] = 2;
	ext[33] = cache;
	memcpy(ext + 212, &sectors, sizeof(sectors));
}

static void
test_cache(void)
{
	reset_mock();
	assert(sdhci_synchronize_cache() == EIO && !m.locked);
	set_ext_csd(0);
	assert(sdhci_record_cache_state() == 0 && se.cache_valid);
	assert(sdhci_synchronize_cache() == 0 && m.flushes == 0);
	m.write_busy_ns = 200000000u;
	assert(sdhci_write_pio(100, 1, card_data) == 0 && se.write_pending);
	assert(m.now < m.busy_until);
	assert(sdhci_synchronize_cache() == 0);
	assert(m.now >= m.ready_until && m.command13 > 0 && m.flushes == 0);
	assert(!se.write_pending && !m.locked);
	reset_mock();
	set_ext_csd(2);
	assert(sdhci_record_cache_state() == EIO && !se.cache_valid);
	set_ext_csd(1);
	((uint8_t *)bounce)[194] = 4;
	assert(sdhci_record_cache_state() == EIO);
	set_ext_csd(1);
	assert(sdhci_record_cache_state() == 0);
	m.flush_busy_ns = 500000000u;
	assert(sdhci_synchronize_cache() == 0);
	assert(m.flushes == 1 && m.command13 > 0 && !m.locked);
	assert(m.now >= m.ready_until && m.fifo_writes == 0);
	assert(se.cache_ctrl == 1); /* flush never toggles inherited CACHE_CTRL */
	assert(sdhci_cmd(0, 0, 0, 0, NULL) == 0 && !se.cache_valid);
	reset_mock();
	set_ext_csd(1);
	assert(sdhci_record_cache_state() == 0);
	m.command6_r1 = SDHCI_R1_SWITCH_ERROR;
	assert(sdhci_synchronize_cache() == EIO);
	assert(m.command13 == 0 && !m.locked);
	reset_mock();
	set_ext_csd(1);
	assert(sdhci_record_cache_state() == 0);
	m.command13_r1 = SDHCI_R1_ERROR;
	assert(sdhci_synchronize_cache() == EIO && !m.locked);
	reset_mock();
	set_ext_csd(1);
	assert(sdhci_record_cache_state() == 0);
	m.flush_stuck = true;
	assert(sdhci_synchronize_cache() == ETIMEDOUT);
	assert(m.command13 == 0 && !m.locked && m.now < 31000000000ULL);
	reset_mock();
	set_ext_csd(0);
	assert(sdhci_record_cache_state() == 0);
	assert(sdhci_write_pio(100, 1, card_data) == 0 && se.write_pending);
	m.flush_stuck = true;
	assert(sdhci_synchronize_cache() == ETIMEDOUT);
	assert(se.write_pending && m.flushes == 0 && !m.locked);
	reset_mock();
	se.ready = false;
	assert(sdhci_synchronize_cache() == ENXIO && m.commands == 0 && !m.locked);
}

static void
test_probe_pio(void)
{
	uint32_t tm, ps, r1;

	reset_mock();
	assert(sdhci_probe_attempt(SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN,
	    &tm, &ps, &r1) == 0);
	assert(m.fifo_reads == 128 && m.underflows == 0);
	reset_mock();
	m.failure_status = SDHCI_INT_DATA_CRC;
	m.fail_after = 47;
	assert(sdhci_probe_attempt(SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN,
	    &tm, &ps, &r1) == EIO);
	assert(m.fifo_reads == 47 && m.underflows == 0);
}

int
main(void)
{
	test_layout();
	test_exact_reads();
	test_partial_and_retry();
	test_stale_and_deadline();
	test_recovery();
	test_writes_and_bounds();
	test_cache();
	test_probe_pio();
#if defined(ARM64_BOARD_CONFIG_LAVENDER)
	puts("test_sdhci_current: SDCC v5 layout/PIO/recovery/cache groups passed");
#else
	puts("test_sdhci_current: SDCC v4 layout/PIO/recovery/cache groups passed");
#endif
	return 0;
}
