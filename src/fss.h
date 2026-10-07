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
** Wire layout is defined below and in DESIGN.md. All multi-byte FSS header,
** descriptor, rowid-index, and numeric fields are stored big-endian. Offsets
** in FssHeader are relative to hdrOffset, not to the start of the DB file.
*/
#ifndef SQLITE_FSS_H
#define SQLITE_FSS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FSS_API
# if defined(__GNUC__) || defined(__clang__)
#  define FSS_API __attribute__((weak))
# else
#  define FSS_API
# endif
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
#define FSS_FLAG_NULLABLE             0x02  /* Every slot has a null bitmask */
#define FSS_FLAG_VAR_ARENA            0x04  /* Reserved; unsupported by format version 1 */

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
#define FSS_TYPE_TEXT                 0x0A  /* Dynamic type for fallback/demotion; not an FSS v1 slot */

/*
** Return / Status Codes.
** FSS_ERROR means invalid API arguments or allocation failure; FSS_CORRUPT
** means malformed on-disk input; FSS_TYPE_MISMATCH means the value is not
** supported by the schema/value representation; FSS_FULL means insertion or
** repacking cannot fit; FSS_SPLIT_REQUIRED means a dynamic SQLite-page
** fallback cannot fit in place. FSS_UNDERFLOW asks the caller to reclaim the
** last-row page.
*/
#define FSS_OK                        0   /* Operation succeeded */
#define FSS_ERROR                     1   /* Generic internal error */
#define FSS_FULL                      2   /* Page/repack capacity exceeded; caller must rebalance or fall back */
#define FSS_NOTFOUND                  3   /* Rowid not found */
#define FSS_DUPLICATE                 4   /* Duplicate rowid attempted */
#define FSS_CORRUPT                   5   /* Corrupt page data */
#define FSS_TYPE_MISMATCH             6   /* Value/type is unsupported or does not conform to page schema */
#define FSS_UNDERFLOW                 7   /* cell_count reduced to 0; chunk must be reclaimed */
#define FSS_DEMOTED_IN_PLACE          8   /* Page morphed in-place from 0x0E to standard 0x0D */
#define FSS_SPLIT_REQUIRED            9   /* Dynamic fallback does not fit; caller must split/rebalance */

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
** Database Compatibility Marker constants (bytes 72-79 of SQLite database header).
*/
#define FSS_DB_MARKER_OFFSET          72
#define FSS_DB_MARKER_SIZE            8
#define FSS_DB_MAGIC                  "FSS1"
#define FSS_DB_MAGIC_SIZE             4
#define FSS_DB_VERSION                1

/*
** Status codes for DB marker validation.
*/
#define FSS_MARKER_NONE               0   /* Standard SQLite database (no FSS marker) */
#define FSS_MARKER_VALID              1   /* Valid FSS marker matching this build */
#define FSS_MARKER_UNSUPPORTED_VER    2   /* Recognized FSS magic but unsupported version */
#define FSS_MARKER_INVALID            3   /* Non-zero unknown/corrupt marker */

/* Byte offsets within the 16-byte FSS page header. */
#define FSS_HDR_PAGE_TYPE             0
#define FSS_HDR_FLAGS                 1
#define FSS_HDR_SCHEMA_VERSION        2
#define FSS_HDR_CELL_COUNT            4
#define FSS_HDR_CAPACITY              6
#define FSS_HDR_ROW_PAYLOAD_SIZE      8
#define FSS_HDR_FIELD_DESC_OFFSET     10
#define FSS_HDR_DATA_AREA_OFFSET      12
#define FSS_HDR_RESERVED              14

/* PageSchemaHeader offsets relative to field_desc_offset. */
#define FSS_SCHEMA_NUM_COLUMNS        0
#define FSS_SCHEMA_COLUMNS            2

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
  uint16_t field_desc_offset;/* Offset to descriptors, relative to hdrOffset. */
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
  uint8_t *aRowIndex;       /* Dense min_rowid (8B) or sorted sparse rowids (8B each). */
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
FSS_API 
int fssIsFssPage(const uint8_t *aData, uint16_t hdrOffset);

/*
** Calculate fixed row payload size and null bitmask bytes for a given schema.
*/
FSS_API 
int fssCalculateRowSize(
  const FssFieldDesc *pCols,
  uint16_t nCols,
  uint8_t flags,
  uint16_t *pPayloadSize,
  uint16_t *pNullBytes
);

/*
** Calculate maximum row capacity for a page of usable size pageSize.
** Returns zero for invalid inputs or when no row fits.
*/
FSS_API 
uint16_t fssCalculateCapacity(
  uint32_t pageSize,              /* Usable bytes in the page (excluding reserved tail) */
  uint16_t hdrOffset,
  uint16_t nCols,
  uint16_t rowPayloadSize,
  uint8_t flags
);

/*
** Initialize a schema structure from column descriptors.
*/
FSS_API 
int fssSchemaInit(
  FssSchema *pSchema,
  const FssFieldDesc *pCols,
  uint16_t nCols,
  uint8_t flags
);

/*
** Serialize a tuple of FssValue into a fixed-width slot.
*/
FSS_API 
void fssWriteSlot(
  uint8_t *pSlot,
  const FssSchema *pSchema,
  uint8_t flags,
  const FssValue *pValues
);

/*
** Direct conversion from raw SQLite record bytes into a fixed-width slot.
** Validates types, widths, and NOT NULL constraints in a single pass.
** Returns FSS_OK on success,
**         FSS_CORRUPT if record payload is malformed,
**         FSS_TYPE_MISMATCH if values do not conform to schema.
*/
FSS_API 
int fssRecordToSlot(
  const uint8_t *pRecord,
  uint32_t nRecord,
  const FssSchema *pSchema,
  uint8_t pageFlags,
  uint8_t *pSlot
);

/*
** Initialize an FSS page with the schema and the initial row (N=1).
** Invariant: An FSS chunk is never instantiated with 0 rows.
*/
FSS_API 
int fssPageInit(
  uint8_t *aData,
  uint32_t pageSize,              /* Usable bytes in the page (excluding reserved tail) */
  uint16_t hdrOffset,
  const FssFieldDesc *pCols,
  uint16_t nCols,
  uint8_t flags,
  int64_t initial_rowid,
  const FssValue *pInitialValues
);

/*
** Initialize an FSS page with the schema and the initial row directly from SQLite record bytes.
*/
FSS_API 
int fssPageInitRecord(
  uint8_t *aData,
  uint32_t pageSize,
  uint16_t hdrOffset,
  const FssFieldDesc *pCols,
  uint16_t nCols,
  uint8_t flags,
  int64_t initial_rowid,
  const uint8_t *pRecord,
  uint32_t nRecord
);

/*
** Parse an existing on-disk FSS page into an FssPage handle.
*/
FSS_API 
int fssPageParse(
  uint8_t *aData,
  uint32_t pageSize,              /* Usable bytes in the page (excluding reserved tail) */
  uint16_t hdrOffset,
  FssPage *pPage
);

/*
** Verify whether a tuple conforms to the page schema.
** Returns FSS_OK if conforming, or FSS_TYPE_MISMATCH if type/width/nullability mismatch.
*/
FSS_API 
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
FSS_API 
int fssPageInsert(
  FssPage *pPage,
  int64_t rowid,
  const FssValue *pValues,
  uint16_t nValues
);

/*
** Direct insert from raw SQLite record bytes into an FSS page.
** Avoids intermediate FssValue unpacking and performs single-pass write.
** If pOutSlot is non-NULL, stores the target slot index where row was placed.
*/
FSS_API 
int fssPageInsertRecord(
  FssPage *pPage,
  int64_t rowid,
  const uint8_t *pRecord,
  uint32_t nRecord,
  uint16_t *pOutSlot
);

/*
** Fast schema extraction directly from the page header without full page validation.
*/
FSS_API 
int fssFastSchemaExtract(const uint8_t *pHdr, FssSchema *pSchema);

/*
** Lightweight parse of an initialized FSS page without deep structural checks.
*/
FSS_API 
int fssPageQuickParse(
  uint8_t *aData,
  uint32_t pageSize,
  uint16_t hdrOffset,
  FssPage *pPage
);

/*
** Find a row by rowid.
** If found, returns FSS_OK and writes the slot index (0..cell_count-1) to *pSlotIndex.
** If not found, returns FSS_NOTFOUND (and writes insertion index to *pSlotIndex).
*/
FSS_API 
int fssPageFindRow(
  const FssPage *pPage,
  int64_t rowid,
  uint16_t *pSlotIndex
);

/*
** Direct O(1) column value extraction.
*/
FSS_API 
int fssPageGetColumn(
  const FssPage *pPage,
  uint16_t slotIndex,
  uint16_t colIndex,
  FssValue *pOutVal
);

/*
** Read an entire row at the given slot index.
*/
FSS_API 
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
FSS_API 
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
FSS_API 
int fssPageDelete(
  FssPage *pPage,
  int64_t rowid
);

/*
** Fast contiguous table scan with predicate filtering.
** Matches column value against pTarget using op (FSS_OP_*).
** Writes matching slot indices to aMatchingSlots array up to maxMatches.
*/
FSS_API 
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
FSS_API 
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
** If total size exceeds the usable page region, returns FSS_SPLIT_REQUIRED.
*/
FSS_API 
int fssPageDemoteToDynamic(
  FssPage *pPage,
  int64_t mismatched_rowid,
  const FssValue *pMismatchedValues,
  uint16_t nMismatchValues
);

/*
** Helper constructors for FssValue.
*/
FSS_API 
FssValue fssValueInt(int64_t val);
FSS_API 
FssValue fssValueFloat(double val);
FSS_API 
FssValue fssValueBlob(const void *p, uint16_t len);
FSS_API 
FssValue fssValueText(const char *text);
FSS_API 
FssValue fssValueNull(void);

/*
** Big-Endian integer serialization utilities.
*/
static inline uint16_t fssGetU16(const uint8_t *p) {
  return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline void fssPutU16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xff);
}

static inline uint32_t fssGetU32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) |
         ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8)  |
         ((uint32_t)p[3]);
}

static inline void fssPutU32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)(v & 0xff);
}

static inline int64_t fssGetI64(const uint8_t *p) {
  uint64_t v = 0;
  for( int i = 0; i < 8; i++ ){
    v = (v << 8) | (uint64_t)p[i];
  }
  if( v <= INT64_MAX ) return (int64_t)v;
  return -1 - (int64_t)(~v);
}

static inline void fssPutI64(uint8_t *p, int64_t v) {
  uint64_t uv = (uint64_t)v;
  for( int i = 7; i >= 0; i-- ){
    p[i] = (uint8_t)(uv & 0xff);
    uv >>= 8;
  }
}

/*
** SQLite Varint utilities.
*/
FSS_API 
int fssPutVarint(uint8_t *p, uint64_t v);
FSS_API 
int fssGetVarint(const uint8_t *p, uint64_t *pv);
FSS_API 
int fssVarintLen(uint64_t v);

/*
** Decode an SQLite record payload (without the outer table-cell rowid).
** Text/blob FssValue pointers refer into pRecord and remain valid only while
** that input buffer remains valid. Returns FSS_FULL if maxValues is too small,
** FSS_CORRUPT for malformed records, and FSS_TYPE_MISMATCH for values larger
** than the FSS value representation can hold.
*/
FSS_API 
int fssRecordToValues(
  const uint8_t *pRecord,
  uint32_t nRecord,
  FssValue *aValues,
  uint16_t maxValues,
  uint16_t *pnValues
);

/*
** Encode values as a complete SQLite table cell, including the rowid varint.
** If pCell is NULL, report the required size in pnCell without copying.
*/
FSS_API 
int fssValuesToSqliteCell(
  int64_t rowid,
  const FssValue *aValues,
  uint16_t nValues,
  uint8_t *pCell,
  uint32_t cellCapacity,
  uint32_t *pnCell
);

/*
** Database Compatibility Marker API functions.
*/
FSS_API 
int fssValidateDbMarker(const uint8_t *aDbHdr, uint32_t *pVersion);
FSS_API 
int fssWriteDbMarker(uint8_t *aDbHdr, uint32_t version);
FSS_API 
int fssClearDbMarker(uint8_t *aDbHdr);
FSS_API 
int fssHasDbMarker(const uint8_t *aDbHdr);

#ifdef __cplusplus
}
#endif

#endif /* SQLITE_FSS_H */
