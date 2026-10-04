/* Link-time stand-ins for the guest's libdl.so, liblog.so and libc.so (guest/media.c links
 * against them for their names; at run time the real ones are used). */
void *dlopen(const char *path, int flags) { (void)path; (void)flags; return 0; }
void *dlsym(void *handle, const char *name) { (void)handle; (void)name; return 0; }
char *dlerror(void) { return 0; }
int __android_log_print(int prio, const char *tag, const char *fmt, ...) { (void)prio; (void)tag; (void)fmt; return 0; }
int pthread_create(void *thread, const void *attr, void *(*fn)(void *), void *arg) { (void)thread; (void)attr; (void)fn; (void)arg; return 0; }
int pthread_detach(unsigned long thread) { (void)thread; return 0; }
int prctl(int option, ...) { (void)option; return 0; }
