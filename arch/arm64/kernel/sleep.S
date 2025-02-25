#include <linux/errno.h>
#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/assembler.h>

	.text
/*
 * Implementation of MPIDR_EL1 hash algorithm through shifting
 * and OR'ing.
 *
 * @dst: register containing hash result
 * @rs0: register containing affinity level 0 bit shift
 * @rs1: register containing affinity level 1 bit shift
 * @rs2: register containing affinity level 2 bit shift
 * @rs3: register containing affinity level 3 bit shift
 * @mpidr: register containing MPIDR_EL1 value
 * @mask: register containing MPIDR mask
 *
 * Pseudo C-code:
 *
 *u32 dst;
 *
 *compute_mpidr_hash(u32 rs0, u32 rs1, u32 rs2, u32 rs3, u64 mpidr, u64 mask) {
 *	u32 aff0, aff1, aff2, aff3;
 *	u64 mpidr_masked = mpidr & mask;
 *	aff0 = mpidr_masked & 0xff;
 *	aff1 = mpidr_masked & 0xff00;
 *	aff2 = mpidr_masked & 0xff0000;
 *	aff2 = mpidr_masked & 0xff00000000;
 *	dst = (aff0 >> rs0 | aff1 >> rs1 | aff2 >> rs2 | aff3 >> rs3);
 *}
 * Input registers: rs0, rs1, rs2, rs3, mpidr, mask
 * Output register: dst
 * Note: input and output registers must be disjoint register sets
         (eg: a macro instance with mpidr = x1 and dst = x1 is invalid)
 */
	.macro compute_mpidr_hash dst, rs0, rs1, rs2, rs3, mpidr, mask
	and	\mpidr, \mpidr, \mask		// mask out MPIDR bits
	and	\dst, \mpidr, #0xff		// mask=aff0
	lsr	\dst ,\dst, \rs0		// dst=aff0>>rs0
	and	\mask, \mpidr, #0xff00		// mask = aff1
	lsr	\mask ,\mask, \rs1
	orr	\dst, \dst, \mask		// dst|=(aff1>>rs1)
	and	\mask, \mpidr, #0xff0000	// mask = aff2
	lsr	\mask ,\mask, \rs2
	orr	\dst, \dst, \mask		// dst|=(aff2>>rs2)
	and	\mask, \mpidr, #0xff00000000	// mask = aff3
	lsr	\mask ,\mask, \rs3
	orr	\dst, \dst, \mask		// dst|=(aff3>>rs3)
	.endm
/*
 * Save CPU state for a suspend.  This saves callee registers, and allocates
 * space on the kernel stack to save the CPU specific registers + some
 * other data for resume.
 *
 *  x0 = suspend finisher argument
 */
ENTRY(__cpu_suspend)
	stp	x29, lr, [sp, #-96]!
	stp	x19, x20, [sp,#16]
	stp	x21, x22, [sp,#32]
	stp	x23, x24, [sp,#48]
	stp	x25, x26, [sp,#64]
	stp	x27, x28, [sp,#80]
	mov	x2, sp
	sub	sp, sp, #CPU_SUSPEND_SZ	// allocate cpu_suspend_ctx
	mov	x1, sp
	/*
	 * x1 now points to struct cpu_suspend_ctx allocated on the stack
	 */
	str	x2, [x1, #CPU_CTX_SP]
	ldr	x2, =sleep_save_sp
	ldr	x2, [x2, #SLEEP_SAVE_SP_VIRT]
#ifdef CONFIG_SMP
	mrs	x7, mpidr_el1
	ldr	x9, =mpidr_hash
	ldr	x10, [x9, #MPIDR_HASH_MASK]
	/*
	 * Following code relies on the struct mpidr_hash
	 * members size.
	 */
	ldp	w3, w4, [x9, #MPIDR_HASH_SHIFTS]
	ldp	w5, w6, [x9, #(MPIDR_HASH_SHIFTS + 8)]
	compute_mpidr_hash x8, x3, x4, x5, x6, x7, x10
	add	x2, x2, x8, lsl #3
#endif
	bl	__cpu_suspend_finisher
        /*
	 * Never gets here, unless suspend fails.
	 * Successful cpu_suspend should return from cpu_resume, returning
	 * through this code path is considered an error
	 * If the return value is set to 0 force x0 = -EOPNOTSUPP
	 * to make sure a proper error condition is propagated
	 */
	cmp	x0, #0
	mov	x3, #-EOPNOTSUPP
	csel	x0, x3, x0, eq
	add	sp, sp, #CPU_SUSPEND_SZ	// rewind stack pointer
	ldp	x19, x20, [sp, #16]
	ldp	x21, x22, [sp, #32]
	ldp	x23, x24, [sp, #48]
	ldp	x25, x26, [sp, #64]
	ldp	x27, x28, [sp, #80]
	ldp	x29, lr, [sp], #96
	ret
ENDPROC(__cpu_suspend)
	.ltorg

/*
 * x0 must contain the sctlr value retrieved from restored context
 */
ENTRY(cpu_resume_mmu)
	ldr	x3, =cpu_resume_after_mmu
	msr	sctlr_el1, x0		// restore sctlr_el1
	isb
	/*
	 * Invalidate the local I-cache so that any instructions fetched
	 * speculatively from the PoC are discarded, since they may have
	 * been dynamically patched at the PoU.
	 */
	ic	iallu
	dsb	nsh
	isb
	br	x3			// global jump to virtual address
ENDPROC(cpu_resume_mmu)
cpu_resume_after_mmu:
	mov	x0, #0			// return zero on success
	ldp	x19, x20, [sp, #16]
	ldp	x21, x22, [sp, #32]
	ldp	x23, x24, [sp, #48]
	ldp	x25, x26, [sp, #64]
	ldp	x27, x28, [sp, #80]
	ldp	x29, lr, [sp], #96
	ret
ENDPROC(cpu_resume_after_mmu)

	.data
ENTRY(cpu_resume)
	bl	el2_setup		// if in EL2 drop to EL1 cleanly
#ifdef CONFIG_SMP
	mrs	x1, mpidr_el1
	adr	x4, mpidr_hash_ptr
	ldr	x5, [x4]
	add	x8, x4, x5		// x8 = struct mpidr_hash phys address
        /* retrieve mpidr_hash members to compute the hash */
	ldr	x2, [x8, #MPIDR_HASH_MASK]
	ldp	w3, w4, [x8, #MPIDR_HASH_SHIFTS]
	ldp	w5, w6, [x8, #(MPIDR_HASH_SHIFTS + 8)]
	compute_mpidr_hash x7, x3, x4, x5, x6, x1, x2
        /* x7 contains hash index, let's use it to grab context pointer */
#else
	mov	x7, xzr
#endif
	adr	x0, sleep_save_sp
	ldr	x0, [x0, #SLEEP_SAVE_SP_PHYS]
	ldr	x0, [x0, x7, lsl #3]
	/* load sp from context */
	ldr	x2, [x0, #CPU_CTX_SP]
	adr	x1, sleep_idmap_phys
	/* load physical address of identity map page table in x1 */
	ldr	x1, [x1]
	mov	sp, x2
	/*
	 * cpu_do_resume expects x0 to contain context physical address
	 * pointer and x1 to contain physical address of 1:1 page tables
	 */
	bl	cpu_do_resume		// PC relative jump, MMU off
	b	cpu_resume_mmu		// Resume MMU, never returns
ENDPROC(cpu_resume)

	.align 3
mpidr_hash_ptr:
	/*
	 * offset of mpidr_hash symbol from current location
	 * used to obtain run-time mpidr_hash address with MMU off
         */
	.quad	mpidr_hash - .
/*
 * physical address of identity mapped page tables
 */
	.type	sleep_idmap_phys, #object
ENTRY(sleep_idmap_phys)
	.quad	0
/*
 * struct sleep_save_sp {
 *	phys_addr_t *save_ptr_stash;
 *	phys_addr_t save_ptr_stash_phys;
 * };
 */
	.type	sleep_save_sp, #object
ENTRY(sleep_save_sp)
	.space	SLEEP_SAVE_SP_SZ	// struct sleep_save_sp
