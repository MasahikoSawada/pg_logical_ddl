/*-------------------------------------------------------------------------
 *
 * pg_logical_ddl.c
 *
 * Copyright (c) 2012-2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "parser/scansup.h"
#include "pg_logical_ddl.h"
#include "tcop/cmdtag.h"
#include "utils/guc.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_logical_ddl",
					.version = PG_VERSION
);

typedef struct PgldTagSet
{
	int			ntags;
	CommandTag	tags[FLEXIBLE_ARRAY_MEMBER];
} PgldTagSet;

void		_PG_init(void);

/* Raw GUC string and the currently active parsed tag set. */
static char *pgld_log_command_tags = NULL;
static PgldTagSet *pgld_tagset = NULL;

static bool check_log_command_tags(char **newval, void **extra, GucSource source);
static void assign_log_command_tags(const char *newval, void *extra);

/*
 * GUC check hook: parse the comma-separated command tags into a validated
 * PgldTagSet handed off through *extra.
 */
static bool
check_log_command_tags(char **newval, void **extra, GucSource source)
{
	char	   *rawstring;
	char	   *tok;
	char	   *saveptr;
	int			maxtags;
	PgldTagSet *set;

	/* Empty string means "replicate nothing". */
	if (*newval == NULL || (*newval)[0] == '\0')
	{
		*extra = NULL;
		return true;
	}

	/* strtok_r scribbles on its input, so work on a copy. */
	rawstring = pstrdup(*newval);

	/* The number of comma-separated elements bounds the number of tags. */
	maxtags = 1;
	for (const char *p = *newval; *p != '\0'; p++)
	{
		if (*p == ',')
			maxtags++;
	}

	set = (PgldTagSet *) guc_malloc(LOG,
									offsetof(PgldTagSet, tags) +
									sizeof(CommandTag) * maxtags);
	if (set == NULL)
	{
		pfree(rawstring);
		return false;
	}
	set->ntags = 0;

	for (tok = strtok_r(rawstring, ",", &saveptr); tok != NULL;
		 tok = strtok_r(NULL, ",", &saveptr))
	{
		char	   *end;
		CommandTag	tag;

		/* Trim leading and trailing whitespace. */
		while (scanner_isspace(*tok))
			tok++;
		end = tok + strlen(tok);
		while (end > tok && scanner_isspace(end[-1]))
			end--;
		*end = '\0';

		/* Skip empty elements (e.g. a trailing comma). */
		if (*tok == '\0')
			continue;

		tag = GetCommandTagEnum(tok);
		if (tag == CMDTAG_UNKNOWN)
		{
			GUC_check_errdetail("unrecognized command tag \"%s\".", tok);
			guc_free(set);
			pfree(rawstring);
			return false;
		}

		set->tags[set->ntags++] = tag;
	}

	pfree(rawstring);

	*extra = set;
	return true;
}

static void
assign_log_command_tags(const char *newval, void *extra)
{
	pgld_tagset = (PgldTagSet *) extra;
}

/*
 * Returns true if DDL with the given command tag should be replicated.
 */
bool
pg_logical_ddl_should_log(CommandTag tag)
{
	if (pgld_tagset == NULL)
		return false;

	for (int i = 0; i < pgld_tagset->ntags; i++)
	{
		if (pgld_tagset->tags[i] == tag)
			return true;
	}

	return false;
}

void
_PG_init(void)
{
	DefineCustomStringVariable("pg_logical_ddl.log_command_tags",
							   "Comma-separated command tags whose DDL is replicated.",
							   NULL,
							   &pgld_log_command_tags,
							   "",
							   PGC_USERSET,
							   0,
							   check_log_command_tags,
							   assign_log_command_tags,
							   NULL);

	MarkGUCPrefixReserved("pg_logical_ddl");

	pgld_publish_init();
	pgld_apply_init();
}
