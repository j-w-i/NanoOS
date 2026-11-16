.syntax unified
.cpu cortex-a5
.thumb
.section .game_header,"a",%progbits
.balign 4
/* struct ngame_hdr, canonical gameabi.h: size and CRC patched by mkgame.py. */
.word 0x314d474e, 1, 0x0b000000, 0
.word __bss_end, app_entry, 0, 0
.section .text.app_entry,"ax",%progbits
.global app_entry
.type app_entry,%function
.thumb_func
app_entry:
    push {r4, r5, r6, lr}
    mov r4, sp
    ldr r1, =__stack_top
    mov sp, r1
    mov r5, r0                    /* keep the services table for nano_example_main */
    ldr r6, =__init_array_start   /* run static constructors first */
1:  ldr r3, =__init_array_end
    cmp r6, r3
    bhs 2f
    ldr r3, [r6], #4
    blx r3
    b 1b
2:  mov r0, r5
    bl nano_example_main
    mov sp, r4
    pop {r4, r5, r6, pc}
.size app_entry, .-app_entry
.section .note.GNU-stack,"",%progbits
