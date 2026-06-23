# pg_logical_ddl

Basic **DDL replication** over PostgreSQL logical replication.

On the publisher, a `ProcessUtility` hook captures executed DDL and writes it to
WAL as a logical decoding message. On the subscriber, the apply worker receives
that message and re-executes the DDL as the original role and `search_path`.

> ⚠️ **Experimental / proof-of-concept.** This extension is for exploration and
> discussion, not production use.

## Requirements

This extension depends on an **in-progress PostgreSQL core patch** that adds a
subscriber-side hook for handling logical decoding messages in the apply worker
(`LogicalRepMessageHandle_hook`), together with the `message = true`
subscription option that delivers those messages to apply workers. The patch is
under discussion on pgsql-hackers here:

  https://www.postgresql.org/message-id/CAD21AoCTNGiddikkUcDKj5QLnsg-51bpr-o6L-GTHWZL4ZFYtQ%40mail.gmail.com

It does **not** build or run against any released PostgreSQL. You need a server
built from the branch/patch that provides those features.

## Build

```sh
make USE_PGXS=1 PG_CONFIG=/path/to/patched/pg_config
make install USE_PGXS=1 PG_CONFIG=/path/to/patched/pg_config
```

## Configuration

Load the library via `shared_preload_libraries` on **both** the publisher and
the subscriber (the hooks must be present in regular backends and in apply
workers).

```ini
# postgresql.conf (publisher and subscriber)
shared_preload_libraries = 'pg_logical_ddl'
```

On the **publisher**, choose which DDL to replicate with a comma-separated list
of command tags (case-insensitive, no quoting needed):

```ini
pg_logical_ddl.log_command_tags = 'CREATE TABLE, ALTER TABLE, DROP TABLE'
```

An empty value (the default) disables replication. The GUC is `USERSET`, so it
can also be set per session or per role.

## Usage

```sql
-- publisher
CREATE PUBLICATION pub FOR ALL TABLES;

-- subscriber: the message option delivers logical messages to the apply worker
CREATE SUBSCRIPTION sub
    CONNECTION 'host=... dbname=...'
    PUBLICATION pub
    WITH (message = true);
```

Now DDL run on the publisher is replicated:

```sql
-- publisher
CREATE TABLE t (a int);
ALTER TABLE t ADD COLUMN b text;
INSERT INTO t VALUES (1, 'x');   -- replicated too: see below
```

The message payload is a JSON object, for example:

```json
{
  "cmd": "CREATE TABLE",
  "ddl": "create table t (a int)",
  "user": "postgres",
  "search_path": "\"$user\", public"
}
```

When a `CREATE TABLE` is applied, the new table is registered in the
subscription (`pg_subscription_rel`) as synced, so its subsequent DML is
replicated without a manual `ALTER SUBSCRIPTION ... REFRESH PUBLICATION`.

## Limitations

- Experimental; minimal error handling and no upgrade path.
- **No loop prevention** — intended for one-directional replication only. Leave
  `log_command_tags` unset on the subscriber so applied DDL is not re-logged.
- Statement-level replication: the DDL text is re-executed verbatim, relying on
  the replicated `search_path` to resolve names. Non-deterministic or
  environment-dependent DDL may not reproduce identically.
- Only plain `CREATE TABLE` auto-registers the new relation into the
  subscription; DML on objects created by other means is not auto-handled.

## License

PostgreSQL License.
