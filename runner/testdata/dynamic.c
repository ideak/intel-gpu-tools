#include "igt.h"

igt_main
{
	igt_subtest_with_dynamic("dynamic-subtest") {
		igt_dynamic("failing")
			igt_assert(false);

		igt_dynamic("passing")
			;
	}
}
