/* Freestanding, kein libc: rohe Syscalls, damit unser Mini-Linker
   ohne Dynamic Linking und ohne libc auskommt. */

extern void print_message(void);   /* definiert in msg.c -> testet Symbolauflösung über Dateigrenzen */
extern int  g_call_count;          /* definiert in msg.c -> testet Datensymbol-Relocation */

static long sys_exit(int code) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(60), "D"(code)
        : "rcx", "r11", "memory"
    );
    return ret;
}

void _start(void) {
    print_message();
    print_message();
    sys_exit(g_call_count);   /* Exit-Code = Anzahl der Aufrufe -> beweist, dass beide Objektdateien
                                  denselben Speicher für g_call_count referenzieren */
}
