/* Ember NX32 startup: header, entry point and interrupt stubs */

        .section .nxhdr,"a"
        .globl  nx_header
nx_header:
        .ascii  "NX32"
        .long   0x110000                /* load address */
        .long   _start                  /* entry */
        .long   __data_end              /* end of the file image */
        .long   __bss_end               /* end of bss */
        .long   65536                   /* stack size */
        .long   0, 0

        .text
        .globl  _start
_start:
        movl    %ebx, nx_info_raw       /* EBX -> app_info from the kernel */
        xorl    %ebp, %ebp
        call    nano_main
1:      hlt
        jmp     1b

/* The kernel far-calls these from its IRQ handlers (ring 0, flat DS/ES). */
        .globl  nx_irq0_stub
nx_irq0_stub:
        pushal
        movl    nx_irq0_fn, %eax
        testl   %eax, %eax
        jz      1f
        call    *%eax
1:      popal
        lret

        .globl  nx_irq1_stub
nx_irq1_stub:
        pushal
        movl    nx_irq1_fn, %eax
        testl   %eax, %eax
        jz      1f
        call    *%eax
1:      popal
        lret

        .globl  nx_irq12_stub
nx_irq12_stub:
        pushal
        movl    nx_irq12_fn, %eax
        testl   %eax, %eax
        jz      1f
        call    *%eax
1:      popal
        lret
