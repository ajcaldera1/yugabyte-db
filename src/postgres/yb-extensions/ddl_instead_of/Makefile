# contrib/ddl_instead_of/Makefile

PGFILEDESC = "ddl_instead_of - optional rewrite of utility DDL before execution"

MODULE_big = ddl_instead_of
OBJS = ddl_instead_of.o $(WIN32RES)

EXTENSION = ddl_instead_of
DATA = ddl_instead_of--1.0.sql \
       ddl_instead_of--1.0--1.1.sql \
       ddl_instead_of--1.1.sql \
       ddl_instead_of--1.1--1.2.sql \
       ddl_instead_of--1.2.sql \
       ddl_instead_of--1.2--1.3.sql \
       ddl_instead_of--1.3.sql \
       ddl_instead_of--1.2--1.3.sql \
       ddl_instead_of--1.3.sql

TESTS = $(wildcard sql/*.sql)
REGRESS = $(patsubst sql/%.sql,%,$(TESTS))

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/ddl_instead_of
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
