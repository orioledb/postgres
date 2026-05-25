#!/usr/bin/env python3
# coding: utf-8

import os
import signal
import unittest

from .base_test import BaseTest


class OrphanedTest(BaseTest):

	def test_orphaned_after_killed_backend(self):
		"""
		Kill a backend with an open transaction that created an OrioleDB table.
		The catalog entry is rolled back, but files remain under orioledb_data.
		"""
		node = self.node
		node.start()
		node.safe_psql('postgres', 'CREATE EXTENSION IF NOT EXISTS orioledb;')

		con = node.connect()
		con.execute('BEGIN;')
		con.execute("""
			CREATE TABLE o_orphan_test (
				id int NOT NULL,
				PRIMARY KEY (id)
			) USING orioledb;
		""")
		con.execute("INSERT INTO o_orphan_test VALUES (1);")

		datoid = con.execute(
		    "SELECT oid FROM pg_database WHERE datname = 'postgres';")[0][0]
		relfilenodes = {
		    row[0]
		    for row in con.execute("""
				SELECT relfilenode FROM pg_class
				WHERE relname IN ('o_orphan_test', 'o_orphan_test_pkey');
			""")
		}
		self.assertGreaterEqual(len(relfilenodes), 2)

		orioledb_dir = os.path.join(node.data_dir, 'orioledb_data', str(datoid))
		for relnode in relfilenodes:
			path = os.path.join(orioledb_dir, str(relnode))
			self.assertTrue(os.path.exists(path) or any(
			    f.startswith(str(relnode)) for f in os.listdir(orioledb_dir)),
			                f"expected data file for relnode {relnode}")

		dml_pid = con.execute('SELECT pg_backend_pid();')[0][0]
		os.kill(dml_pid, signal.SIGKILL)
		con.close()

		node.poll_query_until(
		    "SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = %d" %
		    dml_pid)

		self.assertEqual(
		    node.execute("""
				SELECT count(*) FROM pg_class
				WHERE relname IN ('o_orphan_test', 'o_orphan_test_pkey');
			""")[0][0], 0)

		orphaned = node.execute("""
			SELECT relfilenode, reloid
			FROM orioledb_list_orphaned('0 seconds')
			ORDER BY relfilenode;
		""")
		found_relnodes = {row[0] for row in orphaned}
		self.assertTrue(relfilenodes.issubset(found_relnodes),
		                f"expected {relfilenodes}, got {found_relnodes}")
		for row in orphaned:
			self.assertEqual(row[1], 0)

		moved = node.execute(
		    "SELECT orioledb_move_orphaned('0 seconds');")[0][0]
		self.assertGreaterEqual(moved, len(relfilenodes))

		remaining = node.execute("""
			SELECT count(*) FROM orioledb_list_orphaned('0 seconds');
		""")[0][0]
		self.assertEqual(remaining, 0)

		moved_list = node.execute("""
			SELECT relfilenode FROM orioledb_list_orphaned_moved()
			ORDER BY relfilenode;
		""")
		self.assertTrue(
		    relfilenodes.issubset({row[0]
		                           for row in moved_list}))

		backup_dir = os.path.join(node.data_dir, 'orioledb_orphaned_backup',
		                          str(datoid))
		self.assertTrue(os.path.isdir(backup_dir))

		pg_backup_dir = os.path.join(node.data_dir, 'orphaned_backup')
		self.assertFalse(os.path.exists(pg_backup_dir))

		restored = node.execute(
		    'SELECT orioledb_move_back_orphaned();')[0][0]
		self.assertGreaterEqual(restored, len(relfilenodes))

		node.execute('SELECT orioledb_remove_moved_orphaned();')
		self.assertFalse(os.path.exists(backup_dir))

		node.stop()


if __name__ == '__main__':
	unittest.main()
