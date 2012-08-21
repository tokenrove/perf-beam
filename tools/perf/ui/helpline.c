#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../debug.h"
#include "helpline.h"
#include "ui.h"

char ui_helpline__current[512];

static void nop_helpline__pop(void)
{
}

static void nop_helpline__push(const char *msg __used)
{
}

static struct ui_helpline default_helpline_fns = {
	.pop	= nop_helpline__pop,
	.push	= nop_helpline__push,
};

struct ui_helpline *helpline_fns = &default_helpline_fns;

void ui_helpline__pop(void)
{
	helpline_fns->pop();
}

void ui_helpline__push(const char *msg)
{
	helpline_fns->push(msg);
}

void ui_helpline__vpush(const char *fmt, va_list ap)
{
	char *s;

	if (vasprintf(&s, fmt, ap) < 0)
		vfprintf(stderr, fmt, ap);
	else {
		ui_helpline__push(s);
		free(s);
	}
}

void ui_helpline__fpush(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	ui_helpline__vpush(fmt, ap);
	va_end(ap);
}

void ui_helpline__puts(const char *msg)
{
	ui_helpline__pop();
	ui_helpline__push(msg);
}
