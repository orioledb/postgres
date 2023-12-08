/*-------------------------------------------------------------------------
 *
 * pg_rewind_ext.h
 *
 *
 * Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_REWIND_EXT_H
#define PG_REWIND_EXT_H

#include "access/xlogreader.h"

/* in parsexlog.c */
/*
 * Read WAL from the datadir/pg_wal, starting from 'startpoint' on timeline
 * index 'tliIndex' in target timeline history, until 'endpoint'.
 * Pass all WAL records to 'page_callback'.
 *
 * 'endpoint' is the end of the last record to read. The record starting at
 * 'endpoint' is the first one that is not read.
 */
extern void SimpleXLogRead(const char *datadir, XLogRecPtr startpoint,
						   int tliIndex, XLogRecPtr endpoint,
						   const char *restoreCommand,
						   void (*page_callback) (XLogReaderState *,
												  void *arg),
						   void *arg);


/* in filemap.c */
/* Add NULL-terminated list of dirs that pg_rewind can skip copying */
extern void extensions_exclude_add(char **exclude_dirs);

/* signature for pg_rewind extension library rewind function */
extern PGDLLEXPORT void _PG_rewind(const char *datadir_target,
								   char *datadir_source, char *connstr_source,
								   XLogRecPtr startpoint, int tliIndex,
								   XLogRecPtr endpoint,
								   const char *restoreCommand,
								   const char *argv0, bool debug);

#endif							/* PG_REWIND_EXT_H */
