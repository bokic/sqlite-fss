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
** Implementation of the Fixed-Schema Storage (0x0E) leaf page format.
** Defined in DESIGN.md.
*/
#include "fss.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>

/*
=========================================================================
** ENDIANNESS AND WIRE-FORMAT HELPERS (BIG-ENDIAN)
=========================================================================
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
  return (int64_t)v;
}

static inline void fssPutI64(uint8_t *p, int64_t v) {
  uint64_t uv = (uint64_t)v;
  for( int i = 7; i >= 0; i-- ){
    p[i] = (uint8_t)(uv & 0xff);
    uv >>= 8;
  }
}

static inline float fssGetF32(const uint8_t *p) {
  uint32_t u = fssGetU32(p);
  float f;
  memcpy(&f, &u, sizeof(f));
  return f;
}

static inline void fssPutF32(uint8_t *p, float f) {
  uint32_t u;
  memcpy(&u, &f, sizeof(u));
  fssPutU32(p, u);
}

static inline double fssGetF64(const uint8_t *p) {
  uint64_t u = (uint64_t)fssGetI64(p);
  double d;
  memcpy(&d, &u, sizeof(d));
  return d;
}

static inline void fssPutF64(uint8_t *p, double d) {
  uint64_t u;
  memcpy(&u, &d, sizeof(u));
  fssPutI64(p, (int64_t)u);
}

/*
=========================================================================
** SQLITE VARIABLE-LENGTH INTEGER (VARINT) IMPLEMENTATION
=========================================================================
*/

int fssPutVarint(uint8_t *p, uint64_t v) {
  int i, j, n;
  uint8_t buf[10];
  if( v <= 0x7f ){
    p[0] = (uint8_t)v;
    return 1;
  }
  if( v & (((uint64_t)0xff000000) << 32) ){
    p[8] = (uint8_t)v;
    v >>= 8;
    for( i = 7; i >= 0; i-- ){
      p[i] = (uint8_t)((v & 0x7f) | 0x80);
      v >>= 7;
    }
    return 9;
  }
  n = 0;
  buf[n++] = (uint8_t)(v & 0x7f);
  v >>= 7;
  while( v != 0 ){
    buf[n++] = (uint8_t)((v & 0x7f) | 0x80);
    v >>= 7;
  }
  for( i = 0, j = n - 1; j >= 0; i++, j-- ){
    p[i] = buf[j];
  }
  return n;
}

int fssGetVarint(const uint8_t *p, uint64_t *pv) {
  uint32_t a, b;
  a = *p;
  if( !(a & 0x80) ){
    *pv = a;
    return 1;
  }
  p++;
  b = *p;
  if( !(b & 0x80) ){
    a = (a & 0x7f) << 7 | b;
    *pv = a;
    return 2;
  }
  p++;
  a = a << 14 | (b & 0x7f) << 7 | *p;
  if( !(a & 0x80) ){
    a &= (0x7f << 14) | (0x7f << 7) | 0x7f;
    *pv = a;
    return 3;
  }
  /* Fallback for full 64-bit varint up to 9 bytes */
  {
    uint64_t v = 0;
    int i;
    for( i = 0; i < 8; i++ ){
      v = (v << 7) | (*p & 0x7f);
      if( !(*p & 0x80) ){
        *pv = v;
        return i + 1;
      }
      p++;
    }
    v = (v << 8) | *p;
    *pv = v;
    return 9;
  }
}

int fssVarintLen(uint64_t v) {
  uint8_t buf[10];
  return fssPutVarint(buf, v);
}

/*
=========================================================================
** VALUE HELPERS
=========================================================================
*/

FssValue fssValueInt(int64_t val) {
  FssValue v;
  memset(&v, 0, sizeof(v));
  v.type = FSS_TYPE_INT64;
  v.is_null = 0;
  v.n_bytes = 8;
  v.u.i = val;
  return v;
}

FssValue fssValueFloat(double val) {
  FssValue v;
  memset(&v, 0, sizeof(v));
  v.type = FSS_TYPE_FLOAT64;
  v.is_null = 0;
  v.n_bytes = 8;
  v.u.r = val;
  return v;
}

FssValue fssValueBlob(const void *p, uint16_t len) {
  FssValue v;
  memset(&v, 0, sizeof(v));
  v.type = FSS_TYPE_FIXED_BLOB;
  v.is_null = 0;
  v.n_bytes = len;
  v.u.p = p;
  return v;
}

FssValue fssValueText(const char *text) {
  FssValue v;
  memset(&v, 0, sizeof(v));
  v.type = FSS_TYPE_TEXT;
  v.is_null = (text == NULL);
  v.n_bytes = text ? (uint16_t)strlen(text) : 0;
  v.u.p = text;
  return v;
}

FssValue fssValueNull(void) {
  FssValue v;
  memset(&v, 0, sizeof(v));
  v.type = FSS_TYPE_INT8;
  v.is_null = 1;
  return v;
}

/*
=========================================================================
** SCHEMA AND CAPACITY CALCULATIONS
=========================================================================
*/

int fssCalculateRowSize(
  const FssFieldDesc *pCols,
  uint16_t nCols,
  uint8_t flags,
  uint16_t *pPayloadSize,
  uint16_t *pNullBytes
) {
  uint16_t nullBytes = 0;
  uint32_t dataBytes = 0;

  if( pCols == NULL || nCols == 0 || nCols > FSS_MAX_COLUMNS ){
    return FSS_ERROR;
  }

  if( flags & FSS_FLAG_NULLABLE ){
    nullBytes = (nCols + 7) / 8;
  }

  for( uint16_t i = 0; i < nCols; i++ ){
    uint16_t w = pCols[i].byte_width;
    switch( pCols[i].type_id ){
      case FSS_TYPE_INT8:
        if( w == 0 ) w = 1;
        break;
      case FSS_TYPE_INT16:
        if( w == 0 ) w = 2;
        break;
      case FSS_TYPE_INT32:
      case FSS_TYPE_FLOAT32:
        if( w == 0 ) w = 4;
        break;
      case FSS_TYPE_INT64:
      case FSS_TYPE_FLOAT64:
      case FSS_TYPE_TIMESTAMP_US:
        if( w == 0 ) w = 8;
        break;
      case FSS_TYPE_VAR_REF:
        if( w == 0 ) w = 4;
        break;
      case FSS_TYPE_FIXED_BLOB:
        if( w == 0 ) return FSS_ERROR;
        break;
      default:
        break;
    }
    dataBytes += w;
  }

  if( pNullBytes ) *pNullBytes = nullBytes;
  if( pPayloadSize ) *pPayloadSize = (uint16_t)(nullBytes + dataBytes);
  return FSS_OK;
}

uint16_t fssCalculateCapacity(
  uint32_t pageSize,
  uint16_t hdrOffset,
  uint16_t nCols,
  uint16_t rowPayloadSize,
  uint8_t flags
) {
  uint32_t descSize;
  uint32_t fixedHeaderAndDesc;
  uint32_t avail;

  if( rowPayloadSize == 0 ) return 0;

  descSize = FSS_SCHEMA_HEADER_SIZE + (uint32_t)nCols * FSS_FIELD_DESC_SIZE;
  fixedHeaderAndDesc = FSS_HEADER_SIZE + descSize;

  if( pageSize <= (uint32_t)hdrOffset + fixedHeaderAndDesc ){
    return 0;
  }

  avail = pageSize - hdrOffset - fixedHeaderAndDesc;

  if( flags & FSS_FLAG_DENSE_ROWID ){
    /* Dense sequential mode: index section is simply 8-byte min_rowid */
    if( avail <= 8 ) return 0;
    avail -= 8;
    return (uint16_t)(avail / rowPayloadSize);
  }else{
    /* General / sparse mode: each slot requires 8 bytes rowid + rowPayloadSize */
    uint32_t slotTotal = 8 + (uint32_t)rowPayloadSize;
    return (uint16_t)(avail / slotTotal);
  }
}

int fssSchemaInit(
  FssSchema *pSchema,
  const FssFieldDesc *pCols,
  uint16_t nCols,
  uint8_t flags
) {
  uint16_t offset;
  int rc;

  if( pSchema == NULL || pCols == NULL || nCols == 0 || nCols > FSS_MAX_COLUMNS ){
    return FSS_ERROR;
  }

  memset(pSchema, 0, sizeof(*pSchema));
  pSchema->num_columns = nCols;

  rc = fssCalculateRowSize(pCols, nCols, flags, &pSchema->row_payload_size, &pSchema->null_bytes);
  if( rc != FSS_OK ) return rc;

  offset = pSchema->null_bytes;
  for( uint16_t i = 0; i < nCols; i++ ){
    pSchema->cols[i] = pCols[i];
    /* Normalise standard widths */
    switch( pCols[i].type_id ){
      case FSS_TYPE_INT8:
        if( pSchema->cols[i].byte_width == 0 ) pSchema->cols[i].byte_width = 1;
        break;
      case FSS_TYPE_INT16:
        if( pSchema->cols[i].byte_width == 0 ) pSchema->cols[i].byte_width = 2;
        break;
      case FSS_TYPE_INT32:
      case FSS_TYPE_FLOAT32:
        if( pSchema->cols[i].byte_width == 0 ) pSchema->cols[i].byte_width = 4;
        break;
      case FSS_TYPE_INT64:
      case FSS_TYPE_FLOAT64:
      case FSS_TYPE_TIMESTAMP_US:
        if( pSchema->cols[i].byte_width == 0 ) pSchema->cols[i].byte_width = 8;
        break;
      case FSS_TYPE_VAR_REF:
        if( pSchema->cols[i].byte_width == 0 ) pSchema->cols[i].byte_width = 4;
        break;
      default:
        break;
    }
    pSchema->col_offsets[i] = offset;
    offset += pSchema->cols[i].byte_width;
  }

  return FSS_OK;
}

int fssIsFssPage(const uint8_t *aData, uint16_t hdrOffset) {
  if( aData == NULL ) return 0;
  return aData[hdrOffset] == FSS_PAGE_TYPE;
}

/*
=========================================================================
** SERIALIZATION & DESERIALIZATION OF INDIVIDUAL ROW SLOTS
=========================================================================
*/

static void fssWriteSlot(
  uint8_t *pSlot,
  const FssSchema *pSchema,
  uint8_t pageFlags,
  const FssValue *pValues
) {
  memset(pSlot, 0, pSchema->row_payload_size);

  if( pageFlags & FSS_FLAG_NULLABLE ){
    for( uint16_t i = 0; i < pSchema->num_columns; i++ ){
      if( pValues[i].is_null ){
        pSlot[i / 8] |= (uint8_t)(1 << (i % 8));
      }
    }
  }

  for( uint16_t i = 0; i < pSchema->num_columns; i++ ){
    if( pValues[i].is_null ) continue;
    uint8_t *pDest = pSlot + pSchema->col_offsets[i];
    const FssValue *v = &pValues[i];

    switch( pSchema->cols[i].type_id ){
      case FSS_TYPE_INT8:
        *pDest = (uint8_t)(v->u.i & 0xff);
        break;
      case FSS_TYPE_INT16:
        fssPutU16(pDest, (uint16_t)v->u.i);
        break;
      case FSS_TYPE_INT32:
        fssPutU32(pDest, (uint32_t)v->u.i);
        break;
      case FSS_TYPE_INT64:
      case FSS_TYPE_TIMESTAMP_US:
        fssPutI64(pDest, v->u.i);
        break;
      case FSS_TYPE_FLOAT32:
        if( v->type == FSS_TYPE_FLOAT64 ){
          fssPutF32(pDest, (float)v->u.r);
        }else if( v->type == FSS_TYPE_INT64 ){
          fssPutF32(pDest, (float)v->u.i);
        }else{
          fssPutF32(pDest, v->u.f);
        }
        break;
      case FSS_TYPE_FLOAT64:
        if( v->type == FSS_TYPE_INT64 ){
          fssPutF64(pDest, (double)v->u.i);
        }else{
          fssPutF64(pDest, v->u.r);
        }
        break;
      case FSS_TYPE_FIXED_BLOB:
        if( v->u.p != NULL ){
          uint16_t copyLen = v->n_bytes < pSchema->cols[i].byte_width ? v->n_bytes : pSchema->cols[i].byte_width;
          memcpy(pDest, v->u.p, copyLen);
        }
        break;
      case FSS_TYPE_VAR_REF:
        /* Store 2-byte offset and 2-byte length */
        fssPutU16(pDest, (uint16_t)(v->u.i >> 16));
        fssPutU16(pDest + 2, (uint16_t)(v->u.i & 0xffff));
        break;
      default:
        break;
    }
  }
}

static void fssReadSlot(
  const uint8_t *pSlot,
  const FssSchema *pSchema,
  uint8_t pageFlags,
  uint16_t colIndex,
  FssValue *pOutVal
) {
  memset(pOutVal, 0, sizeof(*pOutVal));
  pOutVal->type = pSchema->cols[colIndex].type_id;

  if( pageFlags & FSS_FLAG_NULLABLE ){
    if( pSlot[colIndex / 8] & (1 << (colIndex % 8)) ){
      pOutVal->is_null = 1;
      return;
    }
  }

  pOutVal->is_null = 0;
  const uint8_t *pSrc = pSlot + pSchema->col_offsets[colIndex];

  switch( pSchema->cols[colIndex].type_id ){
    case FSS_TYPE_INT8:
      pOutVal->u.i = (int8_t)(*pSrc);
      pOutVal->n_bytes = 1;
      break;
    case FSS_TYPE_INT16:
      pOutVal->u.i = (int16_t)fssGetU16(pSrc);
      pOutVal->n_bytes = 2;
      break;
    case FSS_TYPE_INT32:
      pOutVal->u.i = (int32_t)fssGetU32(pSrc);
      pOutVal->n_bytes = 4;
      break;
    case FSS_TYPE_INT64:
    case FSS_TYPE_TIMESTAMP_US:
      pOutVal->u.i = fssGetI64(pSrc);
      pOutVal->n_bytes = 8;
      break;
    case FSS_TYPE_FLOAT32:
      pOutVal->u.f = fssGetF32(pSrc);
      pOutVal->u.r = (double)pOutVal->u.f;
      pOutVal->n_bytes = 4;
      break;
    case FSS_TYPE_FLOAT64:
      pOutVal->u.r = fssGetF64(pSrc);
      pOutVal->n_bytes = 8;
      break;
    case FSS_TYPE_FIXED_BLOB:
      pOutVal->u.p = pSrc;
      pOutVal->n_bytes = pSchema->cols[colIndex].byte_width;
      break;
    case FSS_TYPE_VAR_REF:
      pOutVal->u.i = ((int64_t)fssGetU16(pSrc) << 16) | (int64_t)fssGetU16(pSrc + 2);
      pOutVal->n_bytes = 4;
      break;
    default:
      break;
  }
}

/*
=========================================================================
** PAGE INITIALIZATION & PARSING
=========================================================================
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
) {
  FssSchema schema;
  uint16_t capacity;
  uint16_t descSize;
  uint16_t rowIndexOffset;
  uint16_t dataAreaOffset;
  uint8_t *pHdr;
  uint8_t *pDesc;
  uint8_t *pIndex;
  uint8_t *pData;
  int rc;

  if( aData == NULL || pCols == NULL || pInitialValues == NULL || nCols == 0 ){
    return FSS_ERROR;
  }

  rc = fssSchemaInit(&schema, pCols, nCols, flags);
  if( rc != FSS_OK ) return rc;

  capacity = fssCalculateCapacity(pageSize, hdrOffset, nCols, schema.row_payload_size, flags);
  if( capacity < 1 ){
    return FSS_FULL;
  }

  /* Invariant verification: initial row must conform to schema */
  rc = fssCheckRowMatchesSchema(&schema, flags, pInitialValues, nCols);
  if( rc != FSS_OK ) return rc;

  descSize = FSS_SCHEMA_HEADER_SIZE + nCols * FSS_FIELD_DESC_SIZE;
  rowIndexOffset = FSS_HEADER_SIZE + descSize;

  if( flags & FSS_FLAG_DENSE_ROWID ){
    dataAreaOffset = rowIndexOffset + 8;
  }else{
    dataAreaOffset = rowIndexOffset + 8 * capacity;
  }

  /* Clear page memory */
  memset(aData + hdrOffset, 0, pageSize - hdrOffset);

  pHdr = aData + hdrOffset;

  /*
  ** Section 1: Page Header (16 bytes)
  */
  pHdr[0] = FSS_PAGE_TYPE;
  pHdr[1] = flags;
  fssPutU16(pHdr + 2, FSS_SCHEMA_VERSION);
  fssPutU16(pHdr + 4, 1); /* Strict invariant: instantiated with exactly 1 row */
  fssPutU16(pHdr + 6, capacity);
  fssPutU16(pHdr + 8, schema.row_payload_size);
  fssPutU16(pHdr + 10, FSS_HEADER_SIZE);
  fssPutU16(pHdr + 12, dataAreaOffset);
  fssPutU16(pHdr + 14, 0); /* Reserved */

  /*
  ** Section 2: Field Type Descriptor
  */
  pDesc = pHdr + FSS_HEADER_SIZE;
  fssPutU16(pDesc, nCols);
  for( uint16_t i = 0; i < nCols; i++ ){
    pDesc[2 + i * 4] = schema.cols[i].type_id;
    pDesc[2 + i * 4 + 1] = schema.cols[i].col_flags;
    fssPutU16(pDesc + 2 + i * 4 + 2, schema.cols[i].byte_width);
  }

  /*
  ** Section 3: Row Addressing & Indexing
  */
  pIndex = pHdr + rowIndexOffset;
  fssPutI64(pIndex, initial_rowid);

  /*
  ** Section 4: Data Rows (Initial Row 0)
  */
  pData = pHdr + dataAreaOffset;
  fssWriteSlot(pData, &schema, flags, pInitialValues);

  return FSS_OK;
}

int fssPageParse(
  uint8_t *aData,
  uint32_t pageSize,
  uint16_t hdrOffset,
  FssPage *pPage
) {
  const uint8_t *pHdr;
  const uint8_t *pDesc;
  uint16_t nCols;
  uint16_t descSize;

  if( aData == NULL || pPage == NULL || pageSize < 512 ){
    return FSS_ERROR;
  }

  pHdr = aData + hdrOffset;
  if( pHdr[0] != FSS_PAGE_TYPE ){
    return FSS_CORRUPT;
  }

  memset(pPage, 0, sizeof(*pPage));
  pPage->aData = aData;
  pPage->pageSize = pageSize;
  pPage->hdrOffset = hdrOffset;

  /* Read Header */
  pPage->hdr.page_type = pHdr[0];
  pPage->hdr.flags = pHdr[1];
  pPage->hdr.schema_version = fssGetU16(pHdr + 2);
  pPage->hdr.cell_count = fssGetU16(pHdr + 4);
  pPage->hdr.capacity = fssGetU16(pHdr + 6);
  pPage->hdr.row_payload_size = fssGetU16(pHdr + 8);
  pPage->hdr.field_desc_offset = fssGetU16(pHdr + 10);
  pPage->hdr.data_area_offset = fssGetU16(pHdr + 12);
  pPage->hdr.reserved = fssGetU16(pHdr + 14);

  if( pPage->hdr.cell_count > pPage->hdr.capacity || pPage->hdr.capacity == 0 ){
    return FSS_CORRUPT;
  }

  /* Read Schema */
  pDesc = pHdr + pPage->hdr.field_desc_offset;
  nCols = fssGetU16(pDesc);
  if( nCols == 0 || nCols > FSS_MAX_COLUMNS ){
    return FSS_CORRUPT;
  }

  FssFieldDesc cols[FSS_MAX_COLUMNS];
  for( uint16_t i = 0; i < nCols; i++ ){
    cols[i].type_id = pDesc[2 + i * 4];
    cols[i].col_flags = pDesc[2 + i * 4 + 1];
    cols[i].byte_width = fssGetU16(pDesc + 2 + i * 4 + 2);
  }

  if( fssSchemaInit(&pPage->schema, cols, nCols, pPage->hdr.flags) != FSS_OK ){
    return FSS_CORRUPT;
  }

  descSize = FSS_SCHEMA_HEADER_SIZE + nCols * FSS_FIELD_DESC_SIZE;
  pPage->aRowIndex = aData + hdrOffset + FSS_HEADER_SIZE + descSize;
  pPage->aDataArea = aData + hdrOffset + pPage->hdr.data_area_offset;

  if( pPage->hdr.flags & FSS_FLAG_DENSE_ROWID ){
    pPage->min_rowid = fssGetI64(pPage->aRowIndex);
  }

  return FSS_OK;
}

/*
=========================================================================
** SCHEMA CONFORMANCE & TYPE MISMATCH CHECKING
=========================================================================
*/

int fssCheckRowMatchesSchema(
  const FssSchema *pSchema,
  uint8_t pageFlags,
  const FssValue *pValues,
  uint16_t nValues
) {
  if( pSchema == NULL || pValues == NULL || nValues < pSchema->num_columns ){
    return FSS_TYPE_MISMATCH;
  }

  for( uint16_t i = 0; i < pSchema->num_columns; i++ ){
    const FssValue *v = &pValues[i];
    const FssFieldDesc *col = &pSchema->cols[i];

    if( v->is_null ){
      if( col->col_flags & FSS_COL_FLAG_NOT_NULL ){
        return FSS_TYPE_MISMATCH; /* NOT NULL constraint violated */
      }
      if( !(pageFlags & FSS_FLAG_NULLABLE) ){
        return FSS_TYPE_MISMATCH; /* Page has no null bitmask allocated */
      }
      continue;
    }

    switch( col->type_id ){
      case FSS_TYPE_INT8:
        if( v->type != FSS_TYPE_INT8 && v->type != FSS_TYPE_INT16 &&
            v->type != FSS_TYPE_INT32 && v->type != FSS_TYPE_INT64 ){
          return FSS_TYPE_MISMATCH;
        }
        if( v->u.i < -128 || v->u.i > 127 ){
          return FSS_TYPE_MISMATCH; /* Width overflow */
        }
        break;

      case FSS_TYPE_INT16:
        if( v->type != FSS_TYPE_INT8 && v->type != FSS_TYPE_INT16 &&
            v->type != FSS_TYPE_INT32 && v->type != FSS_TYPE_INT64 ){
          return FSS_TYPE_MISMATCH;
        }
        if( v->u.i < -32768 || v->u.i > 32767 ){
          return FSS_TYPE_MISMATCH; /* Width overflow */
        }
        break;

      case FSS_TYPE_INT32:
        if( v->type != FSS_TYPE_INT8 && v->type != FSS_TYPE_INT16 &&
            v->type != FSS_TYPE_INT32 && v->type != FSS_TYPE_INT64 ){
          return FSS_TYPE_MISMATCH;
        }
        if( v->u.i < (int64_t)INT32_MIN || v->u.i > (int64_t)INT32_MAX ){
          return FSS_TYPE_MISMATCH; /* Width overflow */
        }
        break;

      case FSS_TYPE_INT64:
      case FSS_TYPE_TIMESTAMP_US:
        if( v->type != FSS_TYPE_INT8 && v->type != FSS_TYPE_INT16 &&
            v->type != FSS_TYPE_INT32 && v->type != FSS_TYPE_INT64 &&
            v->type != FSS_TYPE_TIMESTAMP_US ){
          return FSS_TYPE_MISMATCH;
        }
        break;

      case FSS_TYPE_FLOAT32:
      case FSS_TYPE_FLOAT64:
        if( v->type != FSS_TYPE_FLOAT32 && v->type != FSS_TYPE_FLOAT64 &&
            v->type != FSS_TYPE_INT8 && v->type != FSS_TYPE_INT16 &&
            v->type != FSS_TYPE_INT32 && v->type != FSS_TYPE_INT64 ){
          return FSS_TYPE_MISMATCH;
        }
        break;

      case FSS_TYPE_FIXED_BLOB:
        if( v->type != FSS_TYPE_FIXED_BLOB ){
          return FSS_TYPE_MISMATCH;
        }
        if( v->n_bytes != col->byte_width ){
          return FSS_TYPE_MISMATCH; /* Length mismatch */
        }
        break;

      case FSS_TYPE_VAR_REF:
        if( v->type != FSS_TYPE_VAR_REF && v->type != FSS_TYPE_TEXT &&
            v->type != FSS_TYPE_FIXED_BLOB ){
          return FSS_TYPE_MISMATCH;
        }
        break;

      default:
        return FSS_TYPE_MISMATCH;
    }
  }

  return FSS_OK;
}

/*
=========================================================================
** ROW CRUD OPERATIONS
=========================================================================
*/

int fssPageFindRow(
  const FssPage *pPage,
  int64_t rowid,
  uint16_t *pSlotIndex
) {
  if( pPage == NULL || pSlotIndex == NULL ) return FSS_ERROR;

  if( pPage->hdr.cell_count == 0 ){
    *pSlotIndex = 0;
    return FSS_NOTFOUND;
  }

  if( pPage->hdr.flags & FSS_FLAG_DENSE_ROWID ){
    int64_t slot = rowid - pPage->min_rowid;
    if( slot >= 0 && slot < (int64_t)pPage->hdr.cell_count ){
      *pSlotIndex = (uint16_t)slot;
      return FSS_OK;
    }
    *pSlotIndex = (slot < 0) ? 0 : pPage->hdr.cell_count;
    return FSS_NOTFOUND;
  }else{
    /* Binary search in sorted rowid array */
    int low = 0;
    int high = (int)pPage->hdr.cell_count - 1;

    while( low <= high ){
      int mid = low + (high - low) / 2;
      int64_t midRowid = fssGetI64(pPage->aRowIndex + mid * 8);

      if( midRowid == rowid ){
        *pSlotIndex = (uint16_t)mid;
        return FSS_OK;
      }
      if( midRowid < rowid ){
        low = mid + 1;
      }else{
        high = mid - 1;
      }
    }

    *pSlotIndex = (uint16_t)low;
    return FSS_NOTFOUND;
  }
}

int fssPageInsert(
  FssPage *pPage,
  int64_t rowid,
  const FssValue *pValues,
  uint16_t nValues
) {
  uint16_t targetSlot;
  int rc;

  if( pPage == NULL || pValues == NULL ) return FSS_ERROR;

  /* Check capacity */
  if( pPage->hdr.cell_count >= pPage->hdr.capacity ){
    return FSS_FULL;
  }

  /* Check schema match */
  rc = fssCheckRowMatchesSchema(&pPage->schema, pPage->hdr.flags, pValues, nValues);
  if( rc != FSS_OK ){
    return FSS_TYPE_MISMATCH;
  }

  rc = fssPageFindRow(pPage, rowid, &targetSlot);
  if( rc == FSS_OK ){
    return FSS_DUPLICATE;
  }

  uint16_t count = pPage->hdr.cell_count;
  uint16_t rsz = pPage->hdr.row_payload_size;

  if( pPage->hdr.flags & FSS_FLAG_DENSE_ROWID ){
    if( count == 0 ){
      pPage->min_rowid = rowid;
      fssPutI64(pPage->aRowIndex, rowid);
      targetSlot = 0;
    }else if( rowid == pPage->min_rowid + count ){
      targetSlot = count;
    }else if( rowid == pPage->min_rowid - 1 ){
      /* Prepend row */
      memmove(pPage->aDataArea + rsz, pPage->aDataArea, (size_t)count * rsz);
      pPage->min_rowid = rowid;
      fssPutI64(pPage->aRowIndex, rowid);
      targetSlot = 0;
    }else{
      /* Dense mode gap encountered: cannot store non-contiguous rowid in dense mode */
      return FSS_FULL;
    }
  }else{
    /* Sparse mode: shift existing rows right to make room */
    if( targetSlot < count ){
      memmove(pPage->aRowIndex + (targetSlot + 1) * 8,
              pPage->aRowIndex + targetSlot * 8,
              (size_t)(count - targetSlot) * 8);
      memmove(pPage->aDataArea + (targetSlot + 1) * rsz,
              pPage->aDataArea + targetSlot * rsz,
              (size_t)(count - targetSlot) * rsz);
    }
    fssPutI64(pPage->aRowIndex + targetSlot * 8, rowid);
  }

  /* Write row payload into targetSlot */
  fssWriteSlot(pPage->aDataArea + targetSlot * rsz, &pPage->schema, pPage->hdr.flags, pValues);

  /* Increment cell count and update disk header */
  pPage->hdr.cell_count++;
  fssPutU16(pPage->aData + pPage->hdrOffset + 4, pPage->hdr.cell_count);

  return FSS_OK;
}

int fssPageDelete(
  FssPage *pPage,
  int64_t rowid
) {
  uint16_t targetSlot;
  int rc;

  if( pPage == NULL ) return FSS_ERROR;

  /*
  ** Strict minimum invariant:
  ** If cell_count is 1, deleting the row leaves 0 rows.
  ** An FSS chunk must strictly contain 1 or more rows.
  ** The chunk must be reclaimed/freed.
  */
  if( pPage->hdr.cell_count <= 1 ){
    rc = fssPageFindRow(pPage, rowid, &targetSlot);
    if( rc != FSS_OK ) return rc;
    return FSS_UNDERFLOW;
  }

  rc = fssPageFindRow(pPage, rowid, &targetSlot);
  if( rc != FSS_OK ) return rc;

  uint16_t count = pPage->hdr.cell_count;
  uint16_t rsz = pPage->hdr.row_payload_size;

  if( pPage->hdr.flags & FSS_FLAG_DENSE_ROWID ){
    if( targetSlot == 0 ){
      memmove(pPage->aDataArea, pPage->aDataArea + rsz, (size_t)(count - 1) * rsz);
      pPage->min_rowid++;
      fssPutI64(pPage->aRowIndex, pPage->min_rowid);
    }else if( targetSlot == count - 1 ){
      /* Simply truncate trailing slot */
      memset(pPage->aDataArea + (count - 1) * rsz, 0, rsz);
    }else{
      /* Middle deletion in dense mode creates a hole: cannot remain dense */
      return FSS_ERROR;
    }
  }else{
    /* Sparse mode: shift subsequent elements left */
    if( targetSlot < count - 1 ){
      memmove(pPage->aRowIndex + targetSlot * 8,
              pPage->aRowIndex + (targetSlot + 1) * 8,
              (size_t)(count - 1 - targetSlot) * 8);
      memmove(pPage->aDataArea + targetSlot * rsz,
              pPage->aDataArea + (targetSlot + 1) * rsz,
              (size_t)(count - 1 - targetSlot) * rsz);
    }
    memset(pPage->aRowIndex + (count - 1) * 8, 0, 8);
    memset(pPage->aDataArea + (count - 1) * rsz, 0, rsz);
  }

  pPage->hdr.cell_count--;
  fssPutU16(pPage->aData + pPage->hdrOffset + 4, pPage->hdr.cell_count);

  return FSS_OK;
}

int fssPageGetColumn(
  const FssPage *pPage,
  uint16_t slotIndex,
  uint16_t colIndex,
  FssValue *pOutVal
) {
  if( pPage == NULL || pOutVal == NULL ) return FSS_ERROR;
  if( slotIndex >= pPage->hdr.cell_count ) return FSS_NOTFOUND;
  if( colIndex >= pPage->schema.num_columns ) return FSS_ERROR;

  const uint8_t *pSlot = pPage->aDataArea + slotIndex * pPage->hdr.row_payload_size;
  fssReadSlot(pSlot, &pPage->schema, pPage->hdr.flags, colIndex, pOutVal);
  return FSS_OK;
}

int fssPageGetRow(
  const FssPage *pPage,
  uint16_t slotIndex,
  int64_t *pOutRowid,
  FssValue *pOutValues,
  uint16_t maxValues
) {
  if( pPage == NULL || pOutValues == NULL ) return FSS_ERROR;
  if( slotIndex >= pPage->hdr.cell_count ) return FSS_NOTFOUND;

  if( pOutRowid ){
    if( pPage->hdr.flags & FSS_FLAG_DENSE_ROWID ){
      *pOutRowid = pPage->min_rowid + slotIndex;
    }else{
      *pOutRowid = fssGetI64(pPage->aRowIndex + slotIndex * 8);
    }
  }

  uint16_t limit = pPage->schema.num_columns < maxValues ? pPage->schema.num_columns : maxValues;
  const uint8_t *pSlot = pPage->aDataArea + slotIndex * pPage->hdr.row_payload_size;

  for( uint16_t i = 0; i < limit; i++ ){
    fssReadSlot(pSlot, &pPage->schema, pPage->hdr.flags, i, &pOutValues[i]);
  }

  return FSS_OK;
}

int fssPageUpdateColumn(
  FssPage *pPage,
  uint16_t slotIndex,
  uint16_t colIndex,
  const FssValue *pVal
) {
  if( pPage == NULL || pVal == NULL ) return FSS_ERROR;
  if( slotIndex >= pPage->hdr.cell_count ) return FSS_NOTFOUND;
  if( colIndex >= pPage->schema.num_columns ) return FSS_ERROR;

  /* Check column constraints */
  if( pVal->is_null ){
    if( pPage->schema.cols[colIndex].col_flags & FSS_COL_FLAG_NOT_NULL ){
      return FSS_TYPE_MISMATCH;
    }
    if( !(pPage->hdr.flags & FSS_FLAG_NULLABLE) ){
      return FSS_TYPE_MISMATCH;
    }
  }

  uint8_t *pSlot = pPage->aDataArea + slotIndex * pPage->hdr.row_payload_size;

  if( pPage->hdr.flags & FSS_FLAG_NULLABLE ){
    if( pVal->is_null ){
      pSlot[colIndex / 8] |= (uint8_t)(1 << (colIndex % 8));
      return FSS_OK;
    }else{
      pSlot[colIndex / 8] &= (uint8_t)~(1 << (colIndex % 8));
    }
  }

  uint8_t *pDest = pSlot + pPage->schema.col_offsets[colIndex];
  switch( pPage->schema.cols[colIndex].type_id ){
    case FSS_TYPE_INT8:
      *pDest = (uint8_t)(pVal->u.i & 0xff);
      break;
    case FSS_TYPE_INT16:
      fssPutU16(pDest, (uint16_t)pVal->u.i);
      break;
    case FSS_TYPE_INT32:
      fssPutU32(pDest, (uint32_t)pVal->u.i);
      break;
    case FSS_TYPE_INT64:
    case FSS_TYPE_TIMESTAMP_US:
      fssPutI64(pDest, pVal->u.i);
      break;
    case FSS_TYPE_FLOAT32:
      fssPutF32(pDest, (float)(pVal->type == FSS_TYPE_FLOAT64 ? pVal->u.r : pVal->u.f));
      break;
    case FSS_TYPE_FLOAT64:
      fssPutF64(pDest, pVal->u.r);
      break;
    case FSS_TYPE_FIXED_BLOB:
      if( pVal->u.p != NULL ){
        uint16_t w = pPage->schema.cols[colIndex].byte_width;
        uint16_t copyLen = pVal->n_bytes < w ? pVal->n_bytes : w;
        memcpy(pDest, pVal->u.p, copyLen);
      }
      break;
    default:
      return FSS_ERROR;
  }

  return FSS_OK;
}

/*
=========================================================================
** SCAN & VECTORIZED FILTER SCAN
=========================================================================
*/

int fssPageScanFilter(
  const FssPage *pPage,
  uint16_t colIndex,
  int op,
  const FssValue *pTarget,
  uint16_t *aMatchingSlots,
  uint16_t maxMatches,
  uint16_t *pnMatches
) {
  if( pPage == NULL || pTarget == NULL || aMatchingSlots == NULL || pnMatches == NULL ){
    return FSS_ERROR;
  }
  if( colIndex >= pPage->schema.num_columns ){
    return FSS_ERROR;
  }

  *pnMatches = 0;
  uint16_t count = pPage->hdr.cell_count;
  uint16_t rsz = pPage->hdr.row_payload_size;
  uint16_t colOff = pPage->schema.col_offsets[colIndex];
  uint8_t typeId = pPage->schema.cols[colIndex].type_id;
  uint8_t isNullable = (pPage->hdr.flags & FSS_FLAG_NULLABLE) != 0;
  uint8_t nullByteIdx = colIndex / 8;
  uint8_t nullBitMask = 1 << (colIndex % 8);

  const uint8_t *pData = pPage->aDataArea;

  /* Contiguous stride scan loop */
  for( uint16_t i = 0; i < count && *pnMatches < maxMatches; i++ ){
    const uint8_t *slot = pData + i * rsz;

    if( isNullable && (slot[nullByteIdx] & nullBitMask) ){
      if( pTarget->is_null && op == FSS_OP_EQ ){
        aMatchingSlots[*pnMatches] = i;
        (*pnMatches)++;
      }
      continue;
    }
    if( pTarget->is_null ){
      if( op == FSS_OP_NE ){
        aMatchingSlots[*pnMatches] = i;
        (*pnMatches)++;
      }
      continue;
    }

    int match = 0;

    switch( typeId ){
      case FSS_TYPE_INT8: {
        int8_t val = (int8_t)slot[colOff];
        int64_t tgt = pTarget->u.i;
        switch( op ){
          case FSS_OP_EQ: match = (val == tgt); break;
          case FSS_OP_NE: match = (val != tgt); break;
          case FSS_OP_LT: match = (val < tgt);  break;
          case FSS_OP_LE: match = (val <= tgt); break;
          case FSS_OP_GT: match = (val > tgt);  break;
          case FSS_OP_GE: match = (val >= tgt); break;
        }
        break;
      }
      case FSS_TYPE_INT16: {
        int16_t val = (int16_t)fssGetU16(slot + colOff);
        int64_t tgt = pTarget->u.i;
        switch( op ){
          case FSS_OP_EQ: match = (val == tgt); break;
          case FSS_OP_NE: match = (val != tgt); break;
          case FSS_OP_LT: match = (val < tgt);  break;
          case FSS_OP_LE: match = (val <= tgt); break;
          case FSS_OP_GT: match = (val > tgt);  break;
          case FSS_OP_GE: match = (val >= tgt); break;
        }
        break;
      }
      case FSS_TYPE_INT32: {
        int32_t val = (int32_t)fssGetU32(slot + colOff);
        int64_t tgt = pTarget->u.i;
        switch( op ){
          case FSS_OP_EQ: match = (val == tgt); break;
          case FSS_OP_NE: match = (val != tgt); break;
          case FSS_OP_LT: match = (val < tgt);  break;
          case FSS_OP_LE: match = (val <= tgt); break;
          case FSS_OP_GT: match = (val > tgt);  break;
          case FSS_OP_GE: match = (val >= tgt); break;
        }
        break;
      }
      case FSS_TYPE_INT64:
      case FSS_TYPE_TIMESTAMP_US: {
        int64_t val = fssGetI64(slot + colOff);
        int64_t tgt = pTarget->u.i;
        switch( op ){
          case FSS_OP_EQ: match = (val == tgt); break;
          case FSS_OP_NE: match = (val != tgt); break;
          case FSS_OP_LT: match = (val < tgt);  break;
          case FSS_OP_LE: match = (val <= tgt); break;
          case FSS_OP_GT: match = (val > tgt);  break;
          case FSS_OP_GE: match = (val >= tgt); break;
        }
        break;
      }
      case FSS_TYPE_FLOAT64: {
        double val = fssGetF64(slot + colOff);
        double tgt = (pTarget->type == FSS_TYPE_INT64) ? (double)pTarget->u.i : pTarget->u.r;
        switch( op ){
          case FSS_OP_EQ: match = (val == tgt); break;
          case FSS_OP_NE: match = (val != tgt); break;
          case FSS_OP_LT: match = (val < tgt);  break;
          case FSS_OP_LE: match = (val <= tgt); break;
          case FSS_OP_GT: match = (val > tgt);  break;
          case FSS_OP_GE: match = (val >= tgt); break;
        }
        break;
      }
      default:
        break;
    }

    if( match ){
      aMatchingSlots[*pnMatches] = i;
      (*pnMatches)++;
    }
  }

  return FSS_OK;
}

/*
=========================================================================
** DYNAMIC IN-PLACE DEMOTION TO STANDARD SQLITE 0x0D FORMAT
=========================================================================
*/

/*
** Internal structure representing an encoded dynamic SQLite cell.
*/
typedef struct FssDynCell {
  int64_t rowid;
  uint16_t cellSize;
  uint8_t aData[1024]; /* Sufficient for non-overflow leaf row payloads */
} FssDynCell;

/*
** Encode an array of FssValues into a standard SQLite 0x0D dynamic cell:
** [payload_len_varint | rowid_varint | record_header_varint | serial_types... | data...]
*/
static int fssEncodeSqliteCell(
  int64_t rowid,
  const FssValue *pValues,
  uint16_t nValues,
  FssDynCell *pCell
) {
  uint8_t payloadBuf[1024];
  uint8_t hdrVarints[256];
  uint16_t hdrBytes = 0;
  uint16_t dataBytes = 0;
  uint8_t dataBuf[1024];

  pCell->rowid = rowid;

  for( uint16_t i = 0; i < nValues; i++ ){
    const FssValue *v = &pValues[i];
    uint64_t serialType = 0;

    if( v->is_null ){
      serialType = 0;
    }else if( v->type == FSS_TYPE_INT8 || v->type == FSS_TYPE_INT16 ||
              v->type == FSS_TYPE_INT32 || v->type == FSS_TYPE_INT64 ||
              v->type == FSS_TYPE_TIMESTAMP_US ){
      int64_t val = v->u.i;
      if( val == 0 ){
        serialType = 8;
      }else if( val == 1 ){
        serialType = 9;
      }else if( val >= -128 && val <= 127 ){
        serialType = 1;
        dataBuf[dataBytes++] = (uint8_t)(val & 0xff);
      }else if( val >= -32768 && val <= 32767 ){
        serialType = 2;
        fssPutU16(dataBuf + dataBytes, (uint16_t)val);
        dataBytes += 2;
      }else if( val >= -8388608 && val <= 8388607 ){
        serialType = 3;
        dataBuf[dataBytes++] = (uint8_t)((val >> 16) & 0xff);
        dataBuf[dataBytes++] = (uint8_t)((val >> 8) & 0xff);
        dataBuf[dataBytes++] = (uint8_t)(val & 0xff);
      }else if( val >= (int64_t)INT32_MIN && val <= (int64_t)INT32_MAX ){
        serialType = 4;
        fssPutU32(dataBuf + dataBytes, (uint32_t)val);
        dataBytes += 4;
      }else{
        serialType = 6;
        fssPutI64(dataBuf + dataBytes, val);
        dataBytes += 8;
      }
    }else if( v->type == FSS_TYPE_FLOAT64 || v->type == FSS_TYPE_FLOAT32 ){
      serialType = 7;
      fssPutF64(dataBuf + dataBytes, v->type == FSS_TYPE_FLOAT64 ? v->u.r : (double)v->u.f);
      dataBytes += 8;
    }else if( v->type == FSS_TYPE_TEXT ){
      serialType = 2 * (uint64_t)v->n_bytes + 13;
      if( v->u.p && v->n_bytes > 0 ){
        memcpy(dataBuf + dataBytes, v->u.p, v->n_bytes);
        dataBytes += v->n_bytes;
      }
    }else if( v->type == FSS_TYPE_FIXED_BLOB ){
      serialType = 2 * (uint64_t)v->n_bytes + 12;
      if( v->u.p && v->n_bytes > 0 ){
        memcpy(dataBuf + dataBytes, v->u.p, v->n_bytes);
        dataBytes += v->n_bytes;
      }
    }

    hdrBytes += (uint16_t)fssPutVarint(hdrVarints + hdrBytes, serialType);
  }

  /* Compute record header size including header size varint */
  uint16_t totalHdrSize = hdrBytes + 1;
  if( totalHdrSize > 127 ){
    totalHdrSize = hdrBytes + 2;
  }

  uint16_t pOff = 0;
  pOff += (uint16_t)fssPutVarint(payloadBuf + pOff, totalHdrSize);
  memcpy(payloadBuf + pOff, hdrVarints, hdrBytes);
  pOff += hdrBytes;
  memcpy(payloadBuf + pOff, dataBuf, dataBytes);
  pOff += dataBytes;

  uint16_t payloadLen = pOff;

  /* Form outer SQLite cell */
  uint16_t cellLen = 0;
  cellLen += (uint16_t)fssPutVarint(pCell->aData + cellLen, payloadLen);
  cellLen += (uint16_t)fssPutVarint(pCell->aData + cellLen, (uint64_t)rowid);
  memcpy(pCell->aData + cellLen, payloadBuf, payloadLen);
  cellLen += payloadLen;

  pCell->cellSize = cellLen;
  return FSS_OK;
}

static int compareCells(const void *a, const void *b) {
  const FssDynCell *ca = (const FssDynCell*)a;
  const FssDynCell *cb = (const FssDynCell*)b;
  if( ca->rowid < cb->rowid ) return -1;
  if( ca->rowid > cb->rowid ) return 1;
  return 0;
}

int fssCalculateDemotedSize(
  const FssPage *pPage,
  int64_t mismatched_rowid,
  const FssValue *pMismatchedValues,
  uint16_t nMismatchValues,
  uint32_t *pnBytesNeeded
) {
  if( pPage == NULL || pnBytesNeeded == NULL ) return FSS_ERROR;

  uint16_t count = pPage->hdr.cell_count;
  uint16_t totalCells = count + (pMismatchedValues != NULL ? 1 : 0);
  uint32_t totalCellBytes = 0;

  FssValue vals[FSS_MAX_COLUMNS];
  FssDynCell cell;

  for( uint16_t i = 0; i < count; i++ ){
    int64_t rid;
    fssPageGetRow(pPage, i, &rid, vals, FSS_MAX_COLUMNS);
    fssEncodeSqliteCell(rid, vals, pPage->schema.num_columns, &cell);
    totalCellBytes += cell.cellSize;
  }

  if( pMismatchedValues != NULL ){
    fssEncodeSqliteCell(mismatched_rowid, pMismatchedValues, nMismatchValues, &cell);
    totalCellBytes += cell.cellSize;
  }

  /* 0x0D header: 8 bytes + 2 bytes per cell pointer */
  *pnBytesNeeded = pPage->hdrOffset + 8 + 2 * (uint32_t)totalCells + totalCellBytes;
  return FSS_OK;
}

int fssPageDemoteToDynamic(
  FssPage *pPage,
  int64_t mismatched_rowid,
  const FssValue *pMismatchedValues,
  uint16_t nMismatchValues
) {
  if( pPage == NULL ) return FSS_ERROR;

  uint16_t count = pPage->hdr.cell_count;
  uint16_t totalCells = count + (pMismatchedValues != NULL ? 1 : 0);
  uint32_t bytesNeeded = 0;

  int rc = fssCalculateDemotedSize(pPage, mismatched_rowid, pMismatchedValues, nMismatchValues, &bytesNeeded);
  if( rc != FSS_OK ) return rc;

  /* If space needed exceeds page size, B-tree balancing / split is required */
  if( bytesNeeded > pPage->pageSize ){
    return FSS_SPLIT_REQUIRED;
  }

  /* Allocate snapshot buffer for decoded cells */
  FssDynCell *aCells = (FssDynCell*)malloc(sizeof(FssDynCell) * totalCells);
  if( aCells == NULL ) return FSS_ERROR;

  FssValue vals[FSS_MAX_COLUMNS];
  for( uint16_t i = 0; i < count; i++ ){
    int64_t rid;
    fssPageGetRow(pPage, i, &rid, vals, FSS_MAX_COLUMNS);
    fssEncodeSqliteCell(rid, vals, pPage->schema.num_columns, &aCells[i]);
  }

  if( pMismatchedValues != NULL ){
    fssEncodeSqliteCell(mismatched_rowid, pMismatchedValues, nMismatchValues, &aCells[count]);
  }

  /* Sort cells in ascending order of rowid */
  qsort(aCells, totalCells, sizeof(FssDynCell), compareCells);

  /* Clear the entire page from hdrOffset */
  uint8_t *aData = pPage->aData;
  uint16_t hdrOffset = pPage->hdrOffset;
  memset(aData + hdrOffset, 0, pPage->pageSize - hdrOffset);

  /*
  ** Write standard SQLite 0x0D Table Leaf Header (8 bytes):
  ** Offset 0: 0x0D (PTF_LEAF | PTF_LEAFDATA | PTF_INTKEY)
  ** Offset 1..2: First freeblock (0)
  ** Offset 3..4: Number of cells (totalCells)
  ** Offset 5..6: Start of cell content area
  ** Offset 7: Fragmented free bytes (0)
  */
  aData[hdrOffset + 0] = SQLITE_PAGE_DYNAMIC_LEAF;
  fssPutU16(aData + hdrOffset + 1, 0);
  fssPutU16(aData + hdrOffset + 3, totalCells);
  aData[hdrOffset + 7] = 0;

  /* Write cells from bottom of page upwards */
  uint16_t contentOffset = (uint16_t)pPage->pageSize;
  uint16_t cellPtrOffset = hdrOffset + 8;

  for( uint16_t i = 0; i < totalCells; i++ ){
    contentOffset -= aCells[i].cellSize;
    memcpy(aData + contentOffset, aCells[i].aData, aCells[i].cellSize);
    fssPutU16(aData + cellPtrOffset + i * 2, contentOffset);
  }

  /* Update start of cell content area in header */
  fssPutU16(aData + hdrOffset + 5, contentOffset);

  free(aCells);
  return FSS_OK;
}
