/*-------------------------------------------------------------------------
 *
 * pg_logical_ddl.h
 *		Shared declarations for the pg_logical_ddl extension.
 *
 * Copyright (c) 2012-2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LOGICAL_DDL_H
#define PG_LOGICAL_DDL_H

#include "postgres.h"

#include "tcop/cmdtag.h"

/* Logical decoding message prefix shared by both sides. */
#define PG_LOGICAL_DDL_PREFIX		"pg_logical_ddl"

/* JSON keys used in the message payload. */
#define PGLD_KEY_CMD				"cmd"
#define PGLD_KEY_DDL				"ddl"
#define PGLD_KEY_USER				"user"
#define PGLD_KEY_SEARCH_PATH		"search_path"

extern bool pg_logical_ddl_should_log(CommandTag tag);

/* Per-side initialization, called from _PG_init() in pg_logical_ddl.c. */
extern void pgld_publish_init(void);
extern void pgld_apply_init(void);

#endif							/* PG_LOGICAL_DDL_H */
