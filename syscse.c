/* 
  Copyright (C) 2007 Arnaldo Carvalho de Melo <acme@ghostprotocols.net>

  System call sign extender

  This program is free software; you can redistribute it and/or modify it
  under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.
*/

#include <argp.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dwarves.h"

static const char *prefix = "sys_";
static size_t prefix_len = 4;

static struct tag *filter(struct tag *self, struct cu *cu,
			  void *cookie __unused)
{
	if (self->tag == DW_TAG_subprogram) {
		struct function *f = tag__function(self);

		if (f->proto.nr_parms != 0) {
			const char *name = function__name(f, cu);

			if (strlen(name) > prefix_len &&
			    memcmp(name, prefix, prefix_len) == 0)
				return self;
		}
	}
	return NULL;
}

static void zero_extend(const int regparm, const struct base_type *bt,
			const char *parm)
{
	const char *instr = "INVALID";

	switch (bt->size) {
	case 4: /* 32 bits */
		instr = "sll";
		break;
	case 2: /* 16 bits */
		instr = "slw";
		break;
	case 1: /* 8 bits */
		instr = "slb";
		break;
	}

	printf("\t%s\t$a%d, $a%d, 0"
	       "\t/* zero extend $a%d(%s %s) from %zd to 64-bit */\n",
	       instr, regparm, regparm, regparm, bt->name, parm, bt->size * 8);
}

static int emit_wrapper(struct tag *self, struct cu *cu, void *cookie __unused)
{
	struct parameter *parm;
	struct function *f = tag__function(self);
	const char *name = function__name(f, cu);
	int regparm = 0, needs_wrapper = 0;

	function__for_each_parameter(f, parm) {
		const Dwarf_Off type_id = parameter__type(parm, cu);
		struct tag *type = cu__find_tag_by_id(cu, type_id);

		assert(type != NULL);
		if (type->tag == DW_TAG_base_type) {
			struct base_type *bt = tag__base_type(type);

			if (bt->size < 8 &&
			    strncmp(bt->name, "unsigned", 8) == 0) {
				if (!needs_wrapper) {
					printf("wrap_%s:\n", name);
					needs_wrapper = 1;
				}
				zero_extend(regparm, bt,
					    parameter__name(parm, cu));
			}
		}
		++regparm;
	}

	if (needs_wrapper)
		printf("\tj\t%s\n\n", name);


	return 0;
}

static int cu__emit_wrapper(struct cu *self, void *cookie __unused)
{
	return cu__for_each_tag(self, emit_wrapper, NULL, filter);
}

static void cus__emit_wrapper(struct cus *self)
{
	cus__for_each_cu(self, cu__emit_wrapper, NULL, NULL);
}

static const struct argp_option options[] = {
	{
		.key  = 'p',
		.name = "prefix",
		.arg  = "PREFIX",
		.doc  = "function prefix",
	},
	{
		.name = NULL,
	}
};

static error_t options_parser(int key, char *arg, struct argp_state *state)
{
	switch (key) {
	case ARGP_KEY_INIT:
		state->child_inputs[0] = state->input;
		break;
	case 'p':
		prefix = arg;
		prefix_len = strlen(prefix);
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static const char args_doc[] = "[FILE]";

static struct argp argp = {
	.options  = options,
	.parser	  = options_parser,
	.args_doc = args_doc,
};

int main(int argc, char *argv[])
{
	int err;
	struct cus *cus = cus__new(NULL, NULL);

	if (cus == NULL) {
		fprintf(stderr, "%s: insufficient memory\n", argv[0]);
		return EXIT_FAILURE;
	}

	err = cus__loadfl(cus, &argp, argc, argv);
	if (err != 0)
		return EXIT_FAILURE;

	cus__emit_wrapper(cus);
	return EXIT_SUCCESS;
}
