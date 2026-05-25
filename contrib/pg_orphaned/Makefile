MODULE_big = pg_orphaned
OBJS = pg_orphaned.o $(WIN32RES)

EXTENSION = pg_orphaned
DATA = pg_orphaned--1.0.sql
PGFILEDESC = "pg_orphaned"

LDFLAGS_SL += $(filter -lm, $(LIBS))

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
