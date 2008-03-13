/*
 * Copyright (C) 2007 Davi E. M. Arnaut <davi@haxent.com.br>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as published
 * by the Free Software Foundation.
 */

#include <argp.h>
#include <malloc.h>
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dwarves.h"
#include "dutil.h"

static int verbose;

struct extvar {
	struct extvar		*next;
	const char 		*name;
	const struct variable	*var;
	const struct cu 	*cu;
};

struct extfun {
	struct extfun		*next;
	const char		*name;
	const struct function	*fun;
	const struct cu		*cu;
};

static void *tree;

static void oom(const char *msg)
{
	fprintf(stderr, "pglobal: out of memory (%s)\n", msg);
	exit(EXIT_FAILURE);
}

static struct extvar *extvar__new(const struct variable *var,
				  const struct cu *cu)
{
	struct extvar *gvar = malloc(sizeof(*gvar));

	if (gvar != NULL) {
		gvar->next = NULL;
		gvar->var  = var;
		gvar->cu   = cu;
		gvar->name = variable__name(var, cu);
	}

	return gvar;
}

static struct extfun *extfun__new(struct function *fun,
				  const struct cu *cu)
{
	struct extfun *gfun = malloc(sizeof(*gfun));

	if (gfun != NULL) {
		gfun->next = NULL;
		gfun->fun  = fun;
		gfun->cu   = cu;
		gfun->name = function__name(fun, cu);
	}

	return gfun;
}

static int extvar__compare(const void *a, const void *b)
{
	const struct extvar *ga = a, *gb = b;
	return strcmp(ga->name, gb->name);
}

static int extfun__compare(const void *a, const void *b)
{
	const struct extfun *ga = a, *gb = b;
	return strcmp(ga->name, gb->name);
}

static void extvar__add(const struct variable *var, const struct cu *cu)
{
	struct extvar **nodep, *gvar = extvar__new(var, cu);

	if (gvar != NULL) {
		nodep = tsearch(gvar, &tree, extvar__compare);
		if (nodep == NULL)
			oom("tsearch");
		else if (*nodep != gvar)
			if (gvar->var->declaration) {
				gvar->next = (*nodep)->next;
				(*nodep)->next = gvar;
			} else {
				gvar->next = *nodep;
				*nodep = gvar;
			}
	}
}

static void extfun__add(struct function *fun, const struct cu *cu)
{
	struct extfun **nodep, *gfun = extfun__new(fun, cu);

	if (gfun != NULL) {
		nodep = tsearch(gfun, &tree, extfun__compare);
		if (nodep == NULL)
			oom("tsearch");
		else if (*nodep != gfun) {
			gfun->next = (*nodep)->next;
			(*nodep)->next = gfun;
		}
	}
}

static struct tag *extvar__filter(struct tag *tag, struct cu *cu __unused,
				  void *cookie __unused)
{
	const struct variable *var;

	if (tag->tag != DW_TAG_variable)
		return NULL;

	var = tag__variable(tag);

	if (!var->external)
		return NULL;

	return tag;
}

static struct tag *extfun__filter(struct tag *tag, struct cu *cu __unused,
				  void *cookie __unused)
{
	struct function *fun;

	if (tag->tag != DW_TAG_subprogram)
		return NULL;

	fun = tag__function(tag);

	if (!fun->external)
		return NULL;

	return tag;
}

static int extvar_unique_iterator(struct tag *tag, struct cu *cu,
				  void *cookie __unused)
{
	extvar__add(tag__variable(tag), cu);
	return 0;
}

static int extfun_unique_iterator(struct tag *tag, struct cu *cu,
				  void *cookie __unused)
{
	extfun__add(tag__function(tag), cu);
	return 0;
}

static int cu_extvar_iterator(struct cu *cu, void *cookie)
{
	return cu__for_each_tag(cu, extvar_unique_iterator, cookie,
				extvar__filter);
}

static int cu_extfun_iterator(struct cu *cu, void *cookie)
{
	return cu__for_each_tag(cu, extfun_unique_iterator, cookie,
				extfun__filter);
}

static inline const struct extvar *node__variable(const void *nodep)
{
	return *((const struct extvar **)nodep);
}

static inline const struct extfun *node__function(const void *nodep)
{
	return *((const struct extfun **)nodep);
}

static inline struct tag *extvar__tag(const struct extvar *gvar)
{
	return (struct tag *)gvar->var;
}

static inline struct tag *extfun__tag(const struct extfun *gfun)
{
	return (struct tag *)gfun->fun;
}

static void declaration_action__walk(const void *nodep, const VISIT which,
				     const int depth __unused)
{
	uint32_t count = 0;
	struct tag *tag;
	const struct extvar *pos, *gvar = NULL;

	switch(which) {
	case preorder:
		break;
	case postorder:
		gvar = node__variable(nodep);
		break;
	case endorder:
		break;
	case leaf:
		gvar = node__variable(nodep);
		break;
	}

	if (gvar == NULL)
		return;

	tag = extvar__tag(gvar);

	tag__fprintf(tag, gvar->cu, NULL, stdout);

	for (pos = gvar->next; pos; pos = pos->next)
		count++;

	printf("; /* %u */\n\n", count);
}

static void function_action__walk(const void *nodep, const VISIT which,
				  const int depth __unused)
{
	struct tag *tag;
	const struct extfun *gfun = NULL;

	switch(which) {
	case preorder:
		break;
	case postorder:
		gfun = node__function(nodep);
		break;
	case endorder:
		break;
	case leaf:
		gfun = node__function(nodep);
		break;
	}

	if (gfun == NULL)
		return;

	tag = extfun__tag(gfun);

	tag__fprintf(tag, gfun->cu, NULL, stdout);

	fputs("\n\n", stdout);
}

static void free_node(void *nodep)
{
	void **node = nodep;
	free(*node);
}

static const struct argp_option pglobal__options[] = {
	{
		.key  = 'v',
		.name = "variables",
		.doc  = "show global variables",
	},
	{
		.key  = 'f',
		.name = "functions",
		.doc  = "show global functions",
	},
	{
		.key  = 'V',
		.name = "verbose",
		.doc  = "be verbose",
	},
	{
		.name = NULL,
	}
};

static int walk_var, walk_fun;

static error_t pglobal__options_parser(int key, char *arg __unused,
				      struct argp_state *state)
{
	switch (key) {
	case ARGP_KEY_INIT: state->child_inputs[0] = state->input; break;
	case 'v': walk_var = 1;		break;
	case 'f': walk_fun = 1;		break;
	case 'V': verbose = 1;		break;
	default:  return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static const char pglobal__args_doc[] = "FILE";

static struct argp pglobal__argp = {
	.options  = pglobal__options,
	.parser	  = pglobal__options_parser,
	.args_doc = pglobal__args_doc,
};

int main(int argc, char *argv[])
{
	int err;
	struct cus *cus = cus__new(NULL, NULL);

	if (cus == NULL) {
		fputs("pglobal: insufficient memory\n", stderr);
		return EXIT_FAILURE;
	}

	err = cus__loadfl(cus, &pglobal__argp, argc, argv);
	if (err != 0)
		return EXIT_FAILURE;

	dwarves__init(0);

	if (walk_var) {
		cus__for_each_cu(cus, cu_extvar_iterator, NULL, NULL);
		twalk(tree, declaration_action__walk);
	} else if (walk_fun) {
		cus__for_each_cu(cus, cu_extfun_iterator, NULL, NULL);
		twalk(tree, function_action__walk);
	}

	tdestroy(tree, free_node);

	return EXIT_SUCCESS;
}
