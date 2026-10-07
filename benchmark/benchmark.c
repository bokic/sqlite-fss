/*
** SQLite Fixed-Schema Storage (FSS) vs. Standard SQLite Benchmark & Verification
**
** This program:
**  1. Creates and fills two SQLite databases with the same 1,000,000 records:
**     - standard.db (Standard SQLite dynamic leaf format 0x0D)
**     - fss.db (Fixed-Schema Storage leaf format 0x0E)
**  2. Compares disk usage (bytes and page counts) proving storage savings.
**  3. Benchmarks 1,000,000 INSERT operations on both databases.
**  4. Benchmarks 1,000,000 SELECT operations (point lookups).
**  5. Benchmarks 1,000,000 UPDATE operations on both databases.
**  6. Verifies 100% data correctness across both databases.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <assert.h>
#include "sqlite3.h"
#include "fss.h"

#define NUM_ROWS 1000000
#define PAGE_SIZE 4096

static double get_time_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static long get_file_size(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    return (long)st.st_size;
  }
  return -1;
}

static inline void putU16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xff);
}
static inline void putU32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)((v >> 24) & 0xff);
  p[1] = (uint8_t)((v >> 16) & 0xff);
  p[2] = (uint8_t)((v >> 8) & 0xff);
  p[3] = (uint8_t)(v & 0xff);
}

static void format_interior_root_page(uint8_t *page, int numLeaves, int cap, int totalRows) {
  memset(page, 0, PAGE_SIZE);
  page[0] = 0x05; /* Interior table page */
  putU16(&page[1], 0); /* First freeblock */
  putU16(&page[3], (uint16_t)(numLeaves - 1)); /* Number of cells */

  uint16_t contentOffset = PAGE_SIZE;
  uint16_t ptrOffset = 12;

  for (int i = 0; i < numLeaves - 1; i++) {
    uint32_t childPgno = 3 + i;
    int maxRowidOnChild = (i + 1) * cap;
    if (maxRowidOnChild > totalRows) maxRowidOnChild = totalRows;

    uint8_t cellBuf[16];
    putU32(cellBuf, childPgno);
    int varintLen = fssPutVarint(cellBuf + 4, (uint64_t)maxRowidOnChild);
    int cellSize = 4 + varintLen;

    contentOffset -= cellSize;
    memcpy(page + contentOffset, cellBuf, cellSize);
    putU16(page + ptrOffset + i * 2, contentOffset);
  }

  putU16(&page[5], contentOffset); /* Content start offset */
  page[7] = 0; /* Fragmented free bytes */
  putU32(&page[8], (uint32_t)(3 + numLeaves - 1)); /* Right child pointer */
}

int main(void) {
  printf("=======================================================================\n");
  printf(" SQLite Fixed-Schema Storage (FSS) vs Standard SQLite Benchmark\n");
  printf(" Workload: %d Records (timestamp INT64, sensor_id INT32, reading FLOAT64, status INT32)\n", NUM_ROWS);
  printf("=======================================================================\n\n");

  const char *stdDbPath = "standard.db";
  const char *fssDbPath = "fss.db";
  remove(stdDbPath);
  remove(fssDbPath);

  sqlite3 *dbStd = NULL;
  sqlite3 *dbFss = NULL;

  /* -------------------------------------------------------------------------
  ** 1. INSERT BENCHMARK
  ** ------------------------------------------------------------------------- */
  printf("[1/4] Benchmarking %d INSERT operations...\n", NUM_ROWS);

  /* Benchmark FSS INSERT */
  int rc = sqlite3_open(fssDbPath, &dbFss);
  assert(rc == SQLITE_OK);
  sqlite3_exec(dbFss, "PRAGMA page_size = 4096;", NULL, NULL, NULL);
  sqlite3_exec(dbFss, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);
  sqlite3_exec(dbFss, "PRAGMA journal_mode = MEMORY;", NULL, NULL, NULL);
  rc = sqlite3_exec(dbFss, "PRAGMA fixed_schema = ON;", NULL, NULL, NULL);
  assert(rc == SQLITE_OK);

  sqlite3_exec(dbFss,
    "CREATE TABLE sensor_readings(timestamp INT64 NOT NULL, sensor_id INT32 NOT NULL, reading DOUBLE NOT NULL, status INT32 NOT NULL);",
    NULL, NULL, NULL
  );

  sqlite3_stmt *stmtInsertFss;
  rc = sqlite3_prepare_v2(dbFss,
    "INSERT INTO sensor_readings(rowid, timestamp, sensor_id, reading, status) VALUES (?, ?, ?, ?, ?);",
    -1, &stmtInsertFss, NULL
  );
  assert(rc == SQLITE_OK);

  sqlite3_exec(dbFss, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  double t1 = get_time_sec();
  for (int i = 1; i <= NUM_ROWS; i++) {
    sqlite3_bind_int64(stmtInsertFss, 1, i);
    sqlite3_bind_int64(stmtInsertFss, 2, 1728123000000LL + (int64_t)i * 1000LL);
    sqlite3_bind_int(stmtInsertFss, 3, 100000 + (i % 250));
    sqlite3_bind_double(stmtInsertFss, 4, 20.0 + (i % 100) * 0.25);
    sqlite3_bind_int(stmtInsertFss, 5, 50000 + (i % 10));
    sqlite3_step(stmtInsertFss);
    sqlite3_reset(stmtInsertFss);
  }
  sqlite3_exec(dbFss, "COMMIT;", NULL, NULL, NULL);
  double fssInsertTime = get_time_sec() - t1;
  sqlite3_finalize(stmtInsertFss);
  sqlite3_close(dbFss);

  /* Benchmark Standard SQLite INSERT */
  rc = sqlite3_open(stdDbPath, &dbStd);
  assert(rc == SQLITE_OK);
  sqlite3_exec(dbStd, "PRAGMA page_size = 4096;", NULL, NULL, NULL);
  sqlite3_exec(dbStd, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);
  sqlite3_exec(dbStd, "PRAGMA journal_mode = MEMORY;", NULL, NULL, NULL);
  rc = sqlite3_exec(dbStd, "PRAGMA fixed_schema = OFF;", NULL, NULL, NULL);
  assert(rc == SQLITE_OK);
  sqlite3_exec(dbStd,
    "CREATE TABLE sensor_readings(timestamp INT64 NOT NULL, sensor_id INT32 NOT NULL, reading DOUBLE NOT NULL, status INT32 NOT NULL);",
    NULL, NULL, NULL
  );

  sqlite3_stmt *stmtInsertStd;
  rc = sqlite3_prepare_v2(dbStd,
    "INSERT INTO sensor_readings(rowid, timestamp, sensor_id, reading, status) VALUES (?, ?, ?, ?, ?);",
    -1, &stmtInsertStd, NULL
  );
  assert(rc == SQLITE_OK);

  sqlite3_exec(dbStd, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  double t0 = get_time_sec();
  for (int i = 1; i <= NUM_ROWS; i++) {
    sqlite3_bind_int64(stmtInsertStd, 1, i);
    sqlite3_bind_int64(stmtInsertStd, 2, 1728123000000LL + (int64_t)i * 1000LL);
    sqlite3_bind_int(stmtInsertStd, 3, 100000 + (i % 250));
    sqlite3_bind_double(stmtInsertStd, 4, 20.0 + (i % 100) * 0.25);
    sqlite3_bind_int(stmtInsertStd, 5, 50000 + (i % 10));
    sqlite3_step(stmtInsertStd);
    sqlite3_reset(stmtInsertStd);
  }
  sqlite3_exec(dbStd, "COMMIT;", NULL, NULL, NULL);
  double stdInsertTime = get_time_sec() - t0;
  sqlite3_finalize(stmtInsertStd);
  sqlite3_close(dbStd);

  printf("  -> Standard SQLite: %.4f s (%ld ops/sec)\n",
         stdInsertTime, (long)(NUM_ROWS / stdInsertTime));
  printf("  -> FSS Storage:     %.4f s (%ld ops/sec) [%.1fx speedup]\n\n",
         fssInsertTime, (long)(NUM_ROWS / fssInsertTime), stdInsertTime / fssInsertTime);

  /* -------------------------------------------------------------------------
  ** 2. DISK USAGE COMPARISON
  ** ------------------------------------------------------------------------- */
  printf("[2/4] Measuring Disk Space Consumption...\n");
  long stdSize = get_file_size(stdDbPath);
  long fssSize = get_file_size(fssDbPath);
  long stdPages = stdSize / PAGE_SIZE;
  long fssPages = fssSize / PAGE_SIZE;
  double savingsPct = 100.0 * (double)(stdSize - fssSize) / (double)stdSize;

  printf("  -> Standard SQLite DB: %10ld bytes (%ld pages)\n", stdSize, stdPages);
  printf("  -> FSS Leaf Storage DB: %10ld bytes (%ld pages)\n", fssSize, fssPages);
  printf("  -> SPACE SAVINGS:      %10ld bytes (%.2f%% REDUCTION!)\n",
         stdSize - fssSize, savingsPct);
  assert(fssSize < stdSize);

  /* Verify that fss.db actually contains 0x0E table leaf pages */
  FILE *fFss = fopen(fssDbPath, "rb");
  assert(fFss != NULL);
  uint8_t hdrBuf[16];
  int countFssLeaves = 0;
  int countInterior = 0;
  for (int p = 2; p <= fssPages; p++) {
    fseek(fFss, (long)(p - 1) * PAGE_SIZE, SEEK_SET);
    assert(fread(hdrBuf, 1, 16, fFss) == 16);
    if (hdrBuf[0] == 0x0E) countFssLeaves++;
    else if (hdrBuf[0] == 0x05) countInterior++;
  }
  fclose(fFss);
  printf("  -> Verified actual disk structure: %d FSS leaves (0x0E), %d interior nodes (0x05)\n\n",
         countFssLeaves, countInterior);
  assert(countFssLeaves > 0); /* Fail if expected FSS leaves are absent! */

  /* -------------------------------------------------------------------------
  ** 3. SELECT BENCHMARK (10,000 Point Lookups + Full Scan)
  ** ------------------------------------------------------------------------- */
  printf("[3/4] Benchmarking %d SELECT operations...\n", NUM_ROWS);

  rc = sqlite3_open(stdDbPath, &dbStd);
  assert(rc == SQLITE_OK);
  rc = sqlite3_open(fssDbPath, &dbFss);
  assert(rc == SQLITE_OK);

  /* Enable FSS on the FSS database */
  rc = sqlite3_exec(dbFss, "PRAGMA fixed_schema = ON;", NULL, NULL, NULL);
  assert(rc == SQLITE_OK);

  /* Point Lookups on Standard SQLite */
  sqlite3_stmt *stmtSelectStd;
  rc = sqlite3_prepare_v2(dbStd,
    "SELECT timestamp, sensor_id, reading, status FROM sensor_readings WHERE rowid = ?;",
    -1, &stmtSelectStd, NULL
  );
  assert(rc == SQLITE_OK);

  t0 = get_time_sec();
  for (int i = 1; i <= NUM_ROWS; i++) {
    sqlite3_bind_int64(stmtSelectStd, 1, i);
    rc = sqlite3_step(stmtSelectStd);
    assert(rc == SQLITE_ROW);
    sqlite3_reset(stmtSelectStd);
  }
  double stdSelectTime = get_time_sec() - t0;
  sqlite3_finalize(stmtSelectStd);

  /* Point Lookups on FSS */
  sqlite3_stmt *stmtSelectFss;
  rc = sqlite3_prepare_v2(dbFss,
    "SELECT timestamp, sensor_id, reading, status FROM sensor_readings WHERE rowid = ?;",
    -1, &stmtSelectFss, NULL
  );
  assert(rc == SQLITE_OK);

  t0 = get_time_sec();
  for (int i = 1; i <= NUM_ROWS; i++) {
    sqlite3_bind_int64(stmtSelectFss, 1, i);
    rc = sqlite3_step(stmtSelectFss);
    assert(rc == SQLITE_ROW);
    sqlite3_reset(stmtSelectFss);
  }
  double fssSelectTime = get_time_sec() - t0;
  sqlite3_finalize(stmtSelectFss);

  printf("  -> Standard SQLite: %.4f s (%ld queries/sec)\n",
         stdSelectTime, (long)(NUM_ROWS / stdSelectTime));
  printf("  -> FSS Storage:     %.4f s (%ld queries/sec) [%.1fx speedup]\n\n",
         fssSelectTime, (long)(NUM_ROWS / fssSelectTime), stdSelectTime / fssSelectTime);

  /* -------------------------------------------------------------------------
  ** 4. UPDATE BENCHMARK (1,000,000 Updates)
  ** ------------------------------------------------------------------------- */
  printf("[4/4] Benchmarking %d UPDATE operations...\n", NUM_ROWS);

  /*
  ** Both implementations run the same prepared SQL UPDATE workload, in one
  ** transaction. Keep the connection open and check every SQLite result so
  ** an unsupported or skipped FSS update cannot appear as fast throughput.
  */
  assert(sqlite3_exec(dbStd, "PRAGMA synchronous = OFF;", NULL, NULL, NULL) == SQLITE_OK);
  assert(sqlite3_exec(dbStd, "PRAGMA journal_mode = MEMORY;", NULL, NULL, NULL) == SQLITE_OK);
  assert(sqlite3_exec(dbFss, "PRAGMA synchronous = OFF;", NULL, NULL, NULL) == SQLITE_OK);
  assert(sqlite3_exec(dbFss, "PRAGMA journal_mode = MEMORY;", NULL, NULL, NULL) == SQLITE_OK);

  sqlite3_stmt *stmtUpdateStd = NULL;
  rc = sqlite3_prepare_v2(dbStd,
    "UPDATE sensor_readings SET reading = reading + 1.0 WHERE rowid = ?;",
    -1, &stmtUpdateStd, NULL
  );
  assert(rc == SQLITE_OK);
  sqlite3_stmt *stmtUpdateFss = NULL;
  rc = sqlite3_prepare_v2(dbFss,
    "UPDATE sensor_readings SET reading = reading + 1.0 WHERE rowid = ?;",
    -1, &stmtUpdateFss, NULL
  );
  assert(rc == SQLITE_OK);

  rc = sqlite3_exec(dbStd, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  assert(rc == SQLITE_OK);
  t0 = get_time_sec();
  for (int i = 1; i <= NUM_ROWS; i++) {
    assert(sqlite3_bind_int64(stmtUpdateStd, 1, i) == SQLITE_OK);
    rc = sqlite3_step(stmtUpdateStd);
    assert(rc == SQLITE_DONE);
    assert(sqlite3_changes(dbStd) == 1);
    assert(sqlite3_reset(stmtUpdateStd) == SQLITE_OK);
  }
  assert(sqlite3_exec(dbStd, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK);
  double stdUpdateTime = get_time_sec() - t0;
  sqlite3_finalize(stmtUpdateStd);

  rc = sqlite3_exec(dbFss, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  assert(rc == SQLITE_OK);
  t0 = get_time_sec();
  for (int i = 1; i <= NUM_ROWS; i++) {
    assert(sqlite3_bind_int64(stmtUpdateFss, 1, i) == SQLITE_OK);
    rc = sqlite3_step(stmtUpdateFss);
    assert(rc == SQLITE_DONE);
    assert(sqlite3_changes(dbFss) == 1);
    assert(sqlite3_reset(stmtUpdateFss) == SQLITE_OK);
  }
  assert(sqlite3_exec(dbFss, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK);
  double fssUpdateTime = get_time_sec() - t0;
  sqlite3_finalize(stmtUpdateFss);

  printf("  -> Standard SQLite: %.4f s (%ld updates/sec)\n",
         stdUpdateTime, (long)(NUM_ROWS / stdUpdateTime));
  printf("  -> FSS Storage:     %.4f s (%ld updates/sec) [%.1fx speedup]\n\n",
         fssUpdateTime, (long)(NUM_ROWS / fssUpdateTime), stdUpdateTime / fssUpdateTime);

  /* -------------------------------------------------------------------------
  ** 5. DATA CORRECTNESS VERIFICATION
  ** ------------------------------------------------------------------------- */
  printf("Verifying 100%% Data Consistency across both databases...\n");
  sqlite3_stmt *sStd, *sFss;
  sqlite3_prepare_v2(dbStd, "SELECT rowid, timestamp, sensor_id, reading, status FROM sensor_readings ORDER BY rowid;", -1, &sStd, NULL);
  sqlite3_prepare_v2(dbFss, "SELECT rowid, timestamp, sensor_id, reading, status FROM sensor_readings ORDER BY rowid;", -1, &sFss, NULL);

  int verifiedCount = 0;
  while (sqlite3_step(sStd) == SQLITE_ROW && sqlite3_step(sFss) == SQLITE_ROW) {
    int64_t id1 = sqlite3_column_int64(sStd, 0);
    int64_t id2 = sqlite3_column_int64(sFss, 0);
    int64_t ts1 = sqlite3_column_int64(sStd, 1);
    int64_t ts2 = sqlite3_column_int64(sFss, 1);
    int sid1 = sqlite3_column_int(sStd, 2);
    int sid2 = sqlite3_column_int(sFss, 2);
    double r1 = sqlite3_column_double(sStd, 3);
    double r2 = sqlite3_column_double(sFss, 3);
    int st1 = sqlite3_column_int(sStd, 4);
    int st2 = sqlite3_column_int(sFss, 4);

    assert(id1 == id2);
    assert(ts1 == ts2);
    assert(sid1 == sid2);
    assert(st1 == st2);
    assert(r1 > r2 - 0.001 && r1 < r2 + 0.001);
    verifiedCount++;
  }
  sqlite3_finalize(sStd);
  sqlite3_finalize(sFss);
  assert(verifiedCount == NUM_ROWS);
  printf("  -> Verified %d identical rows between Standard SQLite and FSS!\n\n", verifiedCount);

  sqlite3_close(dbStd);
  sqlite3_close(dbFss);

  /* -------------------------------------------------------------------------
  ** EXECUTIVE SUMMARY
  ** ------------------------------------------------------------------------- */
  printf("=======================================================================\n");
  printf(" EXECUTIVE BENCHMARK SUMMARY\n");
  printf("=======================================================================\n");
  printf(" Metric                    | Standard SQLite   | FSS Storage       | Gain\n");
  printf(" --------------------------+-------------------+-------------------+----------\n");
  printf(" Database File Size        | %10ld bytes   | %10ld bytes   | -%.1f%% SPACE\n",
         stdSize, fssSize, savingsPct);
  printf(" Total 4KB Pages Allocated | %10ld pages   | %10ld pages   | -%ld pages\n",
         stdPages, fssPages, stdPages - fssPages);
  printf(" INSERT Throughput         | %10ld ops/s   | %10ld ops/s   | %.1fx FASTER\n",
         (long)(NUM_ROWS / stdInsertTime), (long)(NUM_ROWS / fssInsertTime), stdInsertTime / fssInsertTime);
  printf(" SELECT Throughput         | %10ld ops/s   | %10ld ops/s   | %.1fx FASTER\n",
         (long)(NUM_ROWS / stdSelectTime), (long)(NUM_ROWS / fssSelectTime), stdSelectTime / fssSelectTime);
  printf(" UPDATE Throughput         | %10ld ops/s   | %10ld ops/s   | %.1fx FASTER\n",
         (long)(NUM_ROWS / stdUpdateTime), (long)(NUM_ROWS / fssUpdateTime), stdUpdateTime / fssUpdateTime);
  printf("=======================================================================\n");

  return 0;
}
