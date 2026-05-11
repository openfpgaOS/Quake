/*
 * sysreg_stub.h -- stand-ins for PocketQuake sysreg MMIO.
 *
 * The engine's pq_cycleprof / HW performance-counter code reads
 * SYS_CYCLE_LO and a bunch of SYS_PERF_* and SPAN_PERF_* registers to
 * build the in-game profiler overlay. Those MMIOs don't exist on
 * openfpgaOS.  SYS_CYCLE_LO is synthesised from of_time_us() so the
 * per-frame timings populate.  The renderer profiler reads GPU-native
 * command/ring/DMA counters via of_gpu_debug_snapshot(); these legacy
 * SYS_PERF_* / SPAN_PERF_* definitions remain only for old code paths.
 */

#ifndef SYSREG_STUB_H
#define SYSREG_STUB_H

#include <stdint.h>
#include "of_timer.h"  /* of_time_us — see SYS_CYCLE_LO below */

/* PocketQuake scanline accelerator is not present on this port. */
#ifndef HW_SCANLINE_ACCEL
#define HW_SCANLINE_ACCEL 0
#endif

extern uint32_t _of_sysreg_ro_zero;

#define SYS_STATUS              _of_sysreg_ro_zero

/* rdcycle / mcycle trap illegal-instruction on this VexRiscv build (see
 * of_gpu.h:397-400) and openfpgaOS doesn't expose a SYS_CYCLE_* MMIO,
 * so synthesise a cycle counter from of_time_us().  Multiply by 105 to
 * keep the engine's "cycles / 105000 = ms" arithmetic correct on a
 * ~105 MHz CPU.  Each read becomes one ecall — fine for the per-frame
 * outer probes; sub-microsecond inner-loop probes quantise to 0. */
#define SYS_CYCLE_LO            (of_time_us() * 105u)
#define SYS_CYCLE_HI            _of_sysreg_ro_zero
#define SYS_PERF_SPAN_BUSY      _of_sysreg_ro_zero
#define SYS_PERF_SPAN_FIFO_FULL _of_sysreg_ro_zero
#define SYS_PERF_ICACHE_MISS    _of_sysreg_ro_zero
#define SYS_PERF_DCACHE_MISS    _of_sysreg_ro_zero
#define SYS_PERF_ICACHE_STALL   _of_sysreg_ro_zero
#define SYS_PERF_DCACHE_STALL   _of_sysreg_ro_zero
#define SPAN_PERF_CACHE_HITS    _of_sysreg_ro_zero
#define SPAN_PERF_CACHE_MISSES  _of_sysreg_ro_zero
#define SPAN_PERF_PIXELS        _of_sysreg_ro_zero

#endif
