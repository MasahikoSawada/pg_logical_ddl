# pg_logical_ddl/Makefile

MODULE_big = pg_logical_ddl
OBJS = \
	$(WIN32RES) \
	pg_logical_ddl.o \
	ddl_publish.o \
	ddl_apply.o

PGFILEDESC = "pg_logical_ddl - logical DDL replication"

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
