/*
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *  Copyright (C) 2000, 2001, 2002 Andi Kleen, SuSE Labs
 *
 *  Pentium III FXSR, SSE support
 *	Gareth Hughes <gareth@valinux.com>, May 2000
 */

/*
 * Handle hardware traps and faults.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/context_tracking.h>
#include <linux/interrupt.h>
#include <linux/kallsyms.h>
#include <linux/spinlock.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/kdebug.h>
#include <linux/kgdb.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ptrace.h>
#include <linux/uprobes.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kexec.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/init.h>
#include <linux/bug.h>
#include <linux/nmi.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/io.h>

#ifdef CONFIG_EISA
#include <linux/ioport.h>
#include <linux/eisa.h>
#endif

#if defined(CONFIG_EDAC)
#include <linux/edac.h>
#endif

#include <asm/kmemcheck.h>
#include <asm/stacktrace.h>
#include <asm/processor.h>
#include <asm/debugreg.h>
#include <linux/atomic.h>
#include <asm/ftrace.h>
#include <asm/traps.h>
#include <asm/desc.h>
#include <asm/fpu/internal.h>
#include <asm/mce.h>
#include <asm/fixmap.h>
#include <asm/mach_traps.h>
#include <asm/alternative.h>
#include <asm/fpu/xstate.h>
#include <asm/trace/mpx.h>
#include <asm/mpx.h>
#include <asm/vm86.h>

#ifdef CONFIG_X86_64
#include <asm/x86_init.h>
#include <asm/pgalloc.h>
#include <asm/proto.h>

/* No need to be aligned, but done to keep all IDTs defined the same way. */
gate_desc debug_idt_table[NR_VECTORS] __page_aligned_bss;
#else
#include <asm/processor-flags.h>
#include <asm/setup.h>
#include <asm/proto.h>
#endif

#include "SSEPlus_REF.h"

/* Must be page-aligned because the real IDT is used in a fixmap. */
gate_desc idt_table[NR_VECTORS] __page_aligned_bss;

DECLARE_BITMAP(used_vectors, NR_VECTORS);
EXPORT_SYMBOL_GPL(used_vectors);

static inline void conditional_sti(struct pt_regs *regs)
{
	if (regs->flags & X86_EFLAGS_IF)
		local_irq_enable();
}

static inline void preempt_conditional_sti(struct pt_regs *regs)
{
	preempt_count_inc();
	if (regs->flags & X86_EFLAGS_IF)
		local_irq_enable();
}

static inline void conditional_cli(struct pt_regs *regs)
{
	if (regs->flags & X86_EFLAGS_IF)
		local_irq_disable();
}

static inline void preempt_conditional_cli(struct pt_regs *regs)
{
	if (regs->flags & X86_EFLAGS_IF)
		local_irq_disable();
	preempt_count_dec();
}

void ist_enter(struct pt_regs *regs)
{
	if (user_mode(regs)) {
		RCU_LOCKDEP_WARN(!rcu_is_watching(), "entry code didn't wake RCU");
	} else {
		/*
		 * We might have interrupted pretty much anything.  In
		 * fact, if we're a machine check, we can even interrupt
		 * NMI processing.  We don't want in_nmi() to return true,
		 * but we need to notify RCU.
		 */
		rcu_nmi_enter();
	}

	/*
	 * We are atomic because we're on the IST stack; or we're on
	 * x86_32, in which case we still shouldn't schedule; or we're
	 * on x86_64 and entered from user mode, in which case we're
	 * still atomic unless ist_begin_non_atomic is called.
	 */
	preempt_count_add(HARDIRQ_OFFSET);

	/* This code is a bit fragile.  Test it. */
	RCU_LOCKDEP_WARN(!rcu_is_watching(), "ist_enter didn't work");
}

void ist_exit(struct pt_regs *regs)
{
	preempt_count_sub(HARDIRQ_OFFSET);

	if (!user_mode(regs))
		rcu_nmi_exit();
}

/**
 * ist_begin_non_atomic() - begin a non-atomic section in an IST exception
 * @regs:	regs passed to the IST exception handler
 *
 * IST exception handlers normally cannot schedule.  As a special
 * exception, if the exception interrupted userspace code (i.e.
 * user_mode(regs) would return true) and the exception was not
 * a double fault, it can be safe to schedule.  ist_begin_non_atomic()
 * begins a non-atomic section within an ist_enter()/ist_exit() region.
 * Callers are responsible for enabling interrupts themselves inside
 * the non-atomic section, and callers must call ist_end_non_atomic()
 * before ist_exit().
 */
void ist_begin_non_atomic(struct pt_regs *regs)
{
	BUG_ON(!user_mode(regs));

	/*
	 * Sanity check: we need to be on the normal thread stack.  This
	 * will catch asm bugs and any attempt to use ist_preempt_enable
	 * from double_fault.
	 */
	BUG_ON((unsigned long)(current_top_of_stack() -
			       current_stack_pointer()) >= THREAD_SIZE);

	preempt_count_sub(HARDIRQ_OFFSET);
}

/**
 * ist_end_non_atomic() - begin a non-atomic section in an IST exception
 *
 * Ends a non-atomic section started with ist_begin_non_atomic().
 */
void ist_end_non_atomic(void)
{
	preempt_count_add(HARDIRQ_OFFSET);
}

static nokprobe_inline int
do_trap_no_signal(struct task_struct *tsk, int trapnr, char *str,
		  struct pt_regs *regs,	long error_code)
{
	if (v8086_mode(regs)) {
		/*
		 * Traps 0, 1, 3, 4, and 5 should be forwarded to vm86.
		 * On nmi (interrupt 2), do_trap should not be called.
		 */
		if (trapnr < X86_TRAP_UD) {
			if (!handle_vm86_trap((struct kernel_vm86_regs *) regs,
						error_code, trapnr))
				return 0;
		}
		return -1;
	}

	if (!user_mode(regs)) {
		if (!fixup_exception(regs)) {
			tsk->thread.error_code = error_code;
			tsk->thread.trap_nr = trapnr;
			die(str, regs, error_code);
		}
		return 0;
	}

	return -1;
}

static siginfo_t *fill_trap_info(struct pt_regs *regs, int signr, int trapnr,
				siginfo_t *info)
{
	unsigned long siaddr;
	int sicode;

	switch (trapnr) {
	default:
		return SEND_SIG_PRIV;

	case X86_TRAP_DE:
		sicode = FPE_INTDIV;
		siaddr = uprobe_get_trap_addr(regs);
		break;
	case X86_TRAP_UD:
		sicode = ILL_ILLOPN;
		siaddr = uprobe_get_trap_addr(regs);
		break;
	case X86_TRAP_AC:
		sicode = BUS_ADRALN;
		siaddr = 0;
		break;
	}

	info->si_signo = signr;
	info->si_errno = 0;
	info->si_code = sicode;
	info->si_addr = (void __user *)siaddr;
	return info;
}

static void
do_trap(int trapnr, int signr, char *str, struct pt_regs *regs,
	long error_code, siginfo_t *info)
{
	struct task_struct *tsk = current;


	if (!do_trap_no_signal(tsk, trapnr, str, regs, error_code))
		return;
	/*
	 * We want error_code and trap_nr set for userspace faults and
	 * kernelspace faults which result in die(), but not
	 * kernelspace faults which are fixed up.  die() gives the
	 * process no chance to handle the signal and notice the
	 * kernel fault information, so that won't result in polluting
	 * the information about previously queued, but not yet
	 * delivered, faults.  See also do_general_protection below.
	 */
	tsk->thread.error_code = error_code;
	tsk->thread.trap_nr = trapnr;

#ifdef CONFIG_X86_64
	if (show_unhandled_signals && unhandled_signal(tsk, signr) &&
	    printk_ratelimit()) {
		pr_info("%s[%d] trap %s ip:%lx sp:%lx error:%lx",
			tsk->comm, tsk->pid, str,
			regs->ip, regs->sp, error_code);
		print_vma_addr(" in ", regs->ip);
		pr_cont("\n");
	}
#endif

	force_sig_info(signr, info ?: SEND_SIG_PRIV, tsk);
}
NOKPROBE_SYMBOL(do_trap);

static void do_error_trap(struct pt_regs *regs, long error_code, char *str,
			  unsigned long trapnr, int signr)
{
	siginfo_t info;

	RCU_LOCKDEP_WARN(!rcu_is_watching(), "entry code didn't wake RCU");

	if (notify_die(DIE_TRAP, str, regs, error_code, trapnr, signr) !=
			NOTIFY_STOP) {
		conditional_sti(regs);
		do_trap(trapnr, signr, str, regs, error_code,
			fill_trap_info(regs, signr, trapnr, &info));
	}
}

#define DO_ERROR(trapnr, signr, str, name)				\
dotraplinkage void do_##name(struct pt_regs *regs, long error_code)	\
{									\
	do_error_trap(regs, error_code, str, trapnr, signr);		\
}

DO_ERROR(X86_TRAP_DE,     SIGFPE,  "divide error",		divide_error)
DO_ERROR(X86_TRAP_OF,     SIGSEGV, "overflow",			overflow)
DO_ERROR(X86_TRAP_OLD_MF, SIGFPE,  "coprocessor segment overrun",coprocessor_segment_overrun)
DO_ERROR(X86_TRAP_TS,     SIGSEGV, "invalid TSS",		invalid_TSS)
DO_ERROR(X86_TRAP_NP,     SIGBUS,  "segment not present",	segment_not_present)
DO_ERROR(X86_TRAP_SS,     SIGBUS,  "stack segment",		stack_segment)
DO_ERROR(X86_TRAP_AC,     SIGBUS,  "alignment check",		alignment_check)


#define OPCODE_SIZE 12
#define DEBUG_INST_EMULATION 0

#if DEBUG_INST_EMULATION
#define INSTR_NAME(x) __instr_name = x
#else
#define INSTR_NAME(x)
#endif

dotraplinkage void do_invalid_op(struct pt_regs *regs, long error_code)
{
	siginfo_t info;
	enum ctx_state prev_state;
	int handled = 0;
	union {
		unsigned char byte[OPCODE_SIZE];
	} opcode;
	int prefix66 = 0, prefixREX = 0;
#if DEBUG_INST_EMULATION
	const char* __instr_name = NULL;
#endif

	info.si_signo = SIGILL;
	info.si_errno = 0;
	info.si_code = ILL_ILLOPN;
	info.si_addr = (void __user *)regs->ip;

	prev_state = exception_enter();

	if (copy_from_user((void *)&opcode.byte[0],
		(const void __user *)regs->ip, OPCODE_SIZE)) {
		pr_info("No user code available.");
	}

	// 0xf3 prefix is used by popcnt
	if (opcode.byte[0] == 0x66 || opcode.byte[0] == 0xf3) {
		int i;
		prefix66 = opcode.byte[0] == 0x66;
		for (i = 1; i < OPCODE_SIZE; i++)
			opcode.byte[i-1] = opcode.byte[i];
		regs->ip++;
	}

	while ((opcode.byte[0] & 0xf0) == 0x40) {
		int i;
		prefixREX = opcode.byte[0];
		for (i = 1; i < OPCODE_SIZE; i++)
			opcode.byte[i-1] = opcode.byte[i];
		regs->ip++;
	}

	if (opcode.byte[0] == 0x0f) {
		if (opcode.byte[1] == 0x38) {
			ssp_m128 ret, src;
			unsigned int dstIndex = (opcode.byte[3]>>3) & 0x7;
			int op_len;

			if (opcode.byte[2] == 0x2a) {
				unsigned long memAddr = 0;
				int regIndex = (opcode.byte[3]>>3) & 0x7;
				int op_len = 4 + decodeMemAddress(opcode.byte[3], regs, prefixREX, &opcode.byte[4], &memAddr);
				u8 data[sizeof(ssp_m128)];

				INSTR_NAME("movntdqa");

				if (memAddr && !copy_from_user((void *)data, (const void __user *)memAddr, sizeof(ssp_m128))) {
					ssp_m128 ret = ssp_stream_load_si128((ssp_m128*)data);
					setXMMRegister(regIndex, testREX(prefixREX, REX_R), &ret);
					handled = 1;
					regs->ip += op_len;
				}
			}
			else if (opcode.byte[2] == 0xf0 || opcode.byte[2] == 0xf1) {
				unsigned long memAddr = 0;
				int regIndex = (opcode.byte[3]>>3) & 0x7;
				int op_bytes = testREX(prefixREX, REX_W) ? 8 : (prefix66 ? 2 : 4);
				int op_len = 4 + decodeMemAddress(opcode.byte[3], regs, prefixREX, &opcode.byte[4], &memAddr);
				u8 data[8];

				INSTR_NAME("movbe");

				if (memAddr && opcode.byte[2] == 0xf0) {
					// dst reg
					if (!copy_from_user((void *)data, (const void __user *)memAddr, op_bytes)) {
						unsigned long* regValue = getRegisterPtr(regIndex, regs, testREX(prefixREX, REX_R));
						switch (op_bytes) {
						case 2:
							*regValue &= ~0xffffUL;
							*regValue |= swab16(*(u16*)data);
							break;
						case 4:
							*regValue &= ~0xffffffffUL;
							*regValue |= swab32(*(u32*)data);
							break;
						case 8:
							*regValue = swab64(*(u64*)data);
							break;
						}
						handled = 1;
						regs->ip += op_len;
					}
					else {
						pr_info("movbe copy_from_user failed. op_bytes=%d, op_len=%d, memAddr=%p\n",
								op_bytes, op_len, (void*)memAddr);
					}
				}
				else if (memAddr) {
					// dst mem
					switch (op_bytes) {
					case 2:
						*(u16*)data = swab16(*(u16*)getRegisterPtr(regIndex, regs, testREX(prefixREX, REX_R)));
						break;
					case 4:
						*(u32*)data = swab32(*(u32*)getRegisterPtr(regIndex, regs, testREX(prefixREX, REX_R)));
						break;
					case 8:
						*(u64*)data = swab64(*(u64*)getRegisterPtr(regIndex, regs, testREX(prefixREX, REX_R)));
						break;
					}
					if (!copy_to_user((void __user *)memAddr, (void *)data, op_bytes)) {
						handled = 1;
						regs->ip += op_len;
					}
					else {
						pr_info("movbe copy_to_user failed. op_bytes=%d, op_len=%d, memAddr=%p\n",
								op_bytes, op_len, (void*)memAddr);
					}
				}
			}
			else if ((op_len = getOp2XMMValue(opcode.byte[3], regs, prefixREX, &opcode.byte[4], &src)) != -1) {
				op_len += 4;
				ret = getXMMRegister(dstIndex, testREX(prefixREX, REX_R));

				switch (opcode.byte[2]) {
				case 0x00:
					INSTR_NAME("pshufb");
					ret = ssp_shuffle_epi8(&ret, &src);
					handled = 1;
					break;
				case 0x01:
					INSTR_NAME("phaddw");
					ret = ssp_hadd_epi16(&ret, &src);
					handled = 1;
					break;
				case 0x02:
					INSTR_NAME("phaddd");
					ret = ssp_hadd_epi32(&ret, &src);
					handled = 1;
					break;
				case 0x03:
					INSTR_NAME("phaddsw");
					ret = ssp_hadds_epi16(&ret, &src);
					handled = 1;
					break;
				case 0x04:
					INSTR_NAME("pmaddubsw");
					ret = ssp_maddubs_epi16(&ret, &src);
					handled = 1;
					break;
				case 0x05:
					INSTR_NAME("phsubw");
					ret = ssp_hsub_epi16(&ret, &src);
					handled = 1;
					break;
				case 0x06:
					INSTR_NAME("phsubd");
					ret = ssp_hsub_epi32(&ret, &src);
					handled = 1;
					break;
				case 0x07:
					INSTR_NAME("phsubsw");
					ret = ssp_hsubs_epi16(&ret, &src);
					handled = 1;
					break;
				case 0x08:
					INSTR_NAME("psignb");
					ret = ssp_sign_epi8(&ret, &src);
					handled = 1;
					break;
				case 0x09:
					INSTR_NAME("psignw");
					ret = ssp_sign_epi16(&ret, &src);
					handled = 1;
					break;
				case 0x0a:
					INSTR_NAME("psignd");
					ret = ssp_sign_epi32(&ret, &src);
					handled = 1;
					break;
				case 0x0b:
					INSTR_NAME("pmulhrsw");
					ret = ssp_mulhrs_epi16(&ret, &src);
					handled = 1;
					break;
				case 0x10:
				{
					ssp_m128 op3 = getXMMRegister(0, 0);
					INSTR_NAME("pblendvb");
					ret = ssp_blendv_epi8(&ret, &src, &op3);
					handled = 1;
					break;
				}
				case 0x14:
				{
					ssp_m128 op3 = getXMMRegister(0, 0);
					INSTR_NAME("blendvps");
					ret = ssp_blendv_ps(&ret, &src, &op3);
					handled = 1;
					break;
				}
				case 0x15:
				{
					ssp_m128 op3 = getXMMRegister(0, 0);
					INSTR_NAME("blendvpd");
					ret = ssp_blendv_pd(&ret, &src, &op3);
					handled = 1;
					break;
				}
				case 0x17:
				{
					int cf = ssp_testc_si128(&ret, &src);
					int zf = ssp_testz_si128(&ret, &src);
					INSTR_NAME("ptest");
					if (zf) regs->flags |= 1<<6;
					if (cf) regs->flags |= 1;
					handled = 1;
					break;
				}
				case 0x1c:
					INSTR_NAME("pabsb");
					ret = src;
					ssp_abs_epi8(&ret);
					handled = 1;
					break;
				case 0x1d:
					INSTR_NAME("pabsw");
					ret = src;
					ssp_abs_epi16(&ret);
					handled = 1;
					break;
				case 0x1e:
					INSTR_NAME("pabsd");
					ret = src;
					ssp_abs_epi32(&ret);
					handled = 1;
					break;
				case 0x20:
					INSTR_NAME("pmovsxbw");
					ret = ssp_cvtepi8_epi16(&src);
					handled = 1;
					break;
				case 0x21:
					INSTR_NAME("pmovsxbd");
					ret = ssp_cvtepi8_epi32(&src);
					handled = 1;
					break;
				case 0x22:
					INSTR_NAME("pmovsxbq");
					ret = ssp_cvtepi8_epi64(&src);
					handled = 1;
					break;
				case 0x23:
					INSTR_NAME("pmovsxwd");
					ret = ssp_cvtepi16_epi32(&src);
					handled = 1;
					break;
				case 0x24:
					INSTR_NAME("pmovsxwq");
					ret = ssp_cvtepi16_epi64(&src);
					handled = 1;
					break;
				case 0x25:
					INSTR_NAME("pmovsxdq");
					ret = ssp_cvtepi32_epi64(&src);
					handled = 1;
					break;
				case 0x28:
					INSTR_NAME("pmuldq");
					ret = ssp_mul_epi32(&ret, &src);
					handled = 1;
					break;
				case 0x29:
					INSTR_NAME("pcmpeqq");
					ret = ssp_cmpeq_epi64(&ret, &src);
					handled = 1;
					break;
				case 0x2b:
					INSTR_NAME("packusdw");
					ret = ssp_packus_epi32(&ret, &src);
					handled = 1;
					break;
				case 0x30:
					INSTR_NAME("pmovzxbw");
					ret = ssp_cvtepu8_epi16(&src);
					handled = 1;
					break;
				case 0x31:
					INSTR_NAME("pmovzxbd");
					ret = ssp_cvtepu8_epi32(&src);
					handled = 1;
					break;
				case 0x32:
					INSTR_NAME("pmovzxbq");
					ret = ssp_cvtepu8_epi64(&src);
					handled = 1;
					break;
				case 0x33:
					INSTR_NAME("pmovzxwd");
					ret = ssp_cvtepu16_epi32(&src);
					handled = 1;
					break;
				case 0x34:
					INSTR_NAME("pmovzxwq");
					ret = ssp_cvtepu16_epi64(&src);
					handled = 1;
					break;
				case 0x35:
					INSTR_NAME("pmovzxdq");
					ret = ssp_cvtepu32_epi64(&src);
					handled = 1;
					break;
				case 0x38:
					INSTR_NAME("pminsb");
					ret = ssp_min_epi8(&src, &ret);
					handled = 1;
					break;
				case 0x39:
					INSTR_NAME("pminsd");
					ret = ssp_min_epi32(&src, &ret);
					handled = 1;
					break;
				case 0x3a:
					INSTR_NAME("pminuw");
					ret = ssp_min_epu16(&src, &ret);
					handled = 1;
					break;
				case 0x3b:
					INSTR_NAME("pminud");
					ret = ssp_min_epu32(&src, &ret);
					handled = 1;
					break;
				case 0x3c:
					INSTR_NAME("pmaxsb");
					ret = ssp_max_epi8(&src, &ret);
					handled = 1;
					break;
				case 0x3d:
					INSTR_NAME("pmaxsd");
					ret = ssp_max_epi32(&src, &ret);
					handled = 1;
					break;
				case 0x3e:
					INSTR_NAME("pmaxuw");
					ret = ssp_max_epu16(&src, &ret);
					handled = 1;
					break;
				case 0x3f:
					INSTR_NAME("pmaxud");
					ret = ssp_max_epu32(&src, &ret);
					handled = 1;
					break;
				case 0x40:
					INSTR_NAME("pmulld");
					ret = ssp_mullo_epi32(&ret, &src);
					handled = 1;
					break;
				case 0x41:
					INSTR_NAME("phminposuw");
					ret = ssp_minpos_epu16(&src);
					handled = 1;
					break;
				}

				if (handled) {
					setXMMRegister(dstIndex, testREX(prefixREX, REX_R), &ret);
					regs->ip += op_len;
				}
			}
		}
		else if (opcode.byte[1] == 0x3a) {
			ssp_m128 a, b, ret;
			int op_len, immValue;

			unsigned int aIndex = (opcode.byte[3]>>3) & 0x7;;
			a = getXMMRegister(aIndex, testREX(prefixREX, REX_R));

			// PINSRB family
			unsigned long memValue;
			if ((opcode.byte[2] == 0x20 || opcode.byte[2] == 0x22) &&
				((op_len = getOp2MemValue(opcode.byte[3], regs, prefixREX, &opcode.byte[4], &memValue)) != -1)) {
				immValue = opcode.byte[4 + op_len];
				op_len += 5;

				switch (opcode.byte[2]) {
				case 0x20:
					INSTR_NAME("pinsrb");
					ret = ssp_insert_epi8(&a, memValue, immValue);
					setXMMRegister(aIndex, testREX(prefixREX, REX_R), &ret);
					handled = 1;
					break;
				case 0x22:
					if (testREX(prefixREX, REX_W)) {
						INSTR_NAME("pinsrq");
						ret = ssp_insert_epi64(&a, memValue, immValue);
					}
					else {
						INSTR_NAME("pinsrd");
						ret = ssp_insert_epi32(&a, memValue, immValue);
					}
					setXMMRegister(aIndex, testREX(prefixREX, REX_R), &ret);
					handled = 1;
					break;
				}
			}

			// EXTRACTPS/PEXTRB family
			if (!handled && (opcode.byte[2] == 0x14 || opcode.byte[2] == 0x16 || opcode.byte[2] == 0x17)) {
				s64 extractValue;
				unsigned long memAddr = 0;
				int regIndex = 0;
				int dstLength = 0;
				if (opcode.byte[3] >= 0xc0) {
					immValue = opcode.byte[4];
					op_len = 5;
					regIndex = opcode.byte[3] & 0x7;
				}
				else {
					op_len = decodeMemAddress(opcode.byte[3], regs, prefixREX, &opcode.byte[4], &memAddr);
					if (op_len != -1) {
						immValue = opcode.byte[4 + op_len];
						op_len += 5;
					}
				}

				switch (opcode.byte[2]) {
				case 0x14:
					INSTR_NAME("pextrb");
					if (testREX(prefixREX, REX_W)) {
						dstLength = 8;
					}
					else {
						dstLength = 1;
					}
					extractValue= ssp_extract_epi8(&a, immValue);
					break;
				case 0x16:
					if (testREX(prefixREX, REX_W)) {
						INSTR_NAME("pextrq");
						extractValue = ssp_extract_epi64(&a, immValue);
						dstLength = 8;
					}
					else {
						INSTR_NAME("pextrd");
						extractValue = ssp_extract_epi32(&a, immValue);
						dstLength = 4;
					}
					break;
				case 0x17:
					INSTR_NAME("extractps");
					extractValue = ssp_extract_ps(&a, immValue);
					dstLength = 4;
					break;
				}

				if (memAddr && dstLength) {
					handled = !copy_to_user((void __user *)memAddr, &extractValue, dstLength);
				}
				else if (dstLength) {
					unsigned long *regPtr = getRegisterPtr(regIndex, regs, testREX(prefixREX, REX_B));
					switch (dstLength) {
					case 1:
						*regPtr &= ~0xffUL;
						*regPtr |= extractValue & 0xff;
						handled = 1;
						break;
					case 4:
						*regPtr &= ~0xffffffffUL;
						*regPtr |= extractValue & 0xffffffff;
						handled = 1;
						break;
					case 8:
						*regPtr = extractValue;
						handled = 1;
						break;
					}
				}
			}

			if (!handled && (op_len = getOp2XMMValue(opcode.byte[3], regs, prefixREX, &opcode.byte[4], &b)) != -1) {
				immValue = opcode.byte[4 + op_len];
				op_len += 5;

				switch (opcode.byte[2]) {
				case 0x08:
					INSTR_NAME("roundps");
					ret = ssp_round_ps(&b, immValue);
					handled = 1;
					setXMMRegister(aIndex, testREX(prefixREX, REX_R), &ret);
					break;
				case 0x09:
					INSTR_NAME("roundpd");
					ret = ssp_round_pd(&b, immValue);
					handled = 1;
					setXMMRegister(aIndex, testREX(prefixREX, REX_R), &ret);
					break;
				case 0x0a:
					INSTR_NAME("roundss");
					ret = ssp_round_ss(&a, &b, immValue);
					handled = 1;
					setXMMRegister(aIndex, testREX(prefixREX, REX_R), &ret);
					break;
				case 0x0b:
					INSTR_NAME("roundsd");
					ret = ssp_round_sd(&a, &b, immValue);
					handled = 1;
					setXMMRegister(aIndex, testREX(prefixREX, REX_R), &ret);
					break;
				case 0x0c:
					INSTR_NAME("blendps");
					ret = ssp_blend_ps(&a, &b, immValue);
					handled = 1;
					setXMMRegister(aIndex, testREX(prefixREX, REX_R), &ret);
					break;
				case 0x0d:
					INSTR_NAME("blendpd");
					ret = ssp_blend_pd(&a, &b, immValue);
					handled = 1;
					setXMMRegister(aIndex, testREX(prefixREX, REX_R), &ret);
					break;
				case 0x0e:
					INSTR_NAME("pblendw");
					ret = ssp_blend_epi16(&a, &b, immValue);
					handled = 1;
					setXMMRegister(aIndex, testREX(prefixREX, REX_R), &ret);
					break;
				case 0x0f:
					INSTR_NAME("palignr");
					ssp_alignr_epi8(&ret, &a, &b, immValue);
					handled = 1;
					setXMMRegister(aIndex, testREX(prefixREX, REX_R), &ret);
					break;
				case 0x21:
					INSTR_NAME("insertps");
					ret = ssp_insert_ps(&a, &b, immValue);
					handled = 1;
					setXMMRegister(aIndex, testREX(prefixREX, REX_R), &ret);
					break;
				case 0x40:
					INSTR_NAME("dpps");
					ret = ssp_dp_ps(&a, &b, immValue);
					handled = 1;
					setXMMRegister(aIndex, testREX(prefixREX, REX_R), &ret);
					break;
				case 0x41:
					INSTR_NAME("dppd");
					ret = ssp_dp_pd(&a, &b, immValue);
					handled = 1;
					setXMMRegister(aIndex, testREX(prefixREX, REX_R), &ret);
					break;
				case 0x42:
					INSTR_NAME("mpsadbw");
					ret = ssp_mpsadbw_epu8(&a, &b, immValue);
					handled = 1;
					setXMMRegister(aIndex, testREX(prefixREX, REX_R), &ret);
					break;
				}
			}

			if (handled) {
				regs->ip += op_len;
			}
		}
		else if (opcode.byte[1] == 0xb8 && opcode.byte[2] >= 0xc0) {
			// popcnt with memory addressing not supported yet
			unsigned int srcIndex = opcode.byte[2] & 0x7;
			unsigned int dstIndex = (opcode.byte[2] >> 3) & 0x7;
			int op_bytes = testREX(prefixREX, REX_W) ? 8 : (prefix66 ? 2 : 4);

			unsigned long regValue = *getRegisterPtr(srcIndex, regs, testREX(prefixREX, REX_B));
			unsigned long *dstReg = getRegisterPtr(dstIndex, regs, testREX(prefixREX, REX_R));

			switch (op_bytes) {
			case 2:
				INSTR_NAME("popcnt.16");
				*dstReg &= ~0xffffUL;
				*dstReg |= ssp_popcnt_16(regValue);
				break;
			case 4:
				INSTR_NAME("popcnt.32");
				*dstReg &= ~0xffffffffUL;
				*dstReg |= ssp_popcnt_32(regValue);
				break;
			case 8:
				INSTR_NAME("popcnt.64");
				*dstReg = ssp_popcnt_64(regValue);
				break;
			}

			handled = 1;
			regs->ip += 3;
		}
	}

#if DEBUG_INST_EMULATION
	u8 buf[32];
	copy_from_user((void *)buf, (const void __user *)(regs->ip - 16), sizeof(buf));
	pr_info("invalid opcode %s %8llx %4x handled: %d REX: %#x %s\n", __instr_name ? __instr_name : "UNKNOWN",
			swab64(*(u64*)&opcode.byte[0]), swab32(*(u32*)&opcode.byte[8]), handled, prefixREX, prefix66 ? "V" : "");
	pr_info("code around ip: \n");
	pr_info("%8llx %8llx %8llx %8llx\n", swab64(*(u64*)&buf[0]), swab64(*(u64*)&buf[8]),
			swab64(*(u64*)&buf[16]), swab64(*(u64*)&buf[24]));
#endif

	if (!handled) {
		if (notify_die(DIE_TRAP, "invalid opcode", regs, error_code,
			X86_TRAP_UD, SIGILL) == NOTIFY_STOP) {
			exception_exit(prev_state);
			return;
		}
		conditional_sti(regs);
		do_trap(X86_TRAP_UD, SIGILL, "invalid opcode", regs, error_code, &info);
	}
	exception_exit(prev_state);
}

#ifdef CONFIG_X86_64
/* Runs on IST stack */
dotraplinkage void do_double_fault(struct pt_regs *regs, long error_code)
{
	static const char str[] = "double fault";
	struct task_struct *tsk = current;

#ifdef CONFIG_X86_ESPFIX64
	extern unsigned char native_irq_return_iret[];

	/*
	 * If IRET takes a non-IST fault on the espfix64 stack, then we
	 * end up promoting it to a doublefault.  In that case, modify
	 * the stack to make it look like we just entered the #GP
	 * handler from user space, similar to bad_iret.
	 *
	 * No need for ist_enter here because we don't use RCU.
	 */
	if (((long)regs->sp >> PGDIR_SHIFT) == ESPFIX_PGD_ENTRY &&
		regs->cs == __KERNEL_CS &&
		regs->ip == (unsigned long)native_irq_return_iret)
	{
		struct pt_regs *normal_regs = task_pt_regs(current);

		/* Fake a #GP(0) from userspace. */
		memmove(&normal_regs->ip, (void *)regs->sp, 5*8);
		normal_regs->orig_ax = 0;  /* Missing (lost) #GP error code */
		regs->ip = (unsigned long)general_protection;
		regs->sp = (unsigned long)&normal_regs->orig_ax;

		return;
	}
#endif

	ist_enter(regs);
	notify_die(DIE_TRAP, str, regs, error_code, X86_TRAP_DF, SIGSEGV);

	tsk->thread.error_code = error_code;
	tsk->thread.trap_nr = X86_TRAP_DF;

#ifdef CONFIG_DOUBLEFAULT
	df_debug(regs, error_code);
#endif
	/*
	 * This is always a kernel trap and never fixable (and thus must
	 * never return).
	 */
	for (;;)
		die(str, regs, error_code);
}
#endif

dotraplinkage void do_bounds(struct pt_regs *regs, long error_code)
{
	const struct mpx_bndcsr *bndcsr;
	siginfo_t *info;

	RCU_LOCKDEP_WARN(!rcu_is_watching(), "entry code didn't wake RCU");
	if (notify_die(DIE_TRAP, "bounds", regs, error_code,
			X86_TRAP_BR, SIGSEGV) == NOTIFY_STOP)
		return;
	conditional_sti(regs);

	if (!user_mode(regs))
		die("bounds", regs, error_code);

	if (!cpu_feature_enabled(X86_FEATURE_MPX)) {
		/* The exception is not from Intel MPX */
		goto exit_trap;
	}

	/*
	 * We need to look at BNDSTATUS to resolve this exception.
	 * A NULL here might mean that it is in its 'init state',
	 * which is all zeros which indicates MPX was not
	 * responsible for the exception.
	 */
	bndcsr = get_xsave_field_ptr(XFEATURE_MASK_BNDCSR);
	if (!bndcsr)
		goto exit_trap;

	trace_bounds_exception_mpx(bndcsr);
	/*
	 * The error code field of the BNDSTATUS register communicates status
	 * information of a bound range exception #BR or operation involving
	 * bound directory.
	 */
	switch (bndcsr->bndstatus & MPX_BNDSTA_ERROR_CODE) {
	case 2:	/* Bound directory has invalid entry. */
		if (mpx_handle_bd_fault())
			goto exit_trap;
		break; /* Success, it was handled */
	case 1: /* Bound violation. */
		info = mpx_generate_siginfo(regs);
		if (IS_ERR(info)) {
			/*
			 * We failed to decode the MPX instruction.  Act as if
			 * the exception was not caused by MPX.
			 */
			goto exit_trap;
		}
		/*
		 * Success, we decoded the instruction and retrieved
		 * an 'info' containing the address being accessed
		 * which caused the exception.  This information
		 * allows and application to possibly handle the
		 * #BR exception itself.
		 */
		do_trap(X86_TRAP_BR, SIGSEGV, "bounds", regs, error_code, info);
		kfree(info);
		break;
	case 0: /* No exception caused by Intel MPX operations. */
		goto exit_trap;
	default:
		die("bounds", regs, error_code);
	}

	return;

exit_trap:
	/*
	 * This path out is for all the cases where we could not
	 * handle the exception in some way (like allocating a
	 * table or telling userspace about it.  We will also end
	 * up here if the kernel has MPX turned off at compile
	 * time..
	 */
	do_trap(X86_TRAP_BR, SIGSEGV, "bounds", regs, error_code, NULL);
}

dotraplinkage void
do_general_protection(struct pt_regs *regs, long error_code)
{
	struct task_struct *tsk;

	RCU_LOCKDEP_WARN(!rcu_is_watching(), "entry code didn't wake RCU");
	conditional_sti(regs);

	if (v8086_mode(regs)) {
		local_irq_enable();
		handle_vm86_fault((struct kernel_vm86_regs *) regs, error_code);
		return;
	}

	tsk = current;
	if (!user_mode(regs)) {
		if (fixup_exception(regs))
			return;

		tsk->thread.error_code = error_code;
		tsk->thread.trap_nr = X86_TRAP_GP;
		if (notify_die(DIE_GPF, "general protection fault", regs, error_code,
			       X86_TRAP_GP, SIGSEGV) != NOTIFY_STOP)
			die("general protection fault", regs, error_code);
		return;
	}

	tsk->thread.error_code = error_code;
	tsk->thread.trap_nr = X86_TRAP_GP;

	if (show_unhandled_signals && unhandled_signal(tsk, SIGSEGV) &&
			printk_ratelimit()) {
		pr_info("%s[%d] general protection ip:%lx sp:%lx error:%lx",
			tsk->comm, task_pid_nr(tsk),
			regs->ip, regs->sp, error_code);
		print_vma_addr(" in ", regs->ip);
		pr_cont("\n");
	}

	force_sig_info(SIGSEGV, SEND_SIG_PRIV, tsk);
}
NOKPROBE_SYMBOL(do_general_protection);

/* May run on IST stack. */
dotraplinkage void notrace do_int3(struct pt_regs *regs, long error_code)
{
#ifdef CONFIG_DYNAMIC_FTRACE
	/*
	 * ftrace must be first, everything else may cause a recursive crash.
	 * See note by declaration of modifying_ftrace_code in ftrace.c
	 */
	if (unlikely(atomic_read(&modifying_ftrace_code)) &&
	    ftrace_int3_handler(regs))
		return;
#endif
	if (poke_int3_handler(regs))
		return;

	ist_enter(regs);
	RCU_LOCKDEP_WARN(!rcu_is_watching(), "entry code didn't wake RCU");
#ifdef CONFIG_KGDB_LOW_LEVEL_TRAP
	if (kgdb_ll_trap(DIE_INT3, "int3", regs, error_code, X86_TRAP_BP,
				SIGTRAP) == NOTIFY_STOP)
		goto exit;
#endif /* CONFIG_KGDB_LOW_LEVEL_TRAP */

#ifdef CONFIG_KPROBES
	if (kprobe_int3_handler(regs))
		goto exit;
#endif

	if (notify_die(DIE_INT3, "int3", regs, error_code, X86_TRAP_BP,
			SIGTRAP) == NOTIFY_STOP)
		goto exit;

	/*
	 * Let others (NMI) know that the debug stack is in use
	 * as we may switch to the interrupt stack.
	 */
	debug_stack_usage_inc();
	preempt_conditional_sti(regs);
	do_trap(X86_TRAP_BP, SIGTRAP, "int3", regs, error_code, NULL);
	preempt_conditional_cli(regs);
	debug_stack_usage_dec();
exit:
	ist_exit(regs);
}
NOKPROBE_SYMBOL(do_int3);

#ifdef CONFIG_X86_64
/*
 * Help handler running on IST stack to switch off the IST stack if the
 * interrupted code was in user mode. The actual stack switch is done in
 * entry_64.S
 */
asmlinkage __visible notrace struct pt_regs *sync_regs(struct pt_regs *eregs)
{
	struct pt_regs *regs = task_pt_regs(current);
	*regs = *eregs;
	return regs;
}
NOKPROBE_SYMBOL(sync_regs);

struct bad_iret_stack {
	void *error_entry_ret;
	struct pt_regs regs;
};

asmlinkage __visible notrace
struct bad_iret_stack *fixup_bad_iret(struct bad_iret_stack *s)
{
	/*
	 * This is called from entry_64.S early in handling a fault
	 * caused by a bad iret to user mode.  To handle the fault
	 * correctly, we want move our stack frame to task_pt_regs
	 * and we want to pretend that the exception came from the
	 * iret target.
	 */
	struct bad_iret_stack *new_stack =
		container_of(task_pt_regs(current),
			     struct bad_iret_stack, regs);

	/* Copy the IRET target to the new stack. */
	memmove(&new_stack->regs.ip, (void *)s->regs.sp, 5*8);

	/* Copy the remainder of the stack from the current stack. */
	memmove(new_stack, s, offsetof(struct bad_iret_stack, regs.ip));

	BUG_ON(!user_mode(&new_stack->regs));
	return new_stack;
}
NOKPROBE_SYMBOL(fixup_bad_iret);
#endif

/*
 * Our handling of the processor debug registers is non-trivial.
 * We do not clear them on entry and exit from the kernel. Therefore
 * it is possible to get a watchpoint trap here from inside the kernel.
 * However, the code in ./ptrace.c has ensured that the user can
 * only set watchpoints on userspace addresses. Therefore the in-kernel
 * watchpoint trap can only occur in code which is reading/writing
 * from user space. Such code must not hold kernel locks (since it
 * can equally take a page fault), therefore it is safe to call
 * force_sig_info even though that claims and releases locks.
 *
 * Code in ./signal.c ensures that the debug control register
 * is restored before we deliver any signal, and therefore that
 * user code runs with the correct debug control register even though
 * we clear it here.
 *
 * Being careful here means that we don't have to be as careful in a
 * lot of more complicated places (task switching can be a bit lazy
 * about restoring all the debug state, and ptrace doesn't have to
 * find every occurrence of the TF bit that could be saved away even
 * by user code)
 *
 * May run on IST stack.
 */
dotraplinkage void do_debug(struct pt_regs *regs, long error_code)
{
	struct task_struct *tsk = current;
	int user_icebp = 0;
	unsigned long dr6;
	int si_code;

	ist_enter(regs);

	get_debugreg(dr6, 6);

	/* Filter out all the reserved bits which are preset to 1 */
	dr6 &= ~DR6_RESERVED;

	/*
	 * If dr6 has no reason to give us about the origin of this trap,
	 * then it's very likely the result of an icebp/int01 trap.
	 * User wants a sigtrap for that.
	 */
	if (!dr6 && user_mode(regs))
		user_icebp = 1;

	/* Catch kmemcheck conditions first of all! */
	if ((dr6 & DR_STEP) && kmemcheck_trap(regs))
		goto exit;

	/* DR6 may or may not be cleared by the CPU */
	set_debugreg(0, 6);

	/*
	 * The processor cleared BTF, so don't mark that we need it set.
	 */
	clear_tsk_thread_flag(tsk, TIF_BLOCKSTEP);

	/* Store the virtualized DR6 value */
	tsk->thread.debugreg6 = dr6;

#ifdef CONFIG_KPROBES
	if (kprobe_debug_handler(regs))
		goto exit;
#endif

	if (notify_die(DIE_DEBUG, "debug", regs, (long)&dr6, error_code,
							SIGTRAP) == NOTIFY_STOP)
		goto exit;

	/*
	 * Let others (NMI) know that the debug stack is in use
	 * as we may switch to the interrupt stack.
	 */
	debug_stack_usage_inc();

	/* It's safe to allow irq's after DR6 has been saved */
	preempt_conditional_sti(regs);

	if (v8086_mode(regs)) {
		handle_vm86_trap((struct kernel_vm86_regs *) regs, error_code,
					X86_TRAP_DB);
		preempt_conditional_cli(regs);
		debug_stack_usage_dec();
		goto exit;
	}

	/*
	 * Single-stepping through system calls: ignore any exceptions in
	 * kernel space, but re-enable TF when returning to user mode.
	 *
	 * We already checked v86 mode above, so we can check for kernel mode
	 * by just checking the CPL of CS.
	 */
	if ((dr6 & DR_STEP) && !user_mode(regs)) {
		tsk->thread.debugreg6 &= ~DR_STEP;
		set_tsk_thread_flag(tsk, TIF_SINGLESTEP);
		regs->flags &= ~X86_EFLAGS_TF;
	}
	si_code = get_si_code(tsk->thread.debugreg6);
	if (tsk->thread.debugreg6 & (DR_STEP | DR_TRAP_BITS) || user_icebp)
		send_sigtrap(tsk, regs, error_code, si_code);
	preempt_conditional_cli(regs);
	debug_stack_usage_dec();

exit:
	ist_exit(regs);
}
NOKPROBE_SYMBOL(do_debug);

/*
 * Note that we play around with the 'TS' bit in an attempt to get
 * the correct behaviour even in the presence of the asynchronous
 * IRQ13 behaviour
 */
static void math_error(struct pt_regs *regs, int error_code, int trapnr)
{
	struct task_struct *task = current;
	struct fpu *fpu = &task->thread.fpu;
	siginfo_t info;
	char *str = (trapnr == X86_TRAP_MF) ? "fpu exception" :
						"simd exception";

	if (notify_die(DIE_TRAP, str, regs, error_code, trapnr, SIGFPE) == NOTIFY_STOP)
		return;
	conditional_sti(regs);

	if (!user_mode(regs)) {
		if (!fixup_exception(regs)) {
			task->thread.error_code = error_code;
			task->thread.trap_nr = trapnr;
			die(str, regs, error_code);
		}
		return;
	}

	/*
	 * Save the info for the exception handler and clear the error.
	 */
	fpu__save(fpu);

	task->thread.trap_nr	= trapnr;
	task->thread.error_code = error_code;
	info.si_signo		= SIGFPE;
	info.si_errno		= 0;
	info.si_addr		= (void __user *)uprobe_get_trap_addr(regs);

	info.si_code = fpu__exception_code(fpu, trapnr);

	/* Retry when we get spurious exceptions: */
	if (!info.si_code)
		return;

	force_sig_info(SIGFPE, &info, task);
}

dotraplinkage void do_coprocessor_error(struct pt_regs *regs, long error_code)
{
	RCU_LOCKDEP_WARN(!rcu_is_watching(), "entry code didn't wake RCU");
	math_error(regs, error_code, X86_TRAP_MF);
}

dotraplinkage void
do_simd_coprocessor_error(struct pt_regs *regs, long error_code)
{
	RCU_LOCKDEP_WARN(!rcu_is_watching(), "entry code didn't wake RCU");
	math_error(regs, error_code, X86_TRAP_XF);
}

dotraplinkage void
do_spurious_interrupt_bug(struct pt_regs *regs, long error_code)
{
	conditional_sti(regs);
}

dotraplinkage void
do_device_not_available(struct pt_regs *regs, long error_code)
{
	RCU_LOCKDEP_WARN(!rcu_is_watching(), "entry code didn't wake RCU");
	BUG_ON(use_eager_fpu());

#ifdef CONFIG_MATH_EMULATION
	if (read_cr0() & X86_CR0_EM) {
		struct math_emu_info info = { };

		conditional_sti(regs);

		info.regs = regs;
		math_emulate(&info);
		return;
	}
#endif
	fpu__restore(&current->thread.fpu); /* interrupts still off */
#ifdef CONFIG_X86_32
	conditional_sti(regs);
#endif
}
NOKPROBE_SYMBOL(do_device_not_available);

#ifdef CONFIG_X86_32
dotraplinkage void do_iret_error(struct pt_regs *regs, long error_code)
{
	siginfo_t info;

	RCU_LOCKDEP_WARN(!rcu_is_watching(), "entry code didn't wake RCU");
	local_irq_enable();

	info.si_signo = SIGILL;
	info.si_errno = 0;
	info.si_code = ILL_BADSTK;
	info.si_addr = NULL;
	if (notify_die(DIE_TRAP, "iret exception", regs, error_code,
			X86_TRAP_IRET, SIGILL) != NOTIFY_STOP) {
		do_trap(X86_TRAP_IRET, SIGILL, "iret exception", regs, error_code,
			&info);
	}
}
#endif

/* Set of traps needed for early debugging. */
void __init early_trap_init(void)
{
	/*
	 * Don't use IST to set DEBUG_STACK as it doesn't work until TSS
	 * is ready in cpu_init() <-- trap_init(). Before trap_init(),
	 * CPU runs at ring 0 so it is impossible to hit an invalid
	 * stack.  Using the original stack works well enough at this
	 * early stage. DEBUG_STACK will be equipped after cpu_init() in
	 * trap_init().
	 *
	 * We don't need to set trace_idt_table like set_intr_gate(),
	 * since we don't have trace_debug and it will be reset to
	 * 'debug' in trap_init() by set_intr_gate_ist().
	 */
	set_intr_gate_notrace(X86_TRAP_DB, debug);
	/* int3 can be called from all */
	set_system_intr_gate(X86_TRAP_BP, &int3);
#ifdef CONFIG_X86_32
	set_intr_gate(X86_TRAP_PF, page_fault);
#endif
	load_idt(&idt_descr);
}

void __init early_trap_pf_init(void)
{
#ifdef CONFIG_X86_64
	set_intr_gate(X86_TRAP_PF, page_fault);
#endif
}

void __init trap_init(void)
{
	int i;

#ifdef CONFIG_EISA
	void __iomem *p = early_ioremap(0x0FFFD9, 4);

	if (readl(p) == 'E' + ('I'<<8) + ('S'<<16) + ('A'<<24))
		EISA_bus = 1;
	early_iounmap(p, 4);
#endif

	set_intr_gate(X86_TRAP_DE, divide_error);
	set_intr_gate_ist(X86_TRAP_NMI, &nmi, NMI_STACK);
	/* int4 can be called from all */
	set_system_intr_gate(X86_TRAP_OF, &overflow);
	set_intr_gate(X86_TRAP_BR, bounds);
	set_intr_gate(X86_TRAP_UD, invalid_op);
	set_intr_gate(X86_TRAP_NM, device_not_available);
#ifdef CONFIG_X86_32
	set_task_gate(X86_TRAP_DF, GDT_ENTRY_DOUBLEFAULT_TSS);
#else
	set_intr_gate_ist(X86_TRAP_DF, &double_fault, DOUBLEFAULT_STACK);
#endif
	set_intr_gate(X86_TRAP_OLD_MF, coprocessor_segment_overrun);
	set_intr_gate(X86_TRAP_TS, invalid_TSS);
	set_intr_gate(X86_TRAP_NP, segment_not_present);
	set_intr_gate(X86_TRAP_SS, stack_segment);
	set_intr_gate(X86_TRAP_GP, general_protection);
	set_intr_gate(X86_TRAP_SPURIOUS, spurious_interrupt_bug);
	set_intr_gate(X86_TRAP_MF, coprocessor_error);
	set_intr_gate(X86_TRAP_AC, alignment_check);
#ifdef CONFIG_X86_MCE
	set_intr_gate_ist(X86_TRAP_MC, &machine_check, MCE_STACK);
#endif
	set_intr_gate(X86_TRAP_XF, simd_coprocessor_error);

	/* Reserve all the builtin and the syscall vector: */
	for (i = 0; i < FIRST_EXTERNAL_VECTOR; i++)
		set_bit(i, used_vectors);

#ifdef CONFIG_IA32_EMULATION
	set_system_intr_gate(IA32_SYSCALL_VECTOR, entry_INT80_compat);
	set_bit(IA32_SYSCALL_VECTOR, used_vectors);
#endif

#ifdef CONFIG_X86_32
	set_system_trap_gate(IA32_SYSCALL_VECTOR, entry_INT80_32);
	set_bit(IA32_SYSCALL_VECTOR, used_vectors);
#endif

	/*
	 * Set the IDT descriptor to a fixed read-only location, so that the
	 * "sidt" instruction will not leak the location of the kernel, and
	 * to defend the IDT against arbitrary memory write vulnerabilities.
	 * It will be reloaded in cpu_init() */
	__set_fixmap(FIX_RO_IDT, __pa_symbol(idt_table), PAGE_KERNEL_RO);
	idt_descr.address = fix_to_virt(FIX_RO_IDT);

	/*
	 * Should be a barrier for any external CPU state:
	 */
	cpu_init();

	/*
	 * X86_TRAP_DB and X86_TRAP_BP have been set
	 * in early_trap_init(). However, ITS works only after
	 * cpu_init() loads TSS. See comments in early_trap_init().
	 */
	set_intr_gate_ist(X86_TRAP_DB, &debug, DEBUG_STACK);
	/* int3 can be called from all */
	set_system_intr_gate_ist(X86_TRAP_BP, &int3, DEBUG_STACK);

	x86_init.irqs.trap_init();

#ifdef CONFIG_X86_64
	memcpy(&debug_idt_table, &idt_table, IDT_ENTRIES * 16);
	set_nmi_gate(X86_TRAP_DB, &debug);
	set_nmi_gate(X86_TRAP_BP, &int3);
#endif
}
