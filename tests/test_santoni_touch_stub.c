/* The kernel touch source is compiled separately, with WHYRED disabled.
 * Regression for CI 42348926676: the test's errno.h cannot supply ENXIO to
 * another translation unit that forgot its own common BSD errno include. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>

int santoni_touch_probe(void);

int
main(void)
{
	assert(santoni_touch_probe() == ENXIO);
	assert(santoni_touch_probe() == ENXIO);
	puts("test_santoni_touch_stub: unsupported-board entry point passed");
	return 0;
}
