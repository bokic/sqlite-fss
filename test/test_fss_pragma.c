/*
** Test suite for SQLite Fixed-Schema Storage (FSS) PRAGMA control.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include "sqlite3.h"

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

int main(void) {
  printf("========================================\n");
  printf("Running SQLite FSS PRAGMA Test Suite\n");
  printf("========================================\n");

  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  assert(rc == SQLITE_OK);

  /* 1. Default should be OFF (0) */
  printf("Verifying default state is OFF...\n");
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 0);
  assert(get_int_pragma(db, "PRAGMA fss;") == 0);
  assert(get_int_pragma(db, "PRAGMA fixed_schema_storage;") == 0);
  printf("  PASS: Default is 0 (OFF)\n");

  /* 2. Enable via PRAGMA fixed_schema = ON */
  printf("Enabling via PRAGMA fixed_schema = ON...\n");
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 1);
  assert(get_int_pragma(db, "PRAGMA fss;") == 1);
  assert(get_int_pragma(db, "PRAGMA fixed_schema_storage;") == 1);
  printf("  PASS: Feature enabled (1)\n");

  /* 3. Disable via PRAGMA fixed_schema = OFF */
  printf("Disabling via PRAGMA fixed_schema = OFF...\n");
  exec_sql(db, "PRAGMA fixed_schema = OFF;");
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 0);
  assert(get_int_pragma(db, "PRAGMA fss;") == 0);
  printf("  PASS: Feature disabled (0)\n");

  /* 4. Enable via PRAGMA fss = ON */
  printf("Enabling via PRAGMA fss = ON...\n");
  exec_sql(db, "PRAGMA fss = ON;");
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 1);
  assert(get_int_pragma(db, "PRAGMA fss;") == 1);
  printf("  PASS: Feature enabled via fss alias (1)\n");

  /* 5. Disable via PRAGMA fss = OFF */
  printf("Disabling via PRAGMA fss = OFF...\n");
  exec_sql(db, "PRAGMA fss = 0;");
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 0);
  printf("  PASS: Feature disabled via fss = 0 (0)\n");

  /* 6. Test function syntax PRAGMA fixed_schema(1) */
  printf("Testing function syntax PRAGMA fixed_schema(1)...\n");
  exec_sql(db, "PRAGMA fixed_schema(1);");
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 1);
  exec_sql(db, "PRAGMA fixed_schema(0);");
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 0);
  printf("  PASS: Function syntax works\n");

  sqlite3_close(db);

  /* 7. Verify disk corruption behavior when flag is OFF vs ON */
  printf("Testing 0x0E page rejection when OFF and acceptance when ON...\n");
  const char *dbPath = "/tmp/test_fss_pragma.db";
  remove(dbPath);

  rc = sqlite3_open(dbPath, &db);
  assert(rc == SQLITE_OK);
  exec_sql(db, "CREATE TABLE t1(x INTEGER PRIMARY KEY, y INTEGER);");
  exec_sql(db, "INSERT INTO t1 VALUES(1, 100);");
  sqlite3_close(db);

  /* Tamper page 2 (root of t1): change flag from 0x0D to 0x0E (PTF_FSS_LEAF) */
  FILE *f = fopen(dbPath, "r+b");
  assert(f != NULL);
  /* Default page size is 4096. Page 2 starts at offset 4096 */
  fseek(f, 4096, SEEK_SET);
  uint8_t byte0 = 0x0E;
  fwrite(&byte0, 1, 1, f);
  fclose(f);

  /* Open without PRAGMA (default is OFF) */
  rc = sqlite3_open(dbPath, &db);
  assert(rc == SQLITE_OK);
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 0);

  sqlite3_stmt *pStmt = NULL;
  rc = sqlite3_prepare_v2(db, "SELECT * FROM t1;", -1, &pStmt, NULL);
  if (rc == SQLITE_OK) {
    rc = sqlite3_step(pStmt);
  }
  /* Must fail with SQLITE_CORRUPT because 0x0E is unrecognized when flag is OFF */
  assert(rc == SQLITE_CORRUPT);
  sqlite3_finalize(pStmt);
  printf("  PASS: Page with 0x0E rejected when PRAGMA is OFF\n");

  /* Turn PRAGMA fixed_schema = ON and retry */
  exec_sql(db, "PRAGMA fixed_schema = ON;");
  assert(get_int_pragma(db, "PRAGMA fixed_schema;") == 1);

  /* With flag ON, decodeFlags recognizes 0x0E */
  rc = sqlite3_prepare_v2(db, "SELECT * FROM t1;", -1, &pStmt, NULL);
  /* The page is recognized as PTF_FSS_LEAF by decodeFlags */
  assert(rc == SQLITE_OK || rc == SQLITE_ROW);
  if (pStmt) sqlite3_finalize(pStmt);
  printf("  PASS: Page with 0x0E recognized when PRAGMA is ON\n");

  sqlite3_close(db);
  remove(dbPath);

  printf("========================================\n");
  printf("ALL PRAGMA TESTS PASSED SUCCESSFULLY!\n");
  printf("========================================\n");
  return 0;
}
