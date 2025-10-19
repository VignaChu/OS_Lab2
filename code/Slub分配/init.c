#include <console.h>
#include <defs.h>
#include <pmm.h>
#include <stdio.h>
#include <string.h>
#include <dtb.h>
#include <slub.h>

int kern_init(void) __attribute__((noreturn));
void grade_backtrace(void);
void print_kerninfo(void);

/* *
 * print_kerninfo - print the information about kernel, including the location
 * of kernel entry, the start addresses of data and text segements, the start
 * address of free memory and how many memory that kernel has used.
 * */
void print_kerninfo(void) {
    extern char etext[], edata[], end[];
    cprintf("Special kernel symbols:\n");
    cprintf("  entry  0x%016lx (virtual)\n", (uintptr_t)kern_init);
    cprintf("  etext  0x%016lx (virtual)\n", etext);
    cprintf("  edata  0x%016lx (virtual)\n", edata);
    cprintf("  end    0x%016lx (virtual)\n", end);
    cprintf("Kernel executable memory footprint: %dKB\n",
            (end - (char*)kern_init + 1023) / 1024);
}
extern void slub_init(void);
extern void slub_selftest(void);
extern void run_slub_tests(void);

int kern_init(void) {
    extern char edata[], end[];
    memset(edata, 0, end - edata);
    dtb_init();
    cons_init();

    const char *message = "(THU.CST) os is loading ...\0";
    cputs(message);

    print_kerninfo();

    pmm_init();

    slub_init();

    // 调测试 //
    cprintf("[slub] ### run_slub_tests entry ###\n");
    run_slub_tests();
    cprintf("[slub] ### run_slub_tests leave ###\n");

    while (1) ;
}


