#include <dlfcn.h>
#include <stdio.h>

typedef int (*demo_add_fn_t)(int, int);
typedef const char *(*demo_name_fn_t)(void);

int main(void)
{
    void *h;
    demo_add_fn_t demo_add;
    demo_name_fn_t demo_name;
    const char *name;
    int sum;

    h = dlopen("/lib/libdemo.so", RTLD_NOW);
    if (h == NULL) {
        const char *e = dlerror();
        fprintf(stderr, "dlhello: dlopen failed: %s\n", e ? e : "unknown");
        return 1;
    }

    demo_add = (demo_add_fn_t)dlsym(h, "demo_add");
    if (demo_add == NULL) {
        const char *e = dlerror();
        fprintf(stderr, "dlhello: dlsym demo_add failed: %s\n", e ? e : "unknown");
        (void)dlclose(h);
        return 2;
    }

    demo_name = (demo_name_fn_t)dlsym(h, "demo_name");
    if (demo_name == NULL) {
        const char *e = dlerror();
        fprintf(stderr, "dlhello: dlsym demo_name failed: %s\n", e ? e : "unknown");
        (void)dlclose(h);
        return 3;
    }

    name = demo_name();
    sum = demo_add(20, 22);
    printf("dlhello: %s sum=%d\n", name ? name : "?", sum);

    if (dlclose(h) != 0) {
        const char *e = dlerror();
        fprintf(stderr, "dlhello: dlclose failed: %s\n", e ? e : "unknown");
        return 4;
    }

    return 0;
}
