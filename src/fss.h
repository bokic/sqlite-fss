/*
** 2026-10-05
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** SQLite Fixed-Schema Storage (FSS) Data Leaf Page
**
** Header file for the Fixed-Schema Storage (0x0E) leaf page format.
** Defined in DESIGN.md.
*/
#ifndef SQLITE_FSS_H
#define SQLITE_FSS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** Page type identifiers.
*/
#define FSS_PAGE_TYPE                 0x0E  /* FSS Table Leaf Page */
#define SQLITE_PAGE_DYNAMIC_LEAF      0x0D  /* Standard SQLite Table Leaf Page */

/*
** Page Flags (Offset 0x01 in FSS Page Header).
*/
#define FSS_FLAG_DENSE_ROWID          0x01  /* Dense sequential rowids (min_rowid mode) */
#define FSS_FLAG_NULLABLE             0x02  /* Nullable columns present; null bitmask prepended to row */
#define FSS_FLAG_VAR_ARENA            0x04  /* Variable-length tail arena present */

/*
** Column Descriptor Flags.
*/
#define FSS_COL_FLAG_NOT_NULL         0x01  /* Column cannot contain NULL values */
#define FSS_COL_FLAG_PRIMARY_KEY      0x02  /* Column is primary key */

/*
** Supported Field Data Types (type_id).
*/
#define FSS_TYPE_INT8                 0x01  /* Signed 8-bit integer / boolean */
#define FSS_TYPE_BOOL                 0x01  /* Alias for INT8 */
#define FSS_TYPE_INT16                0x02  /* Signed 16-bit integer */
#define FSS_TYPE_INT32                0x03  /* Signed 32-bit integer */
#define FSS_TYPE_INT64                0x04  /* Signed 64-bit integer */
#define FSS_TYPE_FLOAT32              0x05  /* IEEE 754 single-precision float (4 bytes) */
#define FSS_TYPE_FLOAT64              0x06  /* IEEE 754 double-precision float (8 bytes) */
#define FSS_TYPE_FIXED_BLOB           0x07  /* Fixed-length binary blob (byte_width bytes) */
#define FSS_TYPE_TIMESTAMP_US         0x08  /* Signed 64-bit microsecond Unix timestamp */
#define FSS_TYPE_VAR_REF              0x09  /* Variable reference (2B offset + 2B length) */
#define FSS_TYPE_TEXT                 0x0A  /* Dynamic text string (for mismatch/demotion) */

/*
** Return / Status Codes.
*/
#define FSS_OK                        0   /* Operation succeeded */
#define FSS_ERROR                     1   /* Generic internal error */
#define FSS_FULL                      2   /* Page capacity exceeded, split required */
#define FSS_NOTFOUND                  3   /* Rowid not found */
#define FSS_DUPLICATE                 4   /* Duplicate rowid attempted */
#define FSS_CORRUPT                   5   /* Corrupt page data */
#define FSS_TYPE_MISMATCH             6   /* Value does not conform to page schema */
#define FSS_UNDERFLOW                 7   /* cell_count reduced to 0; chunk must be reclaimed */
#define FSS_DEMOTED_IN_PLACE          8   /* Page morphed in-place from 0x0E to standard 0x0D */
#define FSS_SPLIT_REQUIRED            9   /* Mismatched row requires allocating new node / split */

/*
** Comparison operators for scan/filter functions.
*/
#define FSS_OP_EQ                     1   /* Equal (==) */
#define FSS_OP_NE                     2   /* Not equal (!=) */
#define FSS_OP_LT                     3   /* Less than (<) */
#define FSS_OP_LE                     4   /* Less than or equal (<=) */
#define FSS_OP_GT                     5   /* Greater than (>) */
#define FSS_OP_GE                     6   /* Greater than or equal (>=) */

/*
** Header and layout constants.
*/
#define FSS_HEADER_SIZE               16
#define FSS_SCHEMA_HEADER_SIZE        2
#define FSS_FIELD_DESC_SIZE           4
#define FSS_MAX_COLUMNS               256
#define FSS_SCHEMA_VERSION            1

/*
** In-Page Field Descriptor (4 bytes on disk).
*/
typedef struct FssFieldDesc {
  uint8_t type_id;     /* Data type code (FSS_TYPE_*) */
  uint8_t col_flags;   /* Column flags (FSS_COL_FLAG_*) */
  uint16_t byte_width; /* Byte size of this column (e.g. 1, 2, 4, 8, K) */
} FssFieldDesc;

/*
** Parsed Schema representation.
*/
typedef struct FssSchema {
  uint16_t num_columns;
  uint16_t row_payload_size;
  uint16_t null_bytes;
  uint16_t col_offsets[FSS_MAX_COLUMNS];  /* Cached relative offset within row payload */
  FssFieldDesc cols[FSS_MAX_COLUMNS];
} FssSchema;

/*
** Decoded FSS Page Header (16 bytes).
*/
typedef struct FssHeader {
  uint8_t page_type;        /* 0x0E */
  uint8_t flags;            /* FSS_FLAG_* */
  uint16_t schema_version;  /* Generation / version ID */
  uint16_t cell_count;      /* Active rows (1 <= cell_count <= capacity) */
  uint16_t capacity;        /* Max rows page can hold */
  uint16_t row_payload_size;/* Fixed byte length of row payload */
  uint16_t field_desc_offset;/* Offset to Field Type Descriptor (default 0x0010) */
  uint16_t data_area_offset;/* Offset where data rows begin */
  uint16_t reserved;        /* 2 bytes reserved */
} FssHeader;

/*
** Value representation for inserting, querying, and checking types.
*/
typedef struct FssValue {
  uint8_t type;     /* FSS_TYPE_* */
  uint8_t is_null;  /* 1 if NULL, 0 otherwise */
  uint16_t n_bytes; /* Byte length for BLOB or TEXT */
  union {
    int64_t i;
    double r;
    float f;
    const void *p;
  } u;
} FssValue;

/*
** In-memory handle representing an FSS page.
*/
typedef struct FssPage {
  uint8_t *aData;           /* Pointer to raw page buffer */
  uint32_t pageSize;        /* Total page size (e.g., 4096) */
  uint16_t hdrOffset;       /* 0 for page > 1, 100 for page 1 */
  FssHeader hdr;            /* Decoded page header */
  FssSchema schema;         /* Decoded schema definition */
  int64_t min_rowid;        /* For dense sequential mode */
  uint8_t *aRowIndex;       /* Pointer to row index area in page */
  uint8_t *aDataArea;       /* Pointer to data rows in page */
} FssPage;

/*
=========================================================================
** API FUNCTIONS
=========================================================================
*/

/*
** Check if the page is an FSS page (page_type == 0x0E).
*/
int fssIsFssPage(const uint8_t *aData, uint16_t hdrOffset);

/*
** Calculate fixed row payload size and null bitmask bytes for a given schema.
*/
int fssCalculateRowSize(
  const FssFieldDesc *pCols,
  uint16_t nCols,
  uint8_t flags,
  uint16_t *pPayloadSize,
  uint16_t *pNullBytes
);

/*
** Calculate maximum row capacity for a page of size pageSize.
*/
uint16_t fssCalculateCapacity(
  uint32_t pageSize,
  uint16_t hdrOffset,
  uint16_t nCols,
  uint16_t rowPayloadSize,
  uint8_t flags
);

/*
** Initialize a schema structure from column descriptors.
*/
int fssSchemaInit(
  FssSchema *pSchema,
  const FssFieldDesc *pCols,
  uint16_t nCols,
  uint8_t flags
);

/*
** Initialize an FSS page with the schema and the initial row (N=1).
** Invariant: An FSS chunk is never instantiated with 0 rows.
*/
int fssPageInit(
  uint8_t *aData,
  uint32_t pageSize,
  uint16_t hdrOffset,
  const FssFieldDesc *pCols,
  uint16_t nCols,
  uint8_t flags,
  int64_t initial_rowid,
  const FssValue *pInitialValues
);

/*
** Parse an existing on-disk FSS page into an FssPage handle.
*/
int fssPageParse(
  uint8_t *aData,
  uint32_t pageSize,
  uint16_t hdrOffset,
  FssPage *pPage
);

/*
** Verify whether a tuple conforms to the page schema.
** Returns FSS_OK if conforming, or FSS_TYPE_MISMATCH if type/width/nullability mismatch.
*/
int fssCheckRowMatchesSchema(
  const FssSchema *pSchema,
  uint8_t pageFlags,
  const FssValue *pValues,
  uint16_t nValues
);

/*
** Insert a new row into the page.
** Returns FSS_OK on success,
**         FSS_FULL if page capacity is reached,
**         FSS_DUPLICATE if rowid already exists,
**         FSS_TYPE_MISMATCH if values do not conform to schema.
*/
int fssPageInsert(
  FssPage *pPage,
  int64_t rowid,
  const FssValue *pValues,
  uint16_t nValues
);

/*
** Find a row by rowid.
** If found, returns FSS_OK and writes the slot index (0..cell_count-1) to *pSlotIndex.
** If not found, returns FSS_NOTFOUND (and writes insertion index to *pSlotIndex).
*/
int fssPageFindRow(
  const FssPage *pPage,
  int64_t rowid,
  uint16_t *pSlotIndex
);

/*
** Direct O(1) column value extraction.
*/
int fssPageGetColumn(
  const FssPage *pPage,
  uint16_t slotIndex,
  uint16_t colIndex,
  FssValue *pOutVal
);

/*
** Read an entire row at the given slot index.
*/
int fssPageGetRow(
  const FssPage *pPage,
  uint16_t slotIndex,
  int64_t *pOutRowid,
  FssValue *pOutValues,
  uint16_t maxValues
);

/*
** Update an individual column value in an existing row slot.
*/
int fssPageUpdateColumn(
  FssPage *pPage,
  uint16_t slotIndex,
  uint16_t colIndex,
  const FssValue *pVal
);

/*
** Delete a row by rowid.
** If cell_count > 1, removes the slot, decrements cell_count, returns FSS_OK.
** If cell_count == 1, returns FSS_UNDERFLOW (chunk teardown: page must be reclaimed).
*/
int fssPageDelete(
  FssPage *pPage,
  int64_t rowid
);

/*
** Fast contiguous table scan with predicate filtering.
** Matches column value against pTarget using op (FSS_OP_*).
** Writes matching slot indices to aMatchingSlots array up to maxMatches.
*/
int fssPageScanFilter(
  const FssPage *pPage,
  uint16_t colIndex,
  int op,
  const FssValue *pTarget,
  uint16_t *aMatchingSlots,
  uint16_t maxMatches,
  uint16_t *pnMatches
);

/*
** Calculate total byte size required to represent all current FSS rows
** plus an optional mismatched row as standard SQLite 0x0D records.
*/
int fssCalculateDemotedSize(
  const FssPage *pPage,
  int64_t mismatched_rowid,
  const FssValue *pMismatchedValues,
  uint16_t nMismatchValues,
  uint32_t *pnBytesNeeded
);

/*
** In-Place Page Demotion (0x0E -> 0x0D):
** Morphs the page in-place into standard SQLite dynamic table leaf format (0x0D).
** If total size needed <= pageSize, performs conversion in-place and returns FSS_OK.
** If total size exceeds pageSize, returns FSS_SPLIT_REQUIRED.
*/
int fssPageDemoteToDynamic(
  FssPage *pPage,
  int64_t mismatched_rowid,
  const FssValue *pMismatchedValues,
  uint16_t nMismatchValues
);

/*
** Helper constructors for FssValue.
*/
FssValue fssValueInt(int64_t val);
FssValue fssValueFloat(double val);
FssValue fssValueBlob(const void *p, uint16_t len);
FssValue fssValueText(const char *text);
FssValue fssValueNull(void);

/*
** SQLite Varint utilities.
*/
int fssPutVarint(uint8_t *p, uint64_t v);
int fssGetVarint(const uint8_t *p, uint64_t *pv);
int fssVarintLen(uint64_t v);

#ifdef __cplusplus
}
#endif

#endif /* SQLITE_FSS_H */
