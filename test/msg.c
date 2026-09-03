int g_call_count = 0;   /* .data Symbol -> nicht null initialisiert, landet in .data nicht .bss */
int g_call_count_notinit;   /* .data Symbol -> nicht null initialisiert, landet in .data nicht .bss */
int g_flags =0xABCD;

int g_flags_Array[19u] ={0xABCD};

/* 64-bit Array: Elementtyp long long (8 Byte auf x86-64 LP64).
   Initialwert != 0 -> landet in .data, sonst .bss. */
long long g_buf64[18] = { 0x1122334455667788LL };

static const char msg[]  = "Hello from mini-linker!\n";    /* .rodata */

/* msg2 ist ein Zeiger (kein Array): erst NULL, wird in print_message()
   auf ein String-Literal gesetzt. Ein Array liesse sich nicht im
   Funktionskoerper "initialisieren" -- Arrays sind nicht zuweisbar. */
static const char *msg2;   /* .bss (0-initialisiert) */
static const char *msg3;   /* .bss (0-initialisiert) */

/* Ersatz fuer strlen() -- wir haben keine libc */
static unsigned long slen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

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
    g_flags_Array[1]=0xABCD;
    g_buf64[2] = g_buf64[0] + g_call_count;

    msg2 = "Hello from mini-linker 2 Hello from mini-linker 2 !\n";   /* hier initialisiert */
    msg3 = "Hello from mini-linker 2 Hello from mini-linker 3 !\n";   /* hier initialisiert */
    sys_write(1, msg,  sizeof(msg) - 1);
    sys_write(1, msg2, slen(msg2));
    sys_write(1, msg3, slen(msg3));
}
