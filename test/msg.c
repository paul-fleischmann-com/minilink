int g_call_count = 0;   /* .data Symbol -> nicht null initialisiert, landet in .data nicht .bss */
int g_call_count_notinit;   /* .data Symbol -> nicht null initialisiert, landet in .data nicht .bss */
int g_flags =0xABCD;

static const char msg[] = "Hello from mini-linker!\n";  /* .rodata */

static long sys_write(int fd, const void *buf, unsigned long count) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(1), "D"(fd), "S"(buf), "d"(count)
        : "rcx", "r11", "memory"
    );
    return ret;
}

void print_message(void) {
    g_call_count = g_call_count + 1;
    g_call_count_notinit = 14U;
    sys_write(1, msg, sizeof(msg) - 1);
}
