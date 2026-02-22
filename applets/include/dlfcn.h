#ifndef XV6_DLFNC_H
#define XV6_DLFNC_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RTLD_LAZY
#define RTLD_LAZY 0x00001
#endif
#ifndef RTLD_NOW
#define RTLD_NOW 0x00002
#endif
#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0x00004
#endif
#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0x00008
#endif

void *dlopen(const char *file, int mode);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
const char *dlerror(void);

#ifdef __cplusplus
}
#endif

#endif
