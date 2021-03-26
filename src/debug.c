#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#include "console.h"

void __assert_func(const char *file, int line, const char *func, const char *failedexpr)
{
	console_init();
	console_set_render_mode(RENDER_MANUAL);
	fprintf(stdout,
		"assertion \"%s\" failed: file \"%s\", line %d%s%s\n",
		failedexpr, file, line,
		func ? ", function: " : "", func ? func : "");
	console_render();
	abort();
}