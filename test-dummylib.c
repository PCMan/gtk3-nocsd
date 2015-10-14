/* Dummy test library.
 *
 * This library will be dlopen()d by test-static-tls in multiple different
 * versions to see if libgtk3-nocsd.so uses up a static TLS entry, which
 * it shouldn't.
 */

static __thread int i = 0;

#define NAME2_HIDDEN(a, b) a ## b
#define NAME2(a, b) NAME2_HIDDEN(a,b)

int *NAME2(testlib_dummy_get_, TESTLIB_NAME) ()
{
  return &i;
}
