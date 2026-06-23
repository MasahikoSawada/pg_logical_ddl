/*-------------------------------------------------------------------------
 *
 * ddl_apply.c
 *		Subscriber side of pg_logical_ddl.
 *
 * Copyright (c) 2012-2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pg_logical_ddl.h"

#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_subscription_rel.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "replication/logicalproto.h"
#include "replication/worker_internal.h"
#include "tcop/cmdtag.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/jsonb.h"
#include "utils/snapmgr.h"

static LogicalRepMessageHandle_hook_type prev_message_hook = NULL;

static void pgld_message_handler(LogicalRepMessageData *msg);


void
pgld_apply_init(void)
{
	prev_message_hook = LogicalRepMessageHandle_hook;
	LogicalRepMessageHandle_hook = pgld_message_handler;
}

/* Extract a required top-level string field from the payload. */
static char *
pgld_jb_field(Jsonb *jb, const char *key)
{
	JsonbValue *v = getKeyJsonValueFromContainer(&jb->root, key,
												 strlen(key), NULL);

	if (v == NULL || v->type != jbvString)
		ereport(ERROR,
				(errmsg("pg_logical_ddl: missing or non-string field \"%s\" in DDL message",
						key)));

	return pnstrdup(v->val.string.val, v->val.string.len);
}

/*
 * If the applied statement is a plain CREATE TABLE, return the OID of the new
 * table, else InvalidOid.  Must be called with the publisher's search_path
 * still active, since that is how the relation name is resolved.
 *
 * XXX: it should retrieve the OID of just-created table without resolving
 * its OID again.
 */
static Oid
pgld_created_table_relid(const char *ddl)
{
	List	   *parsetree_list;
	RawStmt    *raw;

	parsetree_list = pg_parse_query(ddl);
	if (list_length(parsetree_list) != 1)
		return InvalidOid;

	raw = linitial_node(RawStmt, parsetree_list);
	if (!IsA(raw->stmt, CreateStmt))
		return InvalidOid;

	return RangeVarGetRelid(((CreateStmt *) raw->stmt)->relation,
							NoLock, true);
}

static void
pgld_message_handler(LogicalRepMessageData *msg)
{
	Jsonb	   *jb;
	char	   *cmd;
	char	   *ddl;
	char	   *user;
	char	   *search_path;
	Oid			roleid;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	Oid			created_relid = InvalidOid;

	/* Chain to any previously installed hook. */
	if (prev_message_hook)
		prev_message_hook(msg);

	/* Only handle messages emitted by our publisher side. */
	if (strcmp(msg->prefix, PG_LOGICAL_DDL_PREFIX) != 0)
		return;

	/*
	 * Make sure one is open (and a snapshot is active) before touching the catalogs
	 * or running the DDL.
	 */
	if (!IsTransactionState())
		StartTransactionCommand();

	/*
	 * XXX it should be handled in the core, i.e. calling begin_replication_step()
	 * in apply_handle_message() or expose begin_replication_step().
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/* Parse the payload back into jsonb and pull out the fields. */
	jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
											CStringGetDatum(msg->message)));
	cmd = pgld_jb_field(jb, PGLD_KEY_CMD);
	ddl = pgld_jb_field(jb, PGLD_KEY_DDL);
	user = pgld_jb_field(jb, PGLD_KEY_USER);
	search_path = pgld_jb_field(jb, PGLD_KEY_SEARCH_PATH);

	roleid = get_role_oid(user, false);

	/*
	 * Switch to the publisher's role and search_path, run the DDL, and restore
	 * the previous settings even on error.
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	save_nestlevel = NewGUCNestLevel();

	PG_TRY();
	{
		SetUserIdAndSecContext(roleid,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
		SetConfigOption("search_path", search_path,
						PGC_USERSET, PGC_S_SESSION);

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "pg_logical_ddl: SPI_connect failed");

		SPI_execute(ddl, false, 0);

		SPI_finish();

		/*
		 * Resolve the OID of a newly created table while the publisher's
		 * search_path is still active (that is how the name resolves).
		 */
		if (GetCommandTagEnum(cmd) == CMDTAG_CREATE_TABLE)
			created_relid = pgld_created_table_relid(ddl);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, save_nestlevel);
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();

	/*
	 * If a new table was created, add it to this subscription so that its
	 * future changes are applied without a manual REFRESH PUBLICATION. Mark it
	 * SYNCDONE as of this message's LSN; the apply worker then applies changes
	 * that follow (see should_apply_changes_for_rel()).  Done as the apply
	 * worker's own role, after the publisher role has been restored.
	 */
	if (OidIsValid(created_relid))
	{
		XLogRecPtr	sublsn;

		if (GetSubscriptionRelState(MySubscription->oid, created_relid,
									&sublsn) == SUBREL_STATE_UNKNOWN)
		{
			AddSubscriptionRelState(MySubscription->oid, created_relid,
									SUBREL_STATE_SYNCDONE, msg->lsn, false);
			CommandCounterIncrement();
		}
	}

	PopActiveSnapshot();
}
