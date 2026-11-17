/* fiq.s -- FIQ vector for the interrupt-driven TA FIFO feeder (mbx.c, MBX_FIFO_FIQ).
 *
 * NanoOS runs with IRQs masked. The feeder routes only VIC IRQ 8 (timer block) to the
 * CPU's FIQ line and installs this table through VBAR while a game runs: the FIQ slot
 * calls nano_fiq_handler, every other slot jumps to the vector the CPU used before
 * (filled in by nano_vectors_install), so exceptions behave as without the feeder.
 * FIQ mode has banked r8-r12/sp/lr; r0-r3, r12 and lr are saved around the C call. */
.syntax unified
.cpu cortex-a5
.arm
.section .text.nano_vectors,"ax"
.align 5
.global nano_vectors
nano_vectors:
    ldr pc, nano_vec_old + 0
    b nano_exc_und
    ldr pc, nano_vec_old + 8
    b nano_exc_pabt
    b nano_exc_dabt
    ldr pc, nano_vec_old + 20
    ldr pc, nano_vec_old + 24
    b nano_fiq_entry
.global nano_vec_old
nano_vec_old:
    .word 0, 0, 0, 0, 0, 0, 0, 0

/* Undefined / abort while the table is installed: record where (nano_exc_info: kind, lr,
 * spsr, count) for diagnosis, then continue into the vector used before. */
nano_exc_und:
    push {r0, r1}
    mov r1, #1
    b nano_exc_rec
nano_exc_pabt:
    push {r0, r1}
    mov r1, #3
    b nano_exc_rec
nano_exc_dabt:
    push {r0, r1}
    mov r1, #4
nano_exc_rec:
    ldr r0, =nano_exc_info
    str r1, [r0]
    str lr, [r0, #4]
    mrs r1, spsr
    str r1, [r0, #8]
    ldr r1, [r0, #12]
    add r1, r1, #1
    str r1, [r0, #12]
    mrc p15, 0, r1, c5, c0, 0
    str r1, [r0, #16]
    mrc p15, 0, r1, c6, c0, 0
    str r1, [r0, #20]
    ldr r1, [r0]
    cmp r1, #1
    pop {r0, r1}
    ldreq pc, nano_vec_old + 4
    cmp r1, r1
    ldr pc, nano_vec_old + 16
.ltorg
.global nano_exc_info
.section .bss.nano_exc_info,"aw",%nobits
.align 4
nano_exc_info:
    .space 32
.section .text.nano_vectors,"ax"
nano_fiq_entry:
    push {r0-r3, r12, lr}
    bl nano_fiq_handler
    pop {r0-r3, r12, lr}
    subs pc, lr, #4

/* uint32_t nano_vbar_get(void) / void nano_vbar_set(uint32_t) */
.global nano_vbar_get
.type nano_vbar_get, %function
nano_vbar_get:
    mrc p15, 0, r0, c12, c0, 0
    bx lr
.global nano_vbar_set
.type nano_vbar_set, %function
nano_vbar_set:
    mcr p15, 0, r0, c12, c0, 0
    isb
    bx lr
/* uint32_t nano_sctlr_get(void) / void nano_sctlr_set(uint32_t) */
.global nano_sctlr_get
.type nano_sctlr_get, %function
nano_sctlr_get:
    mrc p15, 0, r0, c1, c0, 0
    bx lr
.global nano_sctlr_set
.type nano_sctlr_set, %function
nano_sctlr_set:
    mcr p15, 0, r0, c1, c0, 0
    isb
    bx lr
/* void nano_fiq_stack(uint32_t top): set the banked FIQ-mode sp, return in the caller's mode */
.global nano_fiq_stack
.type nano_fiq_stack, %function
nano_fiq_stack:
    mrs r1, cpsr
    cpsid if, #0x11
    mov sp, r0
    msr cpsr_cxsf, r1
    isb
    bx lr
/* void nano_fiq_enable(void) / void nano_fiq_disable(void) */
.global nano_fiq_enable
.type nano_fiq_enable, %function
nano_fiq_enable:
    cpsie f
    bx lr
.global nano_fiq_disable
.type nano_fiq_disable, %function
nano_fiq_disable:
    cpsid f
    bx lr
