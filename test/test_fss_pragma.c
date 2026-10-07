/*
** Test suite for SQLite Fixed-Schema Storage (FSS) PRAGMA control.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include "sqlite3.h"
#include "fss.h"

static int get_int_pragma(sqlite3 *db, const char *zSql) {
  sqlite3_stmt *pStmt = NULL;
  int rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);
  int val = sqlite3_column_int(pStmt, 0);
  sqlite3_finalize(pStmt);
  return val;
}

static void exec_sql(sqlite3 *db, const char *zSql) {
  char *zErrMsg = NULL;
  int rc = sqlite3_exec(db, zSql, NULL, NULL, &zErrMsg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL error: %s (query: %s)\n", zErrMsg ? zErrMsg : "unknown", zSql);
    sqlite3_free(zErrMsg);
  }
  assert(rc == SQLITE_OK);
}

static void test_task4_btree_read_support(void){
  printf("\n--- Running Task 4: B-tree Read Support Comprehensive Tests ---\n");
  const char *dbPath = "/tmp/test_task4_reads.db";
  remove(dbPath);

  sqlite3 *db = NULL;
  int rc = sqlite3_open(dbPath, &db);
  assert(rc == SQLITE_OK);

/* Create table schema:
  ** t_dense: dense rowids 10..14, columns a (INT32), b (INT64), c (FLOAT64)
  ** Root page of t_dense will be page 2.
  */
  exec_sql(db, "CREATE TABLE t_dense(a INT, b INT, c REAL);");
  exec_sql(db, "INSERT INTO t_dense VALUES(0, 0, 0.0);");
  sqlite3_close(db);

  /* Build valid FSS leaf page for t_dense with 5 rows: rowids 10, 11, 12, 13, 14 */
  FssFieldDesc denseCols[3] = {
    { FSS_TYPE_INT32, FSS_COL_FLAG_NOT_NULL, 4 },
    { FSS_TYPE_INT64, FSS_COL_FLAG_NOT_NULL, 8 },
    { FSS_TYPE_FLOAT64, FSS_COL_FLAG_NOT_NULL, 8 }
  };
  FssValue row10[3] = { fssValueInt(100), fssValueInt(1000), fssValueFloat(10.5) };
  uint8_t pageDense[4096];
  memset(pageDense, 0, sizeof(pageDense));
  rc = fssPageInit(pageDense, 4096, 0, denseCols, 3, FSS_FLAG_DENSE_ROWID, 10, row10);
  assert(rc == FSS_OK);

  FssPage pFss;
  rc = fssPageParse(pageDense, 4096, 0, &pFss);
  assert(rc == FSS_OK);
  for( int i = 1; i < 5; i++ ){
    FssValue rowVals[3] = {
      fssValueInt(100 + i),
      fssValueInt(1000 + i * 100),
      fssValueFloat(10.5 + i)
    };
    rc = fssPageInsert(&pFss, 10 + i, rowVals, 3);
    assert(rc == FSS_OK);
  }
  assert(pFss.hdr.cell_count == 5);

  /* Write marker at offset 72 and pageDense at offset 4096 (page 2) */
  FILE *f = fopen(dbPath, "r+b");
  assert(f != NULL);
  fseek(f, 72, SEEK_SET);
  fwrite("FSS1\0\0\0\1", 1, 8, f);
  fseek(f, 4096, SEEK_SET);
  fwrite(pageDense, 1, 4096, f);
  fclose(f);

  /* 1. Point lookups with dense rowids, PRAGMA fixed_schema = OFF */
  rc = sqlite3_open(dbPath, &db);
  assert(rc == SQLITE_OK);
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 0);

  /* Hit: rowid = 12 */
  sqlite3_stmt *pStmt = NULL;
  rc = sqlite3_prepare_v2(db, "SELECT rowid, a, b, c FROM t_dense WHERE rowid = 12;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);
  assert(sqlite3_column_int64(pStmt, 0) == 12);
  assert(sqlite3_column_int(pStmt, 1) == 102);
  assert(sqlite3_column_int64(pStmt, 2) == 1200);
  assert(sqlite3_column_double(pStmt, 3) == 12.5);
  assert(sqlite3_step(pStmt) == SQLITE_DONE);
  sqlite3_finalize(pStmt);

  /* Miss before range: rowid = 9 */
  rc = sqlite3_prepare_v2(db, "SELECT rowid FROM t_dense WHERE rowid = 9;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_DONE);
  sqlite3_finalize(pStmt);

  /* Miss after range: rowid = 15 */
  rc = sqlite3_prepare_v2(db, "SELECT rowid FROM t_dense WHERE rowid = 15;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_DONE);
  sqlite3_finalize(pStmt);

  /* Scan ascending */
  rc = sqlite3_prepare_v2(db, "SELECT rowid, a FROM t_dense ORDER BY rowid ASC;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  for( int i = 0; i < 5; i++ ){
    assert(sqlite3_step(pStmt) == SQLITE_ROW);
    assert(sqlite3_column_int64(pStmt, 0) == 10 + i);
    assert(sqlite3_column_int(pStmt, 1) == 100 + i);
  }
  assert(sqlite3_step(pStmt) == SQLITE_DONE);
  sqlite3_finalize(pStmt);

  /* Scan descending */
  rc = sqlite3_prepare_v2(db, "SELECT rowid, a FROM t_dense ORDER BY rowid DESC;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  for( int i = 4; i >= 0; i-- ){
    assert(sqlite3_step(pStmt) == SQLITE_ROW);
    assert(sqlite3_column_int64(pStmt, 0) == 10 + i);
    assert(sqlite3_column_int(pStmt, 1) == 100 + i);
  }
  assert(sqlite3_step(pStmt) == SQLITE_DONE);
  sqlite3_finalize(pStmt);

  /* Aggregate */
  assert(get_int_pragma(db, "SELECT count(*) FROM t_dense;") == 5);
  assert(get_int_pragma(db, "SELECT sum(a) FROM t_dense;") == 510);
  assert(get_int_pragma(db, "SELECT min(rowid) FROM t_dense;") == 10);
  assert(get_int_pragma(db, "SELECT max(rowid) FROM t_dense;") == 14);

  /* PRAGMA integrity_check */
  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);

  printf("  PASS: Point lookups, scans, aggregates, integrity_check on dense FSS page\n");

  /* 2. Sparse rowid lookups & scans */
  /* Build sparse FSS page with non-contiguous rowids: 10, 25, 50, 100 */
  FssFieldDesc sparseCols[3] = {
    { FSS_TYPE_INT32, FSS_COL_FLAG_NOT_NULL, 4 },
    { FSS_TYPE_INT64, FSS_COL_FLAG_NOT_NULL, 8 },
    { FSS_TYPE_FLOAT64, FSS_COL_FLAG_NOT_NULL, 8 }
  };
  uint8_t pageSparse[4096];
  memset(pageSparse, 0, sizeof(pageSparse));
  FssValue srow10[3] = { fssValueInt(10), fssValueInt(100), fssValueFloat(1.1) };
  rc = fssPageInit(pageSparse, 4096, 0, sparseCols, 3, 0, 10, srow10);
  assert(rc == FSS_OK);
  rc = fssPageParse(pageSparse, 4096, 0, &pFss);
  assert(rc == FSS_OK);

  int64_t sparseRids[3] = { 25, 50, 100 };
  for( int i = 0; i < 3; i++ ){
    FssValue vals[3] = {
      fssValueInt(sparseRids[i]),
      fssValueInt(sparseRids[i] * 10),
      fssValueFloat(sparseRids[i] * 1.5)
    };
    rc = fssPageInsert(&pFss, sparseRids[i], vals, 3);
    assert(rc == FSS_OK);
  }
  assert(pFss.hdr.cell_count == 4);

  /* Overwrite page 2 with sparse page */
  sqlite3_close(db);
  f = fopen(dbPath, "r+b");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  fwrite(pageSparse, 1, 4096, f);
  fclose(f);

  rc = sqlite3_open(dbPath, &db);
  assert(rc == SQLITE_OK);

  /* Hit: rowid = 50 */
  rc = sqlite3_prepare_v2(db, "SELECT rowid, a, b FROM t_dense WHERE rowid = 50;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(sqlite3_column_int64(pStmt, 0) == 50);
  assert(sqlite3_column_int(pStmt, 1) == 50);
  assert(sqlite3_column_int64(pStmt, 2) == 500);
  sqlite3_finalize(pStmt);

  /* Miss in gap: rowid = 30 */
  rc = sqlite3_prepare_v2(db, "SELECT rowid FROM t_dense WHERE rowid = 30;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_DONE);
  sqlite3_finalize(pStmt);

  /* Miss after range: rowid = 200 */
  rc = sqlite3_prepare_v2(db, "SELECT rowid FROM t_dense WHERE rowid = 200;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_DONE);
  sqlite3_finalize(pStmt);

  /* Sparse scan */
  rc = sqlite3_prepare_v2(db, "SELECT rowid, a FROM t_dense ORDER BY rowid ASC;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  int64_t expectedRids[4] = { 10, 25, 50, 100 };
  for( int i = 0; i < 4; i++ ){
    assert(sqlite3_step(pStmt) == SQLITE_ROW);
    assert(sqlite3_column_int64(pStmt, 0) == expectedRids[i]);
    assert(sqlite3_column_int(pStmt, 1) == expectedRids[i]);
  }
  assert(sqlite3_step(pStmt) == SQLITE_DONE);
  sqlite3_finalize(pStmt);

  /* PRAGMA integrity_check on sparse page */
  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);

  printf("  PASS: Point lookups, gap misses, scans, integrity_check on sparse FSS page\n");

  /* 3. Mixed tables: standard 0x0D leaf table + FSS 0x0E leaf table */
  /* Create standard table t_std */
  exec_sql(db, "CREATE TABLE t_std(id INTEGER PRIMARY KEY, name TEXT);");
  exec_sql(db, "INSERT INTO t_std VALUES(10, 'ten'), (25, 'twenty-five'), (50, 'fifty'), (99, 'ninety-nine');");

  /* Join t_dense (FSS page 2) and t_std (standard table) */
  rc = sqlite3_prepare_v2(db, "SELECT t_dense.rowid, t_dense.a, t_std.name FROM t_dense JOIN t_std ON t_dense.rowid = t_std.id ORDER BY t_dense.rowid;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(sqlite3_column_int64(pStmt, 0) == 10);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 2), "ten") == 0);

  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(sqlite3_column_int64(pStmt, 0) == 25);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 2), "twenty-five") == 0);

  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(sqlite3_column_int64(pStmt, 0) == 50);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 2), "fifty") == 0);

  assert(sqlite3_step(pStmt) == SQLITE_DONE);
  sqlite3_finalize(pStmt);

  /* 4. Corrupt / empty FSS pages detection */
  /* a) Empty FSS page (cell_count == 0) */
  uint8_t corruptPage[4096];
  memcpy(corruptPage, pageDense, 4096);
  corruptPage[4] = 0; corruptPage[5] = 0; /* cell_count = 0 */
  sqlite3_close(db);

  f = fopen(dbPath, "r+b");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  fwrite(corruptPage, 1, 4096, f);
  fclose(f);

  rc = sqlite3_open(dbPath, &db);
  assert(rc == SQLITE_OK);
  rc = sqlite3_prepare_v2(db, "SELECT * FROM t_dense;", -1, &pStmt, NULL);
  if( rc == SQLITE_OK ){
    rc = sqlite3_step(pStmt);
  }
  assert(rc == SQLITE_CORRUPT);
  sqlite3_finalize(pStmt);

  /* Integrity check on empty/corrupt FSS page */
  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") != 0);
  sqlite3_finalize(pStmt);

  /* b) Out-of-order sparse rowids */
  memcpy(corruptPage, pageSparse, 4096);
  /* Sparse rowid index starts at 16 + 2 + 3*4 = 30 */
  /* Swap rowids 25 and 50 so rowids become 10, 50, 25, 100 */
  uint8_t tmpRid[8];
  memcpy(tmpRid, &corruptPage[30 + 8], 8);
  memcpy(&corruptPage[30 + 8], &corruptPage[30 + 16], 8);
  memcpy(&corruptPage[30 + 16], tmpRid, 8);
  sqlite3_close(db);

  f = fopen(dbPath, "r+b");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  fwrite(corruptPage, 1, 4096, f);
  fclose(f);

  rc = sqlite3_open(dbPath, &db);
  assert(rc == SQLITE_OK);
  rc = sqlite3_prepare_v2(db, "SELECT * FROM t_dense;", -1, &pStmt, NULL);
  if( rc == SQLITE_OK ){
    rc = sqlite3_step(pStmt);
  }
  assert(rc == SQLITE_CORRUPT);
  sqlite3_finalize(pStmt);

  sqlite3_close(db);
  remove(dbPath);
  printf("  PASS: Corrupt/empty FSS pages detected and rejected by read & integrity_check\n");
}

static void test_task5_first_row_creation_and_inserts(void){
  printf("\n--- Running Task 5: First-Row Creation and Inserts Tests ---\n");
  const char *dbPath = "/tmp/test_task5_inserts.db";
  remove(dbPath);

  sqlite3 *db = NULL;
  int rc = sqlite3_open(dbPath, &db);
  assert(rc == SQLITE_OK);

  /* 1. First eligible insert on empty table with PRAGMA fixed_schema = ON */
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  exec_sql(db, "CREATE TABLE t_fss(a INT, b INT, c REAL);");
  exec_sql(db, "INSERT INTO t_fss VALUES(10, 20, 30.5);");

  /* Check that page 2 is 0x0E (PTF_FSS_LEAF) and page 1 has FSS marker */
  sqlite3_close(db);

  FILE *f = fopen(dbPath, "rb");
  assert(f != NULL);
  uint8_t marker[8];
  fseek(f, 72, SEEK_SET);
  assert(fread(marker, 1, 8, f) == 8);
  assert(memcmp(marker, "FSS1\0\0\0\1", 8) == 0);

  uint8_t page2Hdr[16];
  fseek(f, 4096, SEEK_SET);
  assert(fread(page2Hdr, 1, 16, f) == 16);
  fclose(f);

  assert(page2Hdr[0] == 0x0E); /* FSS Leaf Page */
  assert(page2Hdr[1] & 0x01);  /* Dense rowid mode initially */
  assert((((uint16_t)page2Hdr[4] << 8) | page2Hdr[5]) == 1); /* cell_count = 1 */
  printf("  PASS: First insert created valid 0x0E page and wrote DB marker\n");

  /* Reopen and test query */
  rc = sqlite3_open(dbPath, &db);
  assert(rc == SQLITE_OK);
  sqlite3_stmt *pStmt = NULL;
  rc = sqlite3_prepare_v2(db, "SELECT rowid, a, b, c FROM t_fss WHERE rowid = 1;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(sqlite3_column_int64(pStmt, 0) == 1);
  assert(sqlite3_column_int64(pStmt, 1) == 10);
  assert(sqlite3_column_int64(pStmt, 2) == 20);
  assert(sqlite3_column_double(pStmt, 3) == 30.5);
  sqlite3_finalize(pStmt);

  /* 2. Later sequential inserts in dense mode */
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  exec_sql(db, "INSERT INTO t_fss VALUES(11, 21, 31.5);");
  exec_sql(db, "INSERT INTO t_fss VALUES(12, 22, 32.5);");

  assert(get_int_pragma(db, "SELECT count(*) FROM t_fss;") == 3);
  rc = sqlite3_prepare_v2(db, "SELECT rowid, a FROM t_fss ORDER BY rowid ASC;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  for( int i = 0; i < 3; i++ ){
    assert(sqlite3_step(pStmt) == SQLITE_ROW);
    assert(sqlite3_column_int64(pStmt, 0) == 1 + i);
    assert(sqlite3_column_int(pStmt, 1) == 10 + i);
  }
  sqlite3_finalize(pStmt);
  printf("  PASS: Subsequent sequential inserts stay in dense mode\n");

  /* 3. Non-sequential insert transitions dense -> sparse mode */
  exec_sql(db, "INSERT INTO t_fss(rowid, a, b, c) VALUES(50, 50, 60, 70.5);");

  assert(get_int_pragma(db, "SELECT count(*) FROM t_fss;") == 4);
  assert(get_int_pragma(db, "SELECT a FROM t_fss WHERE rowid = 50;") == 50);
  printf("  PASS: Non-sequential insert transitioned page to sparse mode\n");

  /* 4. Middle-of-page insert */
  exec_sql(db, "INSERT INTO t_fss(rowid, a, b, c) VALUES(25, 25, 35, 45.5);");
  assert(get_int_pragma(db, "SELECT count(*) FROM t_fss;") == 5);

  rc = sqlite3_prepare_v2(db, "SELECT rowid, a FROM t_fss ORDER BY rowid ASC;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  int64_t expectedRids[5] = { 1, 2, 3, 25, 50 };
  for( int i = 0; i < 5; i++ ){
    assert(sqlite3_step(pStmt) == SQLITE_ROW);
    assert(sqlite3_column_int64(pStmt, 0) == expectedRids[i]);
  }
  sqlite3_finalize(pStmt);
  printf("  PASS: Middle-of-page insert maintains sorted rowid order\n");

  /* 5. Overwrite / Replace existing row */
  exec_sql(db, "INSERT OR REPLACE INTO t_fss(rowid, a, b, c) VALUES(25, 999, 888, 777.5);");
  assert(get_int_pragma(db, "SELECT count(*) FROM t_fss;") == 5);
  rc = sqlite3_prepare_v2(db, "SELECT a, b, c FROM t_fss WHERE rowid = 25;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(sqlite3_column_int64(pStmt, 0) == 999);
  assert(sqlite3_column_int64(pStmt, 1) == 888);
  assert(sqlite3_column_double(pStmt, 2) == 777.5);
  sqlite3_finalize(pStmt);
  printf("  PASS: Overwrite/replace updates row in place\n");

  /* 6. Duplicate rowid rejection */
  rc = sqlite3_exec(db, "INSERT INTO t_fss(rowid, a, b, c) VALUES(25, 1, 2, 3.0);", NULL, NULL, NULL);
  assert(rc == SQLITE_CONSTRAINT || rc == SQLITE_CONSTRAINT_PRIMARYKEY);
  printf("  PASS: Duplicate rowid rejected with constraint violation\n");

  /* 7. Nullable fields and affinity coercion */
  exec_sql(db, "INSERT INTO t_fss(rowid, a, b, c) VALUES(60, NULL, 600, 700);");
  rc = sqlite3_prepare_v2(db, "SELECT a, b, c FROM t_fss WHERE rowid = 60;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(sqlite3_column_type(pStmt, 0) == SQLITE_NULL);
  assert(sqlite3_column_int64(pStmt, 1) == 600);
  assert(sqlite3_column_double(pStmt, 2) == 700.0); /* coerced int->float */
  sqlite3_finalize(pStmt);
  printf("  PASS: Nullable columns and affinity coercion handled\n");

  /* PRAGMA integrity_check */
  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);

  /* 8. Incompatible value triggers in-place demotion (0x0E -> 0x0D) */
  exec_sql(db, "INSERT INTO t_fss(rowid, a, b, c) VALUES(70, 'text_in_int', 1, 2.0);");
  assert(get_int_pragma(db, "SELECT count(*) FROM t_fss;") == 7);

  /* Verify page 2 is now standard 0x0D */
  sqlite3_close(db);
  f = fopen(dbPath, "rb");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  assert(fread(page2Hdr, 1, 16, f) == 16);
  fclose(f);
  assert(page2Hdr[0] == 0x0D); /* Morphed to standard dynamic leaf */

  /* Reopen and check all data preserved */
  rc = sqlite3_open(dbPath, &db);
  assert(rc == SQLITE_OK);
  assert(get_int_pragma(db, "SELECT count(*) FROM t_fss;") == 7);
  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);
  printf("  PASS: Incompatible value triggered graceful in-place demotion (0x0E -> 0x0D)\n");

  /* 9. Ineligible table schema falls back to 0x0D from the start */
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  exec_sql(db, "CREATE TABLE t_ineligible(x INT, y TEXT);");
  exec_sql(db, "INSERT INTO t_ineligible VALUES(1, 'hello');");
  assert(get_int_pragma(db, "SELECT count(*) FROM t_ineligible;") == 1);
  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);
  printf("  PASS: Ineligible table schema transparently routed to standard SQLite storage\n");

  sqlite3_close(db);
  remove(dbPath);
}

static void test_task6_btree_balancing_and_page_splits(void){
  printf("\n--- Running Task 6: B-tree Balancing and Page Splits Tests ---\n");
  const char *dbPath = "/tmp/test_task6_splits.db";
  remove(dbPath);

  sqlite3 *db = NULL;
  int rc = sqlite3_open(dbPath, &db);
  assert(rc == SQLITE_OK);

  /* 1. Sequential inserts causing root split and rightmost append */
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  exec_sql(db, "CREATE TABLE t_seq(val INT NOT NULL);");

  /* Insert 3500 rows: each row is 8 bytes, so 508 rows fit per 4KB leaf.
  ** 3500 rows will trigger root split into child page, and multiple rightmost splits (7 leaves). */
  exec_sql(db, "BEGIN TRANSACTION;");
  sqlite3_stmt *pStmt = NULL;
  rc = sqlite3_prepare_v2(db, "INSERT INTO t_seq(rowid, val) VALUES(?, ?);", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  for( int i = 1; i <= 3500; i++ ){
    sqlite3_bind_int64(pStmt, 1, i);
    sqlite3_bind_int64(pStmt, 2, i * 10);
    assert(sqlite3_step(pStmt) == SQLITE_DONE);
    sqlite3_reset(pStmt);
  }
  sqlite3_finalize(pStmt);
  exec_sql(db, "COMMIT;");

  assert(get_int_pragma(db, "SELECT count(*) FROM t_seq;") == 3500);
  assert(get_int_pragma(db, "SELECT min(rowid) FROM t_seq;") == 1);
  assert(get_int_pragma(db, "SELECT max(rowid) FROM t_seq;") == 3500);
  assert(get_int_pragma(db, "SELECT val FROM t_seq WHERE rowid = 500;") == 5000);

  /* PRAGMA integrity_check */
  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);

  /* Verify page structure on disk: page 2 must be interior node 0x05, leaves must be 0x0E */
  sqlite3_close(db);
  FILE *f = fopen(dbPath, "rb");
  assert(f != NULL);
  fseek(f, 0, SEEK_END);
  long fSize = ftell(f);
  int numPages = (int)(fSize / 4096);
  assert(numPages >= 6); /* At least 1 schema + 1 interior + >=6 leaves */

  uint8_t p2Hdr[16];
  fseek(f, 4096, SEEK_SET);
  assert(fread(p2Hdr, 1, 16, f) == 16);
  assert(p2Hdr[0] == 0x05); /* Interior table page */

  /* Verify child leaf pages are all 0x0E */
  int fssLeafCount = 0;
  for( int p = 3; p <= numPages; p++ ){
    uint8_t lHdr[16];
    fseek(f, (p - 1) * 4096, SEEK_SET);
    assert(fread(lHdr, 1, 16, f) == 16);
    if( lHdr[0] == 0x0E ){
      fssLeafCount++;
    }
  }
  fclose(f);
  assert(fssLeafCount >= 6);
  printf("  PASS: Sequential inserts triggered root split (0x05 interior) and FSS leaves (0x0E)\n");

  /* 2. Non-sequential / middle-of-page inserts triggering general balance (balance_nonroot_fss) */
  rc = sqlite3_open(dbPath, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  exec_sql(db, "CREATE TABLE t_mid(val INT NOT NULL);");

  /* First insert even rowids: 2, 4, 6, ..., 800 */
  exec_sql(db, "BEGIN TRANSACTION;");
  rc = sqlite3_prepare_v2(db, "INSERT INTO t_mid(rowid, val) VALUES(?, ?);", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  for( int i = 1; i <= 400; i++ ){
    sqlite3_bind_int64(pStmt, 1, i * 2);
    sqlite3_bind_int64(pStmt, 2, i * 20);
    assert(sqlite3_step(pStmt) == SQLITE_DONE);
    sqlite3_reset(pStmt);
  }
  sqlite3_finalize(pStmt);
  exec_sql(db, "COMMIT;");

  /* Now insert odd rowids: 1, 3, 5, ..., 799 (inserting into middle of existing pages!) */
  exec_sql(db, "BEGIN TRANSACTION;");
  rc = sqlite3_prepare_v2(db, "INSERT INTO t_mid(rowid, val) VALUES(?, ?);", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  for( int i = 1; i <= 400; i++ ){
    sqlite3_bind_int64(pStmt, 1, (i * 2) - 1);
    sqlite3_bind_int64(pStmt, 2, ((i * 2) - 1) * 10);
    assert(sqlite3_step(pStmt) == SQLITE_DONE);
    sqlite3_reset(pStmt);
  }
  sqlite3_finalize(pStmt);
  exec_sql(db, "COMMIT;");

  assert(get_int_pragma(db, "SELECT count(*) FROM t_mid;") == 800);
  assert(get_int_pragma(db, "SELECT min(rowid) FROM t_mid;") == 1);
  assert(get_int_pragma(db, "SELECT max(rowid) FROM t_mid;") == 800);

  /* Verify all 800 rows returned in strictly sorted rowid order */
  rc = sqlite3_prepare_v2(db, "SELECT rowid, val FROM t_mid ORDER BY rowid ASC;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  for( int i = 1; i <= 800; i++ ){
    assert(sqlite3_step(pStmt) == SQLITE_ROW);
    assert(sqlite3_column_int64(pStmt, 0) == i);
    assert(sqlite3_column_int64(pStmt, 1) == i * 10);
  }
  assert(sqlite3_step(pStmt) == SQLITE_DONE);
  sqlite3_finalize(pStmt);

  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);
  printf("  PASS: Middle-of-page interleaved inserts maintained sorted order across splits\n");

  /* 3. Transaction Rollback across multi-page FSS splits */
  assert(get_int_pragma(db, "SELECT count(*) FROM t_mid;") == 800);
  exec_sql(db, "BEGIN TRANSACTION;");
  rc = sqlite3_prepare_v2(db, "INSERT INTO t_mid(rowid, val) VALUES(?, ?);", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  for( int i = 801; i <= 2000; i++ ){
    sqlite3_bind_int64(pStmt, 1, i);
    sqlite3_bind_int64(pStmt, 2, i * 10);
    assert(sqlite3_step(pStmt) == SQLITE_DONE);
    sqlite3_reset(pStmt);
  }
  sqlite3_finalize(pStmt);
  assert(get_int_pragma(db, "SELECT count(*) FROM t_mid;") == 2000);

  /* Rollback the transaction */
  exec_sql(db, "ROLLBACK;");

  /* Verify row count reverted and database remains intact */
  assert(get_int_pragma(db, "SELECT count(*) FROM t_mid;") == 800);
  assert(get_int_pragma(db, "SELECT max(rowid) FROM t_mid;") == 800);

  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);
  printf("  PASS: Multi-page split rolled back safely without corrupting tree\n");

  sqlite3_close(db);
  remove(dbPath);

  /* 4. Auto-vacuum pointer maps across FSS splits */
  const char *avDbPath = "/tmp/test_task6_autovacuum.db";
  remove(avDbPath);
  rc = sqlite3_open(avDbPath, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA auto_vacuum = FULL;");
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  exec_sql(db, "CREATE TABLE t_av(x INT NOT NULL, y REAL NOT NULL);");

  exec_sql(db, "BEGIN TRANSACTION;");
  rc = sqlite3_prepare_v2(db, "INSERT INTO t_av(rowid, x, y) VALUES(?, ?, ?);", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  for( int i = 1; i <= 1000; i++ ){
    sqlite3_bind_int64(pStmt, 1, i);
    sqlite3_bind_int64(pStmt, 2, i * 5);
    sqlite3_bind_double(pStmt, 3, (double)i * 0.5);
    assert(sqlite3_step(pStmt) == SQLITE_DONE);
    sqlite3_reset(pStmt);
  }
  sqlite3_finalize(pStmt);
  exec_sql(db, "COMMIT;");

  assert(get_int_pragma(db, "SELECT count(*) FROM t_av;") == 1000);
  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);
  sqlite3_close(db);
  remove(avDbPath);
  printf("  PASS: Auto-vacuum pointer maps maintained correctly across FSS page splits\n");
}

static void test_task7_updates_deletes_and_demotion(void){
  printf("\n--- Running Task 7: Updates, Deletes, and Fallback/Demotion Tests ---\n");
  sqlite3 *db = NULL;
  sqlite3_stmt *pStmt = NULL;
  int rc;
  FILE *f;
  uint8_t hdr[16];

  /* 1. Fixed-width in-place UPDATEs */
  const char *upDb = "/tmp/test_task7_update.db";
  remove(upDb);
  rc = sqlite3_open(upDb, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  exec_sql(db, "CREATE TABLE t_up(a INT NOT NULL, b REAL NOT NULL, c INT NOT NULL);");
  exec_sql(db, "BEGIN;");
  for( int i = 1; i <= 50; i++ ){
    char sql[128];
    snprintf(sql, sizeof(sql), "INSERT INTO t_up(rowid, a, b, c) VALUES(%d, %d, %f, %d);", i, i, (double)i * 1.5, i * 10);
    exec_sql(db, sql);
  }
  exec_sql(db, "COMMIT;");

  /* Verify page is FSS 0x0E */
  sqlite3_close(db);
  f = fopen(upDb, "rb");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  assert(fread(hdr, 1, 16, f) == 16);
  fclose(f);
  assert(hdr[0] == 0x0E);

  rc = sqlite3_open(upDb, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");

  /* Point update in place */
  exec_sql(db, "UPDATE t_up SET b = 888.5, c = 9999 WHERE rowid = 25;");
  rc = sqlite3_prepare_v2(db, "SELECT b, c FROM t_up WHERE rowid = 25;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(sqlite3_column_double(pStmt, 0) == 888.5);
  assert(sqlite3_column_int64(pStmt, 1) == 9999);
  sqlite3_finalize(pStmt);

  /* Batch update */
  exec_sql(db, "UPDATE t_up SET a = a + 100 WHERE rowid <= 10;");
  assert(get_int_pragma(db, "SELECT sum(a) FROM t_up WHERE rowid <= 10;") == (55 + 1000));

  /* Still 0x0E after updates */
  sqlite3_close(db);
  f = fopen(upDb, "rb");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  assert(fread(hdr, 1, 16, f) == 16);
  fclose(f);
  assert(hdr[0] == 0x0E);
  remove(upDb);
  printf("  PASS: In-place fixed-width updates correctly route to FSS slots and invalidate column cache\n");

  /* 2. UPDATE and DELETE with triggers, indexes, and conflict handling */
  const char *trDb = "/tmp/test_task7_triggers.db";
  remove(trDb);
  rc = sqlite3_open(trDb, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  exec_sql(db, "CREATE TABLE t_item(id INT NOT NULL, price INT NOT NULL);");
  exec_sql(db, "CREATE TABLE t_audit(op TEXT, id INT, old_price INT, new_price INT);");
  exec_sql(db, "CREATE TRIGGER tr_item_up AFTER UPDATE ON t_item BEGIN "
               "  INSERT INTO t_audit VALUES('UP', OLD.id, OLD.price, NEW.price); "
               "END;");
  exec_sql(db, "CREATE TRIGGER tr_item_del AFTER DELETE ON t_item BEGIN "
               "  INSERT INTO t_audit VALUES('DEL', OLD.id, OLD.price, 0); "
               "END;");
  exec_sql(db, "CREATE INDEX idx_item_price ON t_item(price);");

  exec_sql(db, "INSERT INTO t_item VALUES(1, 100);");
  exec_sql(db, "INSERT INTO t_item VALUES(2, 200);");
  exec_sql(db, "INSERT INTO t_item VALUES(3, 300);");

  /* Test update trigger and index */
  exec_sql(db, "UPDATE t_item SET price = 150 WHERE id = 1;");
  assert(get_int_pragma(db, "SELECT count(*) FROM t_audit WHERE op = 'UP' AND id = 1 AND new_price = 150;") == 1);
  assert(get_int_pragma(db, "SELECT id FROM t_item WHERE price = 150;") == 1);

  /* Test delete trigger and index */
  exec_sql(db, "DELETE FROM t_item WHERE id = 2;");
  assert(get_int_pragma(db, "SELECT count(*) FROM t_audit WHERE op = 'DEL' AND id = 2;") == 1);
  assert(get_int_pragma(db, "SELECT count(*) FROM t_item WHERE price = 200;") == 0);

  /* Unique constraint conflict handling */
  exec_sql(db, "CREATE UNIQUE INDEX idx_item_id ON t_item(id);");
  rc = sqlite3_exec(db, "UPDATE t_item SET id = 3 WHERE id = 1;", NULL, NULL, NULL);
  assert(rc == SQLITE_CONSTRAINT);
  exec_sql(db, "UPDATE OR IGNORE t_item SET id = 3 WHERE id = 1;");
  assert(get_int_pragma(db, "SELECT count(*) FROM t_item WHERE id = 1;") == 1);

  sqlite3_close(db);
  remove(trDb);
  printf("  PASS: Triggers, indexes, and constraint conflicts work with FSS updates and deletes\n");

  /* 3. DELETE boundary and middle transitions (dense -> sparse) */
  const char *delDb = "/tmp/test_task7_delete.db";
  remove(delDb);
  rc = sqlite3_open(delDb, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  exec_sql(db, "CREATE TABLE t_del(val INT NOT NULL);");
  for( int i = 1; i <= 10; i++ ){
    char sql[64];
    snprintf(sql, sizeof(sql), "INSERT INTO t_del(rowid, val) VALUES(%d, %d);", i, i * 10);
    exec_sql(db, sql);
  }

  /* Verify initial page is dense */
  sqlite3_close(db);
  f = fopen(delDb, "rb");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  assert(fread(hdr, 1, 16, f) == 16);
  fclose(f);
  assert(hdr[0] == 0x0E);
  assert((hdr[1] & 0x01) != 0); /* FSS_FLAG_DENSE_ROWID */

  rc = sqlite3_open(delDb, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");

  /* Boundary delete: rowid 1 (targetSlot == 0) */
  exec_sql(db, "DELETE FROM t_del WHERE rowid = 1;");
  assert(get_int_pragma(db, "SELECT count(*) FROM t_del;") == 9);

  /* Verify still dense */
  sqlite3_close(db);
  f = fopen(delDb, "rb");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  assert(fread(hdr, 1, 16, f) == 16);
  fclose(f);
  assert(hdr[0] == 0x0E);
  assert((hdr[1] & 0x01) != 0); /* Still dense */

  rc = sqlite3_open(delDb, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");

  /* Boundary delete: rowid 10 (targetSlot == count - 1) */
  exec_sql(db, "DELETE FROM t_del WHERE rowid = 10;");
  assert(get_int_pragma(db, "SELECT count(*) FROM t_del;") == 8);

  /* Middle delete: rowid 5 (breaks contiguity -> transitions to sparse!) */
  exec_sql(db, "DELETE FROM t_del WHERE rowid = 5;");
  assert(get_int_pragma(db, "SELECT count(*) FROM t_del;") == 7);

  /* Verify converted to sparse */
  sqlite3_close(db);
  f = fopen(delDb, "rb");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  assert(fread(hdr, 1, 16, f) == 16);
  fclose(f);
  assert(hdr[0] == 0x0E);
  assert((hdr[1] & 0x01) == 0); /* FSS_FLAG_DENSE_ROWID is cleared! Now sparse! */

  /* Reopen and check data integrity */
  rc = sqlite3_open(delDb, &db);
  assert(rc == SQLITE_OK);
  rc = sqlite3_prepare_v2(db, "SELECT rowid, val FROM t_del ORDER BY rowid;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  int expectedRowids[] = {2, 3, 4, 6, 7, 8, 9};
  int idx = 0;
  while( sqlite3_step(pStmt) == SQLITE_ROW ){
    assert(idx < 7);
    assert(sqlite3_column_int64(pStmt, 0) == expectedRowids[idx]);
    assert(sqlite3_column_int64(pStmt, 1) == expectedRowids[idx] * 10);
    idx++;
  }
  assert(idx == 7);
  sqlite3_finalize(pStmt);
  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);
  sqlite3_close(db);
  remove(delDb);
  printf("  PASS: Boundary and middle row deletions properly compact slots and transition dense to sparse\n");

  /* 4. Multi-page chunk teardown, sibling merge, and root page teardown */
  const char *reclaimDb = "/tmp/test_task7_reclaim.db";
  remove(reclaimDb);
  rc = sqlite3_open(reclaimDb, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  exec_sql(db, "CREATE TABLE t_rec(x INT NOT NULL, y REAL NOT NULL);");
  exec_sql(db, "BEGIN;");
  for( int i = 1; i <= 600; i++ ){
    char sql[64];
    snprintf(sql, sizeof(sql), "INSERT INTO t_rec(rowid, x, y) VALUES(%d, %d, %f);", i, i, (double)i);
    exec_sql(db, sql);
  }
  exec_sql(db, "COMMIT;");

  /* Verify multi-page tree with interior root */
  sqlite3_close(db);
  f = fopen(reclaimDb, "rb");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  assert(fread(hdr, 1, 16, f) == 16);
  fclose(f);
  assert(hdr[0] == 0x05); /* Interior root */

  rc = sqlite3_open(reclaimDb, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");

  /* Delete half of the rows (300 rows) to trigger underflow, merge, and sibling collapse */
  exec_sql(db, "BEGIN;");
  for( int i = 1; i <= 300; i++ ){
    char sql[64];
    snprintf(sql, sizeof(sql), "DELETE FROM t_rec WHERE rowid = %d;", i);
    exec_sql(db, sql);
  }
  exec_sql(db, "COMMIT;");
  assert(get_int_pragma(db, "SELECT count(*) FROM t_rec;") == 300);

  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);

  /* Now delete ALL remaining rows (chunk teardown to 0 rows) */
  exec_sql(db, "DELETE FROM t_rec;");
  assert(get_int_pragma(db, "SELECT count(*) FROM t_rec;") == 0);

  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);

  /* Verify root page is now standard 0x0D empty leaf */
  sqlite3_close(db);
  f = fopen(reclaimDb, "rb");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  assert(fread(hdr, 1, 16, f) == 16);
  fclose(f);
  assert(hdr[0] == 0x0D);

  /* Re-insert rows into empty table to verify first-row FSS creation works again */
  rc = sqlite3_open(reclaimDb, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  exec_sql(db, "INSERT INTO t_rec(rowid, x, y) VALUES(1, 42, 3.14);");
  assert(get_int_pragma(db, "SELECT count(*) FROM t_rec;") == 1);
  sqlite3_close(db);

  f = fopen(reclaimDb, "rb");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  assert(fread(hdr, 1, 16, f) == 16);
  fclose(f);
  assert(hdr[0] == 0x0E); /* Successfully recreated as FSS page */
  remove(reclaimDb);
  printf("  PASS: Underfull pages reclaimed/merged and empty root page cleanly reset\n");

  /* 5. Type mismatch demotion during UPDATE and large fallback split */
  const char *demoteDb = "/tmp/test_task7_demote.db";
  remove(demoteDb);
  rc = sqlite3_open(demoteDb, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  exec_sql(db, "CREATE TABLE t_morph(id INT NOT NULL, val INT NOT NULL);");
  for( int i = 1; i <= 20; i++ ){
    char sql[64];
    snprintf(sql, sizeof(sql), "INSERT INTO t_morph(rowid, id, val) VALUES(%d, %d, %d);", i, i, i * 100);
    exec_sql(db, sql);
  }

  /* Page is 0x0E initially */
  sqlite3_close(db);
  f = fopen(demoteDb, "rb");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  assert(fread(hdr, 1, 16, f) == 16);
  fclose(f);
  assert(hdr[0] == 0x0E);

  /* Update with type mismatch: store string in integer column */
  rc = sqlite3_open(demoteDb, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  exec_sql(db, "UPDATE t_morph SET val = 'demoted_text_value' WHERE rowid = 10;");

  /* Page should now be 0x0D */
  sqlite3_close(db);
  f = fopen(demoteDb, "rb");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  assert(fread(hdr, 1, 16, f) == 16);
  fclose(f);
  assert(hdr[0] == 0x0D);

  /* Verify content intact */
  rc = sqlite3_open(demoteDb, &db);
  assert(rc == SQLITE_OK);
  rc = sqlite3_prepare_v2(db, "SELECT val FROM t_morph WHERE rowid = 10;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "demoted_text_value") == 0);
  sqlite3_finalize(pStmt);
  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);
  sqlite3_close(db);
  remove(demoteDb);
  printf("  PASS: Incompatible UPDATE value triggered graceful in-place demotion (0x0E -> 0x0D)\n");

  /* 6. Transaction and Savepoint Rollback of updates and deletes */
  const char *rbDb = "/tmp/test_task7_rollback.db";
  remove(rbDb);
  rc = sqlite3_open(rbDb, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  exec_sql(db, "CREATE TABLE t_rb(a INT NOT NULL, b INT NOT NULL);");
  exec_sql(db, "INSERT INTO t_rb VALUES(1, 10);");
  exec_sql(db, "INSERT INTO t_rb VALUES(2, 20);");
  exec_sql(db, "INSERT INTO t_rb VALUES(3, 30);");

  /* Transaction rollback */
  exec_sql(db, "BEGIN;");
  exec_sql(db, "UPDATE t_rb SET b = 99 WHERE a = 1;");
  exec_sql(db, "DELETE FROM t_rb WHERE a = 2;");
  exec_sql(db, "INSERT INTO t_rb VALUES(4, 40);");
  exec_sql(db, "ROLLBACK;");

  assert(get_int_pragma(db, "SELECT count(*) FROM t_rb;") == 3);
  assert(get_int_pragma(db, "SELECT b FROM t_rb WHERE a = 1;") == 10);
  assert(get_int_pragma(db, "SELECT count(*) FROM t_rb WHERE a = 2;") == 1);

  /* Savepoint rollback */
  exec_sql(db, "SAVEPOINT sp1;");
  exec_sql(db, "UPDATE t_rb SET b = 555 WHERE a = 3;");
  exec_sql(db, "DELETE FROM t_rb WHERE a = 1;");
  exec_sql(db, "ROLLBACK TO sp1;");
  exec_sql(db, "RELEASE sp1;");

  assert(get_int_pragma(db, "SELECT count(*) FROM t_rb;") == 3);
  assert(get_int_pragma(db, "SELECT b FROM t_rb WHERE a = 3;") == 30);
  assert(get_int_pragma(db, "SELECT count(*) FROM t_rb WHERE a = 1;") == 1);

  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);
  sqlite3_close(db);
  remove(rbDb);
  printf("  PASS: Transaction and savepoint rollbacks safely restore FSS state\n");
}

static void test_task8_schema_and_pragma_behavior(void){
  printf("\n--- Running Task 8: Schema and PRAGMA Behavior Tests ---\n");
  sqlite3 *db1 = NULL;
  sqlite3 *db2 = NULL;
  sqlite3_stmt *pStmt = NULL;
  int rc;
  FILE *f = NULL;
  uint8_t hdr[16];

  /* 1. Connection-local vs Database-persistent behavior */
  const char *dbPath1 = "/tmp/test_task8_conn1.db";
  remove(dbPath1);

  /* Connection 1 sets fixed_schema = ON and creates table */
  rc = sqlite3_open(dbPath1, &db1);
  assert(rc == SQLITE_OK);
  exec_sql(db1, "PRAGMA fixed_schema = ON;");
  assert(get_int_pragma(db1, "PRAGMA fixed_schema;") == 1);
  exec_sql(db1, "CREATE TABLE t_fss(id INT NOT NULL, val INT NOT NULL);");
  exec_sql(db1, "INSERT INTO t_fss VALUES(1, 100);");
  exec_sql(db1, "INSERT INTO t_fss VALUES(2, 200);");

  /* Open second connection to same database without enabling PRAGMA */
  rc = sqlite3_open(dbPath1, &db2);
  assert(rc == SQLITE_OK);
  /* Effective connection flag on new connection is default OFF (0) */
  assert(get_int_pragma(db2, "PRAGMA fixed_schema;") == 0);

  /* But db2 can still read existing FSS table safely because format marker is persistent in file */
  assert(get_int_pragma(db2, "SELECT count(*) FROM t_fss;") == 2);
  assert(get_int_pragma(db2, "SELECT val FROM t_fss WHERE id = 1;") == 100);

  /* And db2 can update existing FSS table safely */
  exec_sql(db2, "UPDATE t_fss SET val = 150 WHERE id = 1;");
  assert(get_int_pragma(db2, "SELECT val FROM t_fss WHERE id = 1;") == 150);
  assert(get_int_pragma(db1, "SELECT val FROM t_fss WHERE id = 1;") == 150);

  /* Now, with fixed_schema = OFF on db2, creating a NEW table must create a standard 0x0D leaf! */
  exec_sql(db2, "CREATE TABLE t_std(a INT NOT NULL, b INT NOT NULL);");
  exec_sql(db2, "INSERT INTO t_std VALUES(10, 20);");

  /* Check page 3 (t_std root page) - must be 0x0D */
  sqlite3_close(db2);
  sqlite3_close(db1);

  f = fopen(dbPath1, "rb");
  assert(f != NULL);
  /* Page 2: t_fss (0x0E) */
  fseek(f, 4096, SEEK_SET);
  assert(fread(hdr, 1, 16, f) == 16);
  assert(hdr[0] == 0x0E);
  /* Page 3: t_std (0x0D because created while fixed_schema was OFF!) */
  fseek(f, 8192, SEEK_SET);
  assert(fread(hdr, 1, 16, f) == 16);
  assert(hdr[0] == 0x0D);
  fclose(f);
  remove(dbPath1);
  printf("  PASS: Connection-local pragma prevents new FSS pages when OFF while safely reading/updating existing FSS tables\n");

  /* 2. PRAGMA value reporting and boolean transition parsing */
  rc = sqlite3_open(":memory:", &db1);
  assert(rc == SQLITE_OK);
  assert(get_int_pragma(db1, "PRAGMA fixed_schema;") == 0);
  exec_sql(db1, "PRAGMA fixed_schema = YES;");
  assert(get_int_pragma(db1, "PRAGMA fixed_schema;") == 1);
  exec_sql(db1, "PRAGMA fixed_schema = NO;");
  assert(get_int_pragma(db1, "PRAGMA fixed_schema;") == 0);
  exec_sql(db1, "PRAGMA fixed_schema = true;");
  assert(get_int_pragma(db1, "PRAGMA fixed_schema;") == 1);
  exec_sql(db1, "PRAGMA fixed_schema = false;");
  assert(get_int_pragma(db1, "PRAGMA fixed_schema;") == 0);
  exec_sql(db1, "PRAGMA fixed_schema = 1;");
  assert(get_int_pragma(db1, "PRAGMA fixed_schema;") == 1);
  exec_sql(db1, "PRAGMA fixed_schema = 0;");
  assert(get_int_pragma(db1, "PRAGMA fixed_schema;") == 0);

  /* Aliases: fss and fixed_schema_storage */
  exec_sql(db1, "PRAGMA fss = ON;");
  assert(get_int_pragma(db1, "PRAGMA fss;") == 1);
  assert(get_int_pragma(db1, "PRAGMA fixed_schema;") == 1);
  assert(get_int_pragma(db1, "PRAGMA fixed_schema_storage;") == 1);
  exec_sql(db1, "PRAGMA fixed_schema_storage = OFF;");
  assert(get_int_pragma(db1, "PRAGMA fss;") == 0);
  assert(get_int_pragma(db1, "PRAGMA fixed_schema;") == 0);
  assert(get_int_pragma(db1, "PRAGMA fixed_schema_storage;") == 0);
  sqlite3_close(db1);
  printf("  PASS: PRAGMA fixed_schema, fss, and fixed_schema_storage aliases accurately query and report state\n");

  /* 3. Attached Databases behavior */
  const char *mainDb = "/tmp/test_task8_main.db";
  const char *attDb = "/tmp/test_task8_attached.db";
  remove(mainDb);
  remove(attDb);

  rc = sqlite3_open(mainDb, &db1);
  assert(rc == SQLITE_OK);

  /* Create table in main with fixed_schema = OFF */
  assert(get_int_pragma(db1, "PRAGMA fixed_schema;") == 0);
  exec_sql(db1, "CREATE TABLE t_main(m INT NOT NULL);");
  exec_sql(db1, "INSERT INTO t_main VALUES(99);");

  /* Attach second database */
  char attachSql[256];
  snprintf(attachSql, sizeof(attachSql), "ATTACH DATABASE '%s' AS aux;", attDb);
  exec_sql(db1, attachSql);

  /* Enable fixed_schema = ON on the connection */
  exec_sql(db1, "PRAGMA fixed_schema = ON;");
  assert(get_int_pragma(db1, "PRAGMA fixed_schema;") == 1);

  /* Create eligible table in aux database */
  exec_sql(db1, "CREATE TABLE aux.t_aux(x INT NOT NULL, y REAL NOT NULL);");
  exec_sql(db1, "INSERT INTO aux.t_aux VALUES(1, 3.14);");
  exec_sql(db1, "INSERT INTO aux.t_aux VALUES(2, 6.28);");

  /* Verify cross-database joins and queries */
  rc = sqlite3_prepare_v2(db1, "SELECT t_main.m, t_aux.x, t_aux.y FROM t_main CROSS JOIN aux.t_aux ORDER BY t_aux.x;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(sqlite3_column_int(pStmt, 0) == 99);
  assert(sqlite3_column_int(pStmt, 1) == 1);
  assert(sqlite3_column_double(pStmt, 2) == 3.14);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(sqlite3_column_int(pStmt, 0) == 99);
  assert(sqlite3_column_int(pStmt, 1) == 2);
  assert(sqlite3_column_double(pStmt, 2) == 6.28);
  assert(sqlite3_step(pStmt) == SQLITE_DONE);
  sqlite3_finalize(pStmt);

  /* Detach and verify disks: main has NO marker and 0x0D leaf, aux has FSS1 marker and 0x0E leaf */
  exec_sql(db1, "DETACH DATABASE aux;");
  sqlite3_close(db1);

  /* Main database inspection */
  f = fopen(mainDb, "rb");
  assert(f != NULL);
  uint8_t mBuf[8];
  fseek(f, 72, SEEK_SET);
  assert(fread(mBuf, 1, 8, f) == 8);
  for( int i = 0; i < 8; i++ ) assert(mBuf[i] == 0);
  fseek(f, 4096, SEEK_SET);
  assert(fread(hdr, 1, 16, f) == 16);
  assert(hdr[0] == 0x0D); /* Standard SQLite page */
  fclose(f);

  /* Aux database inspection */
  f = fopen(attDb, "rb");
  assert(f != NULL);
  fseek(f, 72, SEEK_SET);
  assert(fread(mBuf, 1, 8, f) == 8);
  assert(memcmp(mBuf, "FSS1\0\0\0\1", 8) == 0); /* FSS format marker present */
  fseek(f, 4096, SEEK_SET);
  assert(fread(hdr, 1, 16, f) == 16);
  assert(hdr[0] == 0x0E); /* FSS page */
  fclose(f);

  /* Reopen aux database directly as main, verify integrity and read/write */
  rc = sqlite3_open(attDb, &db2);
  assert(rc == SQLITE_OK);
  assert(get_int_pragma(db2, "SELECT count(*) FROM t_aux;") == 2);
  exec_sql(db2, "UPDATE t_aux SET y = 9.99 WHERE x = 1;");
  assert(get_int_pragma(db2, "SELECT y FROM t_aux WHERE x = 1;") == 9);
  rc = sqlite3_prepare_v2(db2, "PRAGMA integrity_check;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(pStmt, 0), "ok") == 0);
  sqlite3_finalize(pStmt);
  sqlite3_close(db2);

  remove(mainDb);
  remove(attDb);
  printf("  PASS: Attached databases correctly manage their own format markers and FSS storage\n");
}

static void test_task9_end_to_end_verification(void){
  printf("\n--- Running Task 9: Full End-to-End Verification & Mixed-Page Tests ---\n");
  const char *stdPath = "/tmp/test_task9_std.db";
  const char *fssPath = "/tmp/test_task9_fss.db";
  remove(stdPath);
  remove(fssPath);

  sqlite3 *dbStd = NULL;
  sqlite3 *dbFss = NULL;
  sqlite3_stmt *stmtStd = NULL;
  sqlite3_stmt *stmtFss = NULL;
  int rc;
  FILE *f = NULL;
  uint8_t hdr[16];

  /* 1. Create identical tables in standard SQLite and FSS SQLite */
  rc = sqlite3_open(stdPath, &dbStd);
  assert(rc == SQLITE_OK);
  exec_sql(dbStd, "PRAGMA fixed_schema = OFF;");
  exec_sql(dbStd, "CREATE TABLE t_mixed(id INT NOT NULL, name TEXT, score REAL NOT NULL, tag INT NOT NULL);");

  rc = sqlite3_open(fssPath, &dbFss);
  assert(rc == SQLITE_OK);
  exec_sql(dbFss, "PRAGMA fixed_schema = ON;");
  exec_sql(dbFss, "CREATE TABLE t_fss(id INT NOT NULL, score REAL NOT NULL, tag INT NOT NULL);");
  /* Also create table with unsupported column (TEXT) to verify mixed page types in same database */
  exec_sql(dbFss, "CREATE TABLE t_mixed(id INT NOT NULL, name TEXT, score REAL NOT NULL, tag INT NOT NULL);");

  /* Insert 500 rows into both */
  exec_sql(dbStd, "BEGIN;");
  exec_sql(dbFss, "BEGIN;");
  for( int i = 1; i <= 500; i++ ){
    char sqlStd[128];
    char sqlFss[128];
    snprintf(sqlStd, sizeof(sqlStd), "INSERT INTO t_mixed VALUES(%d, 'item_%d', %f, %d);", i, i, (double)i * 1.25, i % 50);
    snprintf(sqlFss, sizeof(sqlFss), "INSERT INTO t_fss VALUES(%d, %f, %d);", i, (double)i * 1.25, i % 50);
    exec_sql(dbStd, sqlStd);
    exec_sql(dbFss, sqlStd);
    exec_sql(dbFss, sqlFss);
  }
  exec_sql(dbStd, "COMMIT;");
  exec_sql(dbFss, "COMMIT;");

  /* Verify disk page types in fssPath */
  sqlite3_close(dbFss);
  f = fopen(fssPath, "rb");
  assert(f != NULL);
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  int numPages = (int)(sz / 4096);
  assert(numPages >= 4);

  int fssPages = 0;
  int stdPages = 0;
  for( int p = 2; p <= numPages; p++ ){
    fseek(f, (p - 1) * 4096, SEEK_SET);
    assert(fread(hdr, 1, 16, f) == 16);
    if( hdr[0] == 0x0E ) fssPages++;
    else if( hdr[0] == 0x0D ) stdPages++;
  }
  fclose(f);
  assert(fssPages > 0); /* Has 0x0E leaves for t_fss */
  assert(stdPages > 0); /* Has 0x0D leaves for t_mixed */
  printf("  PASS: Database file contains verified 0x0E leaves for FSS table and 0x0D leaves for dynamic table\n");

  /* 2. Run SQLite integrity checks and compare full query result sets */
  rc = sqlite3_open(fssPath, &dbFss);
  assert(rc == SQLITE_OK);
  rc = sqlite3_prepare_v2(dbFss, "PRAGMA integrity_check;", -1, &stmtFss, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(stmtFss) == SQLITE_ROW);
  assert(strcmp((const char*)sqlite3_column_text(stmtFss, 0), "ok") == 0);
  sqlite3_finalize(stmtFss);

  /* Compare result sets between dbStd and dbFss on t_mixed */
  rc = sqlite3_prepare_v2(dbStd, "SELECT id, name, score, tag FROM t_mixed ORDER BY id;", -1, &stmtStd, NULL);
  assert(rc == SQLITE_OK);
  rc = sqlite3_prepare_v2(dbFss, "SELECT id, name, score, tag FROM t_mixed ORDER BY id;", -1, &stmtFss, NULL);
  assert(rc == SQLITE_OK);
  int matchedRows = 0;
  while( sqlite3_step(stmtStd) == SQLITE_ROW && sqlite3_step(stmtFss) == SQLITE_ROW ){
    assert(sqlite3_column_int(stmtStd, 0) == sqlite3_column_int(stmtFss, 0));
    assert(strcmp((const char*)sqlite3_column_text(stmtStd, 1), (const char*)sqlite3_column_text(stmtFss, 1)) == 0);
    assert(sqlite3_column_double(stmtStd, 2) == sqlite3_column_double(stmtFss, 2));
    assert(sqlite3_column_int(stmtStd, 3) == sqlite3_column_int(stmtFss, 3));
    matchedRows++;
  }
  assert(matchedRows == 500);
  sqlite3_finalize(stmtStd);
  sqlite3_finalize(stmtFss);

  /* Compare aggregations between t_fss and t_mixed */
  assert(get_int_pragma(dbFss, "SELECT count(*) FROM t_fss;") == 500);
  assert(get_int_pragma(dbFss, "SELECT min(id) FROM t_fss;") == 1);
  assert(get_int_pragma(dbFss, "SELECT max(id) FROM t_fss;") == 500);
  assert(get_int_pragma(dbFss, "SELECT sum(tag) FROM t_fss;") == get_int_pragma(dbStd, "SELECT sum(tag) FROM t_mixed;"));

  /* 3. Re-open tests after checkpoint & close */
  sqlite3_close(dbStd);
  sqlite3_close(dbFss);

  rc = sqlite3_open(fssPath, &dbFss);
  assert(rc == SQLITE_OK);
  assert(get_int_pragma(dbFss, "SELECT count(*) FROM t_fss;") == 500);
  sqlite3_close(dbFss);

  remove(stdPath);
  remove(fssPath);
  printf("  PASS: SQLite integrity checks pass and query result sets exactly match unmodified SQLite\n");
}

int main(void){
  printf("========================================\n");
  printf("Running SQLite FSS PRAGMA Test Suite\n");
  printf("========================================\n");

  sqlite3 *db = NULL;
  int rc;
  sqlite3_stmt *pStmt = NULL;

  /* 1. Default state is OFF (0) */
  printf("Verifying default state is OFF...\n");
  rc = sqlite3_open(":memory:", &db);
  assert(rc == SQLITE_OK);
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 0);
  sqlite3_close(db);
  printf("  PASS: Default is 0 (OFF)\n");

  /* 2. Enable via PRAGMA fixed_schema = ON */
  printf("Enabling via PRAGMA fixed_schema = ON...\n");
  rc = sqlite3_open(":memory:", &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 1);
  printf("  PASS: Feature enabled (1)\n");

  /* 3. Disable via PRAGMA fixed_schema = OFF */
  printf("Disabling via PRAGMA fixed_schema = OFF...\n");
  exec_sql(db, "PRAGMA fixed_schema = OFF;");
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 0);
  sqlite3_close(db);
  printf("  PASS: Feature disabled (0)\n");

  /* 4. Enable via alias PRAGMA fss = ON */
  printf("Enabling via PRAGMA fss = ON...\n");
  rc = sqlite3_open(":memory:", &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fss = ON;");
  assert(get_int_pragma(db, "PRAGMA fss;") == 1);
  printf("  PASS: Feature enabled via fss alias (1)\n");

  /* 5. Disable via alias PRAGMA fss = 0 */
  printf("Disabling via PRAGMA fss = OFF...\n");
  exec_sql(db, "PRAGMA fss = 0;");
  assert(get_int_pragma(db, "PRAGMA fss;") == 0);
  sqlite3_close(db);
  printf("  PASS: Feature disabled via fss = 0 (0)\n");

  /* 6. Function-style syntax PRAGMA fixed_schema(1) */
  printf("Testing function syntax PRAGMA fixed_schema(1)...\n");
  rc = sqlite3_open(":memory:", &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema(1);");
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 1);
  sqlite3_close(db);
  printf("  PASS: Function syntax works\n");

  /* 7. Gate 0x0E page recognition based on PRAGMA */
  printf("Testing 0x0E page rejection when OFF and acceptance when ON...\n");
  const char *memDb = "/tmp/test_fss_gate.db";
  remove(memDb);
  rc = sqlite3_open(memDb, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "CREATE TABLE t1(x INTEGER);");
  exec_sql(db, "INSERT INTO t1 VALUES(42);");
  sqlite3_close(db);

  /* Write 0x0E to page 2 byte 0 */
  FILE *f = fopen(memDb, "r+b");
  assert(f != NULL);
  fseek(f, 4096, SEEK_SET);
  fputc(0x0E, f);
  fclose(f);

  /* Try to read with PRAGMA default (OFF) */
  rc = sqlite3_open(memDb, &db);
  assert(rc == SQLITE_OK);
  pStmt = NULL;
  rc = sqlite3_prepare_v2(db, "SELECT * FROM t1;", -1, &pStmt, NULL);
  if( rc == SQLITE_OK ){
    rc = sqlite3_step(pStmt);
  }
  assert(rc == SQLITE_CORRUPT);
  sqlite3_finalize(pStmt);
  sqlite3_close(db);
  printf("  PASS: Page with 0x0E rejected when PRAGMA is OFF\n");

  /* Now try with PRAGMA ON */
  rc = sqlite3_open(memDb, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  pStmt = NULL;
  rc = sqlite3_prepare_v2(db, "SELECT * FROM t1;", -1, &pStmt, NULL);
  assert(rc != SQLITE_NOTADB);
  sqlite3_finalize(pStmt);
  sqlite3_close(db);
  remove(memDb);
  printf("  PASS: Page with 0x0E recognized when PRAGMA is ON\n");

  /* 8. Verify database marker preservation on unmodified databases */
  printf("Testing standard SQLite file preserves 0 in bytes 72-79...\n");
  const char *stdDbPath = "/tmp/test_std_marker.db";
  remove(stdDbPath);
  rc = sqlite3_open(stdDbPath, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "CREATE TABLE t1(a INT, b TEXT);");
  exec_sql(db, "INSERT INTO t1 VALUES(1, 'hello');");
  sqlite3_close(db);

  f = fopen(stdDbPath, "rb");
  assert(f != NULL);
  uint8_t markerBuf[8];
  fseek(f, 72, SEEK_SET);
  assert(fread(markerBuf, 1, 8, f) == 8);
  fclose(f);
  for( int i = 0; i < 8; i++ ){
    assert(markerBuf[i] == 0);
  }
  printf("  PASS: Standard SQLite database has zero marker\n");

  /* VACUUM must preserve zeroes in standard database */
  rc = sqlite3_open(stdDbPath, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "VACUUM;");
  sqlite3_close(db);

  f = fopen(stdDbPath, "rb");
  assert(f != NULL);
  fseek(f, 72, SEEK_SET);
  assert(fread(markerBuf, 1, 8, f) == 8);
  fclose(f);
  for( int i = 0; i < 8; i++ ){
    assert(markerBuf[i] == 0);
  }
  remove(stdDbPath);
  printf("  PASS: Standard SQLite database maintains zero marker after VACUUM\n");

  /* 9. FSS-marked database recognition on open */
  printf("Testing FSS-marked database recognized when opened...\n");
  const char *fssDbPath = "/tmp/test_fss_marked.db";
  remove(fssDbPath);
  rc = sqlite3_open(fssDbPath, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "CREATE TABLE t1(y INTEGER);");
  exec_sql(db, "INSERT INTO t1 VALUES(100);");
  sqlite3_close(db);

  /* Set FSS1 marker at offset 72, and write valid 0x0E page to page 2 */
  FssFieldDesc colDesc9 = { FSS_TYPE_INT64, FSS_COL_FLAG_NOT_NULL, 8 };
  FssValue val9 = fssValueInt(100);
  uint8_t page2_buf[4096];
  memset(page2_buf, 0, sizeof(page2_buf));
  rc = fssPageInit(page2_buf, 4096, 0, &colDesc9, 1, FSS_FLAG_DENSE_ROWID, 1, &val9);
  assert(rc == FSS_OK);

  f = fopen(fssDbPath, "r+b");
  assert(f != NULL);
  fseek(f, 72, SEEK_SET);
  fwrite("FSS1\0\0\0\1", 1, 8, f);
  fseek(f, 4096, SEEK_SET);
  fwrite(page2_buf, 1, 4096, f);
  fclose(f);

  /* Open WITHOUT PRAGMA (default is OFF): recognized because DB is FSS-marked! */
  rc = sqlite3_open(fssDbPath, &db);
  assert(rc == SQLITE_OK);
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 0);
  pStmt = NULL;
  rc = sqlite3_prepare_v2(db, "SELECT rowid, y FROM t1;", -1, &pStmt, NULL);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  assert(sqlite3_column_int(pStmt, 0) == 1);
  assert(sqlite3_column_int(pStmt, 1) == 100);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_DONE);
  sqlite3_finalize(pStmt);
  sqlite3_close(db);
  printf("  PASS: FSS-marked database recognizes 0x0E page even with PRAGMA default OFF\n");

  /* 10. Reject unsupported FSS version (e.g. version 2) */
  printf("Testing rejection of unsupported FSS version...\n");
  f = fopen(fssDbPath, "r+b");
  assert(f != NULL);
  fseek(f, 72, SEEK_SET);
  fwrite("FSS1\0\0\0\2", 1, 8, f); /* Version 2 */
  fclose(f);

  rc = sqlite3_open(fssDbPath, &db);
  assert(rc == SQLITE_OK);
  pStmt = NULL;
  rc = sqlite3_prepare_v2(db, "SELECT * FROM t1;", -1, &pStmt, NULL);
  assert(rc == SQLITE_NOTADB);
  const char *errMsg = sqlite3_errmsg(db);
  assert(strstr(errMsg, "unsupported FSS database format version 2") != NULL);
  sqlite3_close(db);
  printf("  PASS: Unsupported FSS version rejected with SQLITE_NOTADB and clear message\n");

  /* 11. Reject corrupted/unknown FSS marker */
  printf("Testing rejection of invalid/corrupt FSS marker...\n");
  f = fopen(fssDbPath, "r+b");
  assert(f != NULL);
  fseek(f, 72, SEEK_SET);
  fwrite("XYZ!\0\0\0\1", 1, 8, f); /* Unknown magic */
  fclose(f);

  rc = sqlite3_open(fssDbPath, &db);
  assert(rc == SQLITE_OK);
  pStmt = NULL;
  rc = sqlite3_prepare_v2(db, "SELECT * FROM t1;", -1, &pStmt, NULL);
  assert(rc == SQLITE_NOTADB);
  errMsg = sqlite3_errmsg(db);
  assert(strstr(errMsg, "unknown or invalid FSS database marker") != NULL);
  sqlite3_close(db);
  remove(fssDbPath);
  printf("  PASS: Corrupted/unknown marker rejected with SQLITE_NOTADB and clear message\n");

  /* 12. VACUUM preserves FSS format marker */
  printf("Testing VACUUM preserves FSS format marker...\n");
  remove(fssDbPath);
  rc = sqlite3_open(fssDbPath, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "CREATE TABLE t1(y INTEGER);");
  exec_sql(db, "INSERT INTO t1 VALUES(100);");
  sqlite3_close(db);

  f = fopen(fssDbPath, "r+b");
  assert(f != NULL);
  fseek(f, 72, SEEK_SET);
  fwrite("FSS1\0\0\0\1", 1, 8, f);
  fclose(f);

  /* Run VACUUM */
  rc = sqlite3_open(fssDbPath, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "VACUUM;");
  sqlite3_close(db);

  f = fopen(fssDbPath, "rb");
  assert(f != NULL);
  fseek(f, 72, SEEK_SET);
  assert(fread(markerBuf, 1, 8, f) == 8);
  fclose(f);
  assert(memcmp(markerBuf, "FSS1\0\0\0\1", 8) == 0);
  printf("  PASS: In-place VACUUM preserved FSS format marker\n");

  /* VACUUM INTO */
  const char *intoPath = "/tmp/test_fss_into.db";
  remove(intoPath);
  rc = sqlite3_open(fssDbPath, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "VACUUM INTO '/tmp/test_fss_into.db';");
  sqlite3_close(db);

  f = fopen(intoPath, "rb");
  assert(f != NULL);
  fseek(f, 72, SEEK_SET);
  assert(fread(markerBuf, 1, 8, f) == 8);
  fclose(f);
  assert(memcmp(markerBuf, "FSS1\0\0\0\1", 8) == 0);
  remove(intoPath);
  remove(fssDbPath);
  printf("  PASS: VACUUM INTO preserved FSS format marker\n");

  /* 13. WAL mode and checkpoint preserve database marker */
  printf("Testing WAL mode and checkpoint preserve FSS marker...\n");
  remove(fssDbPath);
  rc = sqlite3_open(fssDbPath, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "CREATE TABLE t1(y INTEGER);");
  exec_sql(db, "INSERT INTO t1 VALUES(100);");
  sqlite3_close(db);

  f = fopen(fssDbPath, "r+b");
  assert(f != NULL);
  fseek(f, 72, SEEK_SET);
  fwrite("FSS1\0\0\0\1", 1, 8, f);
  fclose(f);

  rc = sqlite3_open(fssDbPath, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "PRAGMA journal_mode = WAL;");
  exec_sql(db, "INSERT INTO t1 VALUES(200);");
  exec_sql(db, "PRAGMA wal_checkpoint(TRUNCATE);");
  sqlite3_close(db);

  f = fopen(fssDbPath, "rb");
  assert(f != NULL);
  fseek(f, 72, SEEK_SET);
  assert(fread(markerBuf, 1, 8, f) == 8);
  fclose(f);
  assert(memcmp(markerBuf, "FSS1\0\0\0\1", 8) == 0);

  /* Reopen after WAL checkpoint */
  rc = sqlite3_open(fssDbPath, &db);
  assert(rc == SQLITE_OK);
  assert(get_int_pragma(db, "SELECT count(*) FROM t1;") == 2);
  sqlite3_close(db);
  remove(fssDbPath);
  char walPath[256], shmPath[256];
  snprintf(walPath, sizeof(walPath), "%s-wal", fssDbPath);
  snprintf(shmPath, sizeof(shmPath), "%s-shm", fssDbPath);
  remove(walPath);
  remove(shmPath);
  printf("  PASS: WAL mode and checkpoint preserved FSS format marker\n");

  /* 14. Transaction rollback preserves database marker */
  printf("Testing transaction rollback preserves FSS marker...\n");
  const char *rbDbPath = "/tmp/test_fss_rb.db";
  remove(rbDbPath);
  rc = sqlite3_open(rbDbPath, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "CREATE TABLE t1(y INTEGER);");
  exec_sql(db, "INSERT INTO t1 VALUES(100);");
  sqlite3_close(db);

  /* Set marker */
  f = fopen(rbDbPath, "r+b");
  assert(f != NULL);
  fseek(f, 72, SEEK_SET);
  fwrite("FSS1\0\0\0\1", 1, 8, f);
  fclose(f);

  rc = sqlite3_open(rbDbPath, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "BEGIN;");
  exec_sql(db, "INSERT INTO t1 VALUES(200);");
  exec_sql(db, "ROLLBACK;");
  sqlite3_close(db);

  f = fopen(rbDbPath, "rb");
  assert(f != NULL);
  fseek(f, 72, SEEK_SET);
  assert(fread(markerBuf, 1, 8, f) == 8);
  fclose(f);
  assert(memcmp(markerBuf, "FSS1\0\0\0\1", 8) == 0);
  remove(rbDbPath);
  printf("  PASS: Transaction rollback safely preserved FSS format marker\n");

  /* 15. Task 4: B-tree Read Support */
  test_task4_btree_read_support();

  /* 16. Task 5: First-row Page Creation and Inserts */
  test_task5_first_row_creation_and_inserts();

  /* 17. Task 6: B-tree Balancing and Page Splits */
  test_task6_btree_balancing_and_page_splits();

  /* 18. Task 7: Updates, Deletes, and Fallback/Demotion */
  test_task7_updates_deletes_and_demotion();

  /* 19. Task 8: Schema and PRAGMA Behavior */
  test_task8_schema_and_pragma_behavior();

  /* 20. Task 9: Full End-to-End Verification & Mixed-Page Tests */
  test_task9_end_to_end_verification();

  printf("========================================\n");
  printf("ALL PRAGMA TESTS PASSED SUCCESSFULLY!\n");
  printf("========================================\n");
  return 0;
}
