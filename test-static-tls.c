/*
 * test-static-tls: Verify that libgtk3-nocsd.so doesn't use up an
 * overflow DTV entry in the loaded program
 *
 * Logic is simple: try to dlopen() as many different libraries as
 * possible that contain static TLS entries (62 were built by the
 * Makefile), so that it can be determined what the cutoff is for
 * the case with and without LD_PRELOAD. (The cutoff is limited by
 * the number of overflow static DTV entries the dynamic linker
 * users by default.)
 *
 * This program will output the number of libraries it could load.
 *
 * Threading is necessary, otherwise the DTV will not be used.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

static const char *preloaded = "none";

static void *doit(void *dummy)
{
  char buf[]  = "testlibs/libdummy-_.so.0"; /* replace char @ index 18 (_) */
  char buf2[] = "testlib_dummy_get__";      /* replace char @ index 18 (_) */
  void *hdl;
  int *(*fn)();
  int c;
  int lastlib = -1;
  int *ptr;

  for (c = 0; c < sizeof(alphabet) - 1; c++) {
    buf[18] = alphabet[c];
    buf2[18] = alphabet[c];
    hdl = dlopen(buf, RTLD_NOW | RTLD_GLOBAL);
    if (!hdl) {
      if (lastlib < 0) {
        printf("ERROR[preloaded = %s]: couldn't load ANY library at all: %s\n", preloaded, dlerror());
        break;
      }
      printf("%d\n", lastlib + 1);
      break;
    }
    fn = (int *(*)())dlsym(hdl, buf2);
    if (!fn) {
      printf("ERROR[preloaded = %s]: symbol %s not found: %s\n", preloaded, buf2, dlerror());
      break;
    }
    ptr = fn();
    if (*ptr != 0) {
      printf("ERROR[preloaded = %s]: function %s did not give expected result\n", preloaded, buf2);
      break;
    }
    lastlib = c;
  }

  if (c == sizeof(alphabet) - 1)
    printf("%d\n", lastlib + 1);

  return dummy;
}

int main(int argc, char **argv)
{
  int r;
  pthread_t thread;

  if (argc >= 2)
    preloaded = argv[1];

  r = pthread_create(&thread, NULL, doit, NULL);
  if (r < 0) {
    printf("ERROR[preloaded = %s]: could not create thread: %s\n", preloaded, strerror(errno));
    return 1;
  }
  (void) pthread_join(thread, NULL);
  return 0;
}
