#include <stdint.h>

int demo_add(int a, int b)
{
    return a + b;
}

const char *demo_name(void)
{
    return "libdemo";
}

int libdemo_anchor(void)
{
    return (int)(uintptr_t)&demo_add;
}
