/*
** 2026-10-05
**
** Test suite for SQLite Fixed-Schema Storage (FSS) leaf page format (0x0E).
** Defined in DESIGN.md.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "fss.h"

#define PAGE_SIZE 4096

static void test_fss_init_and_capacity(void) {
  printf("Running test_fss_init_and_capacity...\n");
  uint8_t page[PAGE_SIZE];

  FssFieldDesc cols[3] = {
    { FSS_TYPE_INT32, FSS_COL_FLAG_NOT_NULL, 4 },
    { FSS_TYPE_FLOAT64, FSS_COL_FLAG_NOT_NULL, 8 },
    { FSS_TYPE_INT16, FSS_COL_FLAG_NOT_NULL, 2 }
  };

  uint16_t rowPayloadSize, nullBytes;
  int rc = fssCalculateRowSize(cols, 3, 0, &rowPayloadSize, &nullBytes);
  assert(rc == FSS_OK);
  assert(rowPayloadSize == 14);
  assert(nullBytes == 0);

  /* In sparse mode: 8 bytes rowid + 14 bytes row payload = 22 bytes per row */
  uint16_t cap = fssCalculateCapacity(PAGE_SIZE, 0, 3, rowPayloadSize, 0);
  /* Avail = 4096 - 16 - (2 + 3*4) = 4066. 4066 / 22 = 184 */
  assert(cap == 184);

  /* In dense mode: 8 bytes min_rowid total. 4066 - 8 = 4058. 4058 / 14 = 289 rows */
  uint16_t capDense = fssCalculateCapacity(PAGE_SIZE, 0, 3, rowPayloadSize, FSS_FLAG_DENSE_ROWID);
  assert(capDense == 289);

  /* Initialize with initial row (invariant: cell_count >= 1) */
  FssValue initVals[3];
  initVals[0] = fssValueInt(42);
  initVals[1] = fssValueFloat(3.14159);
  initVals[2] = fssValueInt(100);

  rc = fssPageInit(page, PAGE_SIZE, 0, cols, 3, 0, 1001, initVals);
  assert(rc == FSS_OK);

  /* Parse back */
  FssPage fssPage;
  rc = fssPageParse(page, PAGE_SIZE, 0, &fssPage);
  assert(rc == FSS_OK);
  assert(fssPage.hdr.page_type == 0x0E);
  assert(fssPage.hdr.cell_count == 1);
  assert(fssPage.hdr.capacity == 184);
  assert(fssPage.schema.num_columns == 3);

  /* Check initial row values */
  int64_t rid;
  FssValue outVals[3];
  rc = fssPageGetRow(&fssPage, 0, &rid, outVals, 3);
  assert(rc == FSS_OK);
  assert(rid == 1001);
  assert(outVals[0].u.i == 42);
  assert(outVals[1].u.r > 3.14158 && outVals[1].u.r < 3.14160);
  assert(outVals[2].u.i == 100);

  printf("  PASS: test_fss_init_and_capacity\n");
}

static void test_fss_sparse_inserts_and_lookups(void) {
  printf("Running test_fss_sparse_inserts_and_lookups...\n");
  uint8_t page[PAGE_SIZE];

  FssFieldDesc cols[2] = {
    { FSS_TYPE_INT32, FSS_COL_FLAG_NOT_NULL, 4 },
    { FSS_TYPE_INT64, FSS_COL_FLAG_NOT_NULL, 8 }
  };

  FssValue initVals[2] = { fssValueInt(10), fssValueInt(1000) };
  int rc = fssPageInit(page, PAGE_SIZE, 0, cols, 2, 0, 50, initVals);
  assert(rc == FSS_OK);

  FssPage fssPage;
  rc = fssPageParse(page, PAGE_SIZE, 0, &fssPage);
  assert(rc == FSS_OK);

  /* Insert out of order: 20, 80, 10, 60 */
  int64_t rids[] = { 20, 80, 10, 60 };
  for (int i = 0; i < 4; i++) {
    FssValue vals[2] = { fssValueInt(i + 1), fssValueInt((i + 1) * 100) };
    rc = fssPageInsert(&fssPage, rids[i], vals, 2);
    assert(rc == FSS_OK);
  }
  assert(fssPage.hdr.cell_count == 5);

  /* Duplicate check */
  FssValue dupVals[2] = { fssValueInt(99), fssValueInt(999) };
  rc = fssPageInsert(&fssPage, 50, dupVals, 2);
  assert(rc == FSS_DUPLICATE);

  /* Verify sorted order: 10, 20, 50, 60, 80 */
  int64_t expectedRids[] = { 10, 20, 50, 60, 80 };
  for (uint16_t i = 0; i < 5; i++) {
    int64_t rid;
    FssValue v[2];
    rc = fssPageGetRow(&fssPage, i, &rid, v, 2);
    assert(rc == FSS_OK);
    assert(rid == expectedRids[i]);

    /* Test binary search lookup */
    uint16_t slot;
    rc = fssPageFindRow(&fssPage, expectedRids[i], &slot);
    assert(rc == FSS_OK);
    assert(slot == i);
  }

  /* Non-existent rowid */
  uint16_t slot;
  rc = fssPageFindRow(&fssPage, 999, &slot);
  assert(rc == FSS_NOTFOUND);

  printf("  PASS: test_fss_sparse_inserts_and_lookups\n");
}

static void test_fss_dense_mode(void) {
  printf("Running test_fss_dense_mode...\n");
  uint8_t page[PAGE_SIZE];

  FssFieldDesc cols[1] = {
    { FSS_TYPE_INT32, FSS_COL_FLAG_NOT_NULL, 4 }
  };

  FssValue initVal = fssValueInt(100);
  int rc = fssPageInit(page, PAGE_SIZE, 0, cols, 1, FSS_FLAG_DENSE_ROWID, 1000, &initVal);
  assert(rc == FSS_OK);

  FssPage fssPage;
  rc = fssPageParse(page, PAGE_SIZE, 0, &fssPage);
  assert(rc == FSS_OK);
  assert(fssPage.hdr.flags & FSS_FLAG_DENSE_ROWID);

  /* Sequential append: 1001, 1002, 1003 ... */
  for (int64_t r = 1001; r <= 1005; r++) {
    FssValue v = fssValueInt((int32_t)r * 2);
    rc = fssPageInsert(&fssPage, r, &v, 1);
    assert(rc == FSS_OK);
  }
  assert(fssPage.hdr.cell_count == 6);

  /* Direct O(1) lookups */
  for (int64_t r = 1000; r <= 1005; r++) {
    uint16_t s;
    rc = fssPageFindRow(&fssPage, r, &s);
    assert(rc == FSS_OK);
    assert(s == (uint16_t)(r - 1000));

    FssValue colVal;
    rc = fssPageGetColumn(&fssPage, s, 0, &colVal);
    assert(rc == FSS_OK);
    if (r == 1000) {
      assert(colVal.u.i == 100);
    } else {
      assert(colVal.u.i == (int32_t)r * 2);
    }
  }

  printf("  PASS: test_fss_dense_mode\n");
}

static void test_fss_nullable_columns(void) {
  printf("Running test_fss_nullable_columns...\n");
  uint8_t page[PAGE_SIZE];

  FssFieldDesc cols[3] = {
    { FSS_TYPE_INT32, FSS_COL_FLAG_NOT_NULL, 4 },
    { FSS_TYPE_INT64, 0, 8 },   /* Nullable */
    { FSS_TYPE_FLOAT64, 0, 8 } /* Nullable */
  };

  FssValue initVals[3] = {
    fssValueInt(1),
    fssValueNull(),
    fssValueFloat(9.99)
  };

  int rc = fssPageInit(page, PAGE_SIZE, 0, cols, 3, FSS_FLAG_NULLABLE, 1, initVals);
  assert(rc == FSS_OK);

  FssPage fssPage;
  rc = fssPageParse(page, PAGE_SIZE, 0, &fssPage);
  assert(rc == FSS_OK);

  FssValue readVals[3];
  rc = fssPageGetRow(&fssPage, 0, NULL, readVals, 3);
  assert(rc == FSS_OK);
  assert(readVals[0].is_null == 0 && readVals[0].u.i == 1);
  assert(readVals[1].is_null == 1);
  assert(readVals[2].is_null == 0 && readVals[2].u.r > 9.98 && readVals[2].u.r < 10.0);

  /* Update column 1 to non-null */
  FssValue newVal = fssValueInt(123456789);
  rc = fssPageUpdateColumn(&fssPage, 0, 1, &newVal);
  assert(rc == FSS_OK);

  FssValue col1;
  rc = fssPageGetColumn(&fssPage, 0, 1, &col1);
  assert(rc == FSS_OK);
  assert(col1.is_null == 0);
  assert(col1.u.i == 123456789);

  printf("  PASS: test_fss_nullable_columns\n");
}

static void test_fss_deletion_and_underflow(void) {
  printf("Running test_fss_deletion_and_underflow...\n");
  uint8_t page[PAGE_SIZE];

  FssFieldDesc cols[1] = {
    { FSS_TYPE_INT32, FSS_COL_FLAG_NOT_NULL, 4 }
  };

  FssValue val1 = fssValueInt(10);
  int rc = fssPageInit(page, PAGE_SIZE, 0, cols, 1, 0, 1, &val1);
  assert(rc == FSS_OK);

  FssPage fssPage;
  rc = fssPageParse(page, PAGE_SIZE, 0, &fssPage);
  assert(rc == FSS_OK);

  /* Insert second and third rows */
  FssValue val2 = fssValueInt(20);
  FssValue val3 = fssValueInt(30);
  fssPageInsert(&fssPage, 2, &val2, 1);
  fssPageInsert(&fssPage, 3, &val3, 1);
  assert(fssPage.hdr.cell_count == 3);

  /* Delete middle row (2) */
  rc = fssPageDelete(&fssPage, 2);
  assert(rc == FSS_OK);
  assert(fssPage.hdr.cell_count == 2);

  uint16_t slot;
  rc = fssPageFindRow(&fssPage, 2, &slot);
  assert(rc == FSS_NOTFOUND);

  /* Delete row 1 -> 1 row left */
  rc = fssPageDelete(&fssPage, 1);
  assert(rc == FSS_OK);
  assert(fssPage.hdr.cell_count == 1);

  /*
  ** Invariant check: deleting the final row returns FSS_UNDERFLOW
  ** indicating chunk teardown / page reclaim.
  */
  rc = fssPageDelete(&fssPage, 3);
  assert(rc == FSS_UNDERFLOW);

  printf("  PASS: test_fss_deletion_and_underflow\n");
}

static void test_fss_vectorized_scan(void) {
  printf("Running test_fss_vectorized_scan...\n");
  uint8_t page[PAGE_SIZE];

  FssFieldDesc cols[2] = {
    { FSS_TYPE_INT32, FSS_COL_FLAG_NOT_NULL, 4 },
    { FSS_TYPE_FLOAT64, FSS_COL_FLAG_NOT_NULL, 8 }
  };

  FssValue initVal[2] = { fssValueInt(0), fssValueFloat(0.0) };
  fssPageInit(page, PAGE_SIZE, 0, cols, 2, 0, 1, initVal);

  FssPage fssPage;
  fssPageParse(page, PAGE_SIZE, 0, &fssPage);

  for (int i = 1; i < 20; i++) {
    FssValue v[2] = { fssValueInt(i * 10), fssValueFloat((double)i * 1.5) };
    fssPageInsert(&fssPage, i + 1, v, 2);
  }
  assert(fssPage.hdr.cell_count == 20);

  /* Scan filter: col0 > 100 */
  uint16_t matchingSlots[32];
  uint16_t nMatches = 0;
  FssValue target = fssValueInt(100);
  int rc = fssPageScanFilter(&fssPage, 0, FSS_OP_GT, &target, matchingSlots, 32, &nMatches);
  assert(rc == FSS_OK);
  /* i*10 > 100 for i in 11..19 -> 9 matches */
  assert(nMatches == 9);

  for (int m = 0; m < nMatches; m++) {
    FssValue v;
    fssPageGetColumn(&fssPage, matchingSlots[m], 0, &v);
    assert(v.u.i > 100);
  }

  printf("  PASS: test_fss_vectorized_scan\n");
}

static void test_fss_demotion_in_place(void) {
  printf("Running test_fss_demotion_in_place...\n");
  uint8_t page[PAGE_SIZE];

  FssFieldDesc cols[2] = {
    { FSS_TYPE_INT32, FSS_COL_FLAG_NOT_NULL, 4 },
    { FSS_TYPE_INT32, FSS_COL_FLAG_NOT_NULL, 4 }
  };

  FssValue initVals[2] = { fssValueInt(100), fssValueInt(200) };
  int rc = fssPageInit(page, PAGE_SIZE, 0, cols, 2, 0, 1, initVals);
  assert(rc == FSS_OK);

  FssPage fssPage;
  rc = fssPageParse(page, PAGE_SIZE, 0, &fssPage);
  assert(rc == FSS_OK);

  /* Insert a few more rows */
  for (int i = 2; i <= 5; i++) {
    FssValue v[2] = { fssValueInt(i * 100), fssValueInt(i * 200) };
    fssPageInsert(&fssPage, i, v, 2);
  }
  assert(fssPage.hdr.cell_count == 5);

  /*
  ** Incoming row has a TYPE MISMATCH: TEXT value into INT32 column!
  */
  FssValue mismatchedRow[2] = {
    fssValueText("mismatched_text_string"),
    fssValueInt(999)
  };

  /* Verify mismatch check fails FSS insertion */
  rc = fssCheckRowMatchesSchema(&fssPage.schema, fssPage.hdr.flags, mismatchedRow, 2);
  assert(rc == FSS_TYPE_MISMATCH);

  rc = fssPageInsert(&fssPage, 6, mismatchedRow, 2);
  assert(rc == FSS_TYPE_MISMATCH);

  /* Trigger In-Place Demotion to standard SQLite 0x0D format */
  rc = fssPageDemoteToDynamic(&fssPage, 6, mismatchedRow, 2);
  assert(rc == FSS_OK);

  /*
  ** Verify standard SQLite 0x0D page header:
  ** Byte 0: 0x0D
  ** Cell count: 6 (5 existing + 1 mismatched)
  */
  assert(page[0] == 0x0D);
  uint16_t cellCount = ((uint16_t)page[3] << 8) | page[4];
  assert(cellCount == 6);

  uint16_t firstCellOffset = ((uint16_t)page[5] << 8) | page[6];
  assert(firstCellOffset > 8 + 6 * 2);
  assert(firstCellOffset <= PAGE_SIZE);

  /* Verify cell pointers and decode cells using SQLite varint logic */
  for (int i = 0; i < 6; i++) {
    uint16_t ptr = ((uint16_t)page[8 + i * 2] << 8) | page[8 + i * 2 + 1];
    assert(ptr >= firstCellOffset && ptr < PAGE_SIZE);

    uint64_t payloadLen, rowid;
    int len1 = fssGetVarint(page + ptr, &payloadLen);
    int len2 = fssGetVarint(page + ptr + len1, &rowid);
    assert(rowid == (uint64_t)(i + 1));
    assert(payloadLen > 0);
    assert(ptr + len1 + len2 + payloadLen <= PAGE_SIZE);
  }

  printf("  PASS: test_fss_demotion_in_place\n");
}

static void test_fss_split_required_on_overflow(void) {
  printf("Running test_fss_split_required_on_overflow...\n");
  /* Small 512-byte page */
  uint8_t smallPage[512];

  FssFieldDesc cols[1] = {
    { FSS_TYPE_INT32, FSS_COL_FLAG_NOT_NULL, 4 }
  };

  FssValue val = fssValueInt(1);
  int rc = fssPageInit(smallPage, 512, 0, cols, 1, 0, 1, &val);
  assert(rc == FSS_OK);

  FssPage fssPage;
  rc = fssPageParse(smallPage, 512, 0, &fssPage);
  assert(rc == FSS_OK);

  /* A giant string that cannot fit in 512 bytes along with headers */
  char giantText[500];
  memset(giantText, 'A', 499);
  giantText[499] = '\0';
  FssValue giantVal[1] = { fssValueText(giantText) };

  /* Verify demotion recognizes space overflow and returns FSS_SPLIT_REQUIRED */
  rc = fssPageDemoteToDynamic(&fssPage, 2, giantVal, 1);
  assert(rc == FSS_SPLIT_REQUIRED);

  printf("  PASS: test_fss_split_required_on_overflow\n");
}

static void test_fss_root_page1_offset(void) {
  printf("Running test_fss_root_page1_offset...\n");
  /* Database Page 1 has 100-byte SQLite database header at offset 0 */
  uint8_t page1[PAGE_SIZE];
  memset(page1, 0xCC, 100); /* Simulate 100-byte db header */

  FssFieldDesc cols[1] = {
    { FSS_TYPE_INT64, FSS_COL_FLAG_NOT_NULL, 8 }
  };

  FssValue val = fssValueInt(999999);
  int rc = fssPageInit(page1, PAGE_SIZE, 100, cols, 1, 0, 1, &val);
  assert(rc == FSS_OK);

  /* Ensure the 100-byte header prefix is untouched */
  for (int i = 0; i < 100; i++) {
    assert(page1[i] == 0xCC);
  }

  assert(fssIsFssPage(page1, 100) == 1);

  FssPage fssPage;
  rc = fssPageParse(page1, PAGE_SIZE, 100, &fssPage);
  assert(rc == FSS_OK);
  assert(fssPage.hdrOffset == 100);
  assert(fssPage.hdr.page_type == 0x0E);
  assert(fssPage.hdr.cell_count == 1);

  FssValue readVal;
  rc = fssPageGetColumn(&fssPage, 0, 0, &readVal);
  assert(rc == FSS_OK);
  assert(readVal.u.i == 999999);

  printf("  PASS: test_fss_root_page1_offset\n");
}

int main(void) {
  printf("========================================\n");
  printf("Running SQLite FSS Test Suite\n");
  printf("========================================\n");

  test_fss_init_and_capacity();
  test_fss_sparse_inserts_and_lookups();
  test_fss_dense_mode();
  test_fss_nullable_columns();
  test_fss_deletion_and_underflow();
  test_fss_vectorized_scan();
  test_fss_demotion_in_place();
  test_fss_split_required_on_overflow();
  test_fss_root_page1_offset();

  printf("========================================\n");
  printf("ALL TESTS PASSED SUCCESSFULLY!\n");
  printf("========================================\n");
  return 0;
}
