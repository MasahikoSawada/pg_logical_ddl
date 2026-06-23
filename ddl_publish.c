/*-------------------------------------------------------------------------
 *
 * ddl_publish.c
 *		Publisher side of pg_logical_ddl.
 *
 * Installs a ProcessUtility hook that captures executed DDL.  After a
 * top-level utility command completes, its command tag is matched against
 * pg_logical_ddl.log_command_tags; if it is listed, the command is described
 * as a JSON object and written to WAL as a transactional logical decoding
 * message with prefix "pg_logical_ddl".  The message carries everything the
 * subscriber needs to faithfully re-execute the statement: the command tag,
 * the individual statement text, and the role and search_path it ran under.
 *
 * For example, "CREATE TABLE test (a int)" run by role "postgres" yields:
 *
 *   {
 *     "cmd": "CREATE TABLE",
 *     "ddl": "create table test (a int)",
 *     "user": "postgres",
 *     "search_path": "\"$user\", public"
 *   }
 *
 * Copyright (c) 2012-2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pg_logical_ddl.h"

#include "miscadmin.h"
#include "nodes/queryjumble.h"
#include "replication/message.h"
#include "tcop/cmdtag.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/jsonb.h"

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

static void pgld_ProcessUtility(PlannedStmt *pstmt,
								const char *queryString,
								bool readOnlyTree,
								ProcessUtilityContext context,
								ParamListInfo params,
								QueryEnvironment *queryEnv,
								DestReceiver *dest,
								QueryCompletion *qc);


void
pgld_publish_init(void)
{
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = pgld_ProcessUtility;
}

/*
 * Return a palloc'd copy of the individual statement's source text.
 *
 * queryString holds the entire submitted query, which may contain several
 * statements; pstmt->stmt_location and stmt_len delimit the one statement this
 * ProcessUtility call is for.  CleanQuerytext() confines the string to that
 * statement and trims surrounding whitespace, exactly as pg_stat_statements
 * does.
 */
static char *
pgld_statement_text(PlannedStmt *pstmt, const char *queryString)
{
	int			loc = pstmt->stmt_location;
	int			len = pstmt->stmt_len;

	queryString = CleanQuerytext(queryString, &loc, &len);

	return pnstrdup(queryString, len);
}

/* Push a string key/value pair into the object being built. */
static void
pgld_push_str(JsonbInState *state, const char *key, const char *val)
{
	JsonbValue	jk;
	JsonbValue	jv;

	jk.type = jbvString;
	jk.val.string.len = strlen(key);
	jk.val.string.val = (char *) key;
	pushJsonbValue(state, WJB_KEY, &jk);

	jv.type = jbvString;
	jv.val.string.len = strlen(val);
	jv.val.string.val = (char *) val;
	pushJsonbValue(state, WJB_VALUE, &jv);
}

static void
pgld_ProcessUtility(PlannedStmt *pstmt,
					const char *queryString,
					bool readOnlyTree,
					ProcessUtilityContext context,
					ParamListInfo params,
					QueryEnvironment *queryEnv,
					DestReceiver *dest,
					QueryCompletion *qc)
{
	CommandTag	tag;
	JsonbInState state;
	Jsonb	   *jb;
	char	   *payload;
	char	   *ddl;

	/* Run the command first; only replicate what actually succeeded. */
	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	/* Only consider top-level commands to avoid double-logging. */
	if (context != PROCESS_UTILITY_TOPLEVEL)
		return;

	/*
	 * Derive the command tag from the parse tree.  The QueryCompletion's tag
	 * is only filled in for commands that report a row count, so it is unusable
	 * for plain DDL.
	 */
	tag = CreateCommandTag(pstmt->utilityStmt);

	if (!pg_logical_ddl_should_log(tag))
		return;

	/*
	 * Extract just this statement's text from the (possibly multi-statement)
	 * query string.
	 */
	ddl = pgld_statement_text(pstmt, queryString);

	/* Build the JSON payload describing the command. */
	memset(&state, 0, sizeof(state));
	pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

	/* command tag */
	pgld_push_str(&state, PGLD_KEY_CMD, GetCommandTagName(tag));

	/* ddl */
	pgld_push_str(&state, PGLD_KEY_DDL, ddl);

	/* user */
	pgld_push_str(&state, PGLD_KEY_USER,
				  GetUserNameFromId(GetUserId(), false));

	/* search_path value */
	pgld_push_str(&state, PGLD_KEY_SEARCH_PATH,
				  GetConfigOption("search_path", false, false));

	pushJsonbValue(&state, WJB_END_OBJECT, NULL);

	jb = JsonbValueToJsonb(state.result);
	payload = JsonbToCString(NULL, &jb->root, VARSIZE(jb));

	/* Emit as a transactional logical decoding message. */
	LogLogicalMessage(PG_LOGICAL_DDL_PREFIX, payload, strlen(payload),
					  true, false);
}
