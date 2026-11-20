#pragma once
#include <stdexcept>
#include "util/types.hpp"
#include "Utilities/StrFmt.h"
#include "util/endian.hpp"
#include "util/to_endian.hpp"
#include "util/asm.hpp"
#include "util/v128.hpp"
#include "util/simd.hpp"
#include "Utilities/BitField.h"
#include "Emu/Cell/SPUOpcodes.h"
#include "Emu/Cell/Common.h"
#if defined(ARCH_ARM64)
#include "Emu/CPU/sse2neon.h"
#endif
#include <array>
#include <cmath>
#include <cfenv>
#include <cstdio>
#include <stdexcept>
#include <utility>

struct spu_log_t { template <typename... A> void fatal(const char*, A&&...) { throw std::runtime_error("unk"); } } static spu_log;

#include "rpcs3_decls.h"

namespace cpu_flag_stub { }
namespace cpu_flag { constexpr u32 pending = 1; }
struct spu_events_stub { u32 count; };

class spu_thread
{
public:
	v128 gpr[128];
	SPU_FPSCR fpscr;
	u32 pc = 0, srr0 = 0;
	u8* ls = nullptr;
	u32 state = 0;
	bool allow_interrupts_in_cpu_work = false;
	int halted = 0, stopped = 0; u32 stop_code = 0;
	int ch_ops = 0; u32 ch_no = 0; u32 ch_in_val = 0, ch_cnt_val = 0, ch_out_val = 0; u32 events = 0;
	mutable u32 last_ref = ~0u;
	template <typename T> to_be_t<T>& _ref(u32 lsa) const { last_ref = lsa; return *reinterpret_cast<to_be_t<T>*>(ls + lsa); }
	void halt() { halted = 1; }
	bool stop_and_signal(u32 code) { stopped = 1; stop_code = code; return true; }
	s64 get_ch_value(u32 ch) { ch_ops++; ch_no = ch; return ch_in_val; }
	u32 get_ch_count(u32 ch) { ch_ops++; ch_no = ch; return ch_cnt_val; }
	bool set_ch_value(u32 ch, u32 v) { ch_ops++; ch_no = ch; ch_out_val = v; return true; }
	spu_events_stub get_events() { return {events}; }
	bool check_mfc_interrupts(u32) { return false; }
	void do_mfc() {}
	void set_interrupt_status(bool) {}
};

struct spu_interpreter { static void set_interrupt_status(spu_thread&, spu_opcode_t); };
struct spu_interpreter_precise
{
#define PF(n) static bool n(spu_thread&, spu_opcode_t);
	PF(FREST) PF(FRSQEST) PF(FCGT) PF(FA) PF(FS) PF(FM) PF(FCMGT) PF(DFA) PF(DFS) PF(DFM) PF(DFMA) PF(DFMS) PF(DFNMS) PF(DFNMA)
	PF(FSCRRD) PF(FESD) PF(FRDS) PF(FSCRWR) PF(FCEQ) PF(FCMEQ) PF(FI) PF(CFLTS) PF(CFLTU) PF(CSFLT) PF(CUFLT) PF(FNMS) PF(FMA) PF(FMS)
#undef PF
};

#ifndef _MM_EXCEPT_INVALID
#define _MM_EXCEPT_INVALID 0x1u
#define _MM_EXCEPT_OVERFLOW 0x8u
#define _MM_EXCEPT_UNDERFLOW 0x10u
#define _MM_EXCEPT_INEXACT 0x20u
#endif

#undef _mm_srai_epi32
static inline __m128i _mm_srai_epi32(__m128i a, int imm) { return (imm & ~31) ? (__m128i)vshrq_n_s32((int32x4_t)a, 31) : (__m128i)vshlq_s32((int32x4_t)a, vdupq_n_s32(-(imm & 31))); }

static inline unsigned oracle_getcsr() {
	unsigned r = 0;
	if (std::fetestexcept(FE_INVALID)) r |= _MM_EXCEPT_INVALID;
	if (std::fetestexcept(FE_OVERFLOW)) r |= _MM_EXCEPT_OVERFLOW;
	if (std::fetestexcept(FE_UNDERFLOW)) r |= _MM_EXCEPT_UNDERFLOW;
	if (std::fetestexcept(FE_INEXACT)) r |= _MM_EXCEPT_INEXACT;
	return r;
}
