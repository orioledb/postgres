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

/*
 * Signature for pg_rewind extension library rewind function.
 *
 * pg_rewind calls _PG_rewind() of each library given via --extension option.
 *
 * 'datadir_target' -- data directory of the target (--target-pgdata).
 * 'datadir_source' -- data directory of the source (--source-pgdata), or NULL.
 * 'connstr_source' -- connection string of the running source server
 *					   (--source-server), NULL otherwise.
 * 'startpoint' -- start of the last common checkpoint record.  The target's
 *				   WAL from here on holds everything the target did that
 *				   the source did not.
 * 'divergerec' -- where the target's and the source's timelines forked:
 *				   the switchpoint of the source's timeline that the
 *				   target is not on.  It is at or after 'startpoint'.
 * 'tliIndex' -- index of the last common timeline in the target's
 *				 timeline history, for reading the target's WAL with SimpleXLogRead().
 * 'endpoint' -- end of the target's WAL: its last checkpoint record's end.
 * 'restoreCommand' -- the target's restore_command if --restore-target-wal
 *					   was given, NULL otherwise.
 * 'argv0' -- pg_rewind's argv[0].
 * 'debug' -- true if '--debug' is set.
 */
extern PGDLLEXPORT void _PG_rewind(const char *datadir_target,
								   char *datadir_source, char *connstr_source,
								   XLogRecPtr startpoint,
								   XLogRecPtr divergerec, int tliIndex,
								   XLogRecPtr endpoint,
								   const char *restoreCommand,
								   const char *argv0, bool debug);

#endif							/* PG_REWIND_EXT_H */
