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
  uint64_t u = 0;
  for( int i = 0; i < 8; i++ ) u = (u << 8) | p[i];
  double d;
  memcpy(&d, &u, sizeof(d));
  return d;
}

static inline void fssPutF64(uint8_t *p, double d) {
  uint64_t u;
  memcpy(&u, &d, sizeof(u));
  for( int i = 7; i >= 0; i-- ){
    p[i] = (uint8_t)(u & 0xff);
    u >>= 8;
  }
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

static int fssGetVarintBounded(const uint8_t *p, uint32_t n, uint64_t *pv){
  uint64_t v = 0;
  if( p == NULL || pv == NULL ) return 0;
  for( uint32_t i = 0; i < 8; i++ ){
    if( i >= n ) return 0;
    v = (v << 7) | (p[i] & 0x7f);
    if( !(p[i] & 0x80) ){
      *pv = v;
      return (int)i + 1;
    }
  }
  if( n < 9 ) return 0;
  *pv = (v << 8) | p[8];
  return 9;
}

static int fssSignExtend(uint64_t u, uint32_t nByte, int64_t *pOut){
  if( nByte == 0 || nByte > 8 || pOut == NULL ) return 0;
  if( nByte < 8 && (u & ((uint64_t)1 << (nByte * 8 - 1))) ){
    u |= UINT64_MAX << (nByte * 8);
  }
  if( u <= INT64_MAX ) *pOut = (int64_t)u;
  else *pOut = -1 - (int64_t)(~u);
  return 1;
}

int fssRecordToValues(
  const uint8_t *pRecord,
  uint32_t nRecord,
  FssValue *aValues,
  uint16_t maxValues,
  uint16_t *pnValues
){
  uint64_t headerSize64;
  uint32_t headerSize;
  uint32_t headerLen;
  uint32_t serialOff;
  uint32_t dataOff;
  uint16_t nValues = 0;
  uint64_t aSerial[FSS_MAX_COLUMNS];

  if( pnValues ) *pnValues = 0;
  if( pRecord == NULL || nRecord == 0 || pnValues == NULL ) return FSS_ERROR;
  int nHeaderLen = fssGetVarintBounded(pRecord, nRecord, &headerSize64);
  if( nHeaderLen == 0 || headerSize64 < (uint64_t)nHeaderLen ||
      headerSize64 > nRecord ) return FSS_CORRUPT;
  headerSize = (uint32_t)headerSize64;
  headerLen = headerSize;
  serialOff = (uint32_t)nHeaderLen;
  dataOff = headerSize;

  while( serialOff < headerLen ){
    uint64_t serial;
    int n = fssGetVarintBounded(pRecord + serialOff, headerLen - serialOff,
                                &serial);
    if( n == 0 || nValues >= FSS_MAX_COLUMNS ) return FSS_CORRUPT;
    aSerial[nValues++] = serial;
    serialOff += (uint32_t)n;
  }
  if( serialOff != headerLen ) return FSS_CORRUPT;
  if( nValues > maxValues || (nValues > 0 && aValues == NULL) ) return FSS_FULL;

  for( uint16_t i = 0; i < nValues; i++ ){
    uint64_t serial = aSerial[i];
    uint32_t nData = 0;
    FssValue v;
    memset(&v, 0, sizeof(v));
    if( serial == 0 ){
      v = fssValueNull();
    }else if( serial >= 1 && serial <= 6 ){
      static const uint8_t aWidth[] = {0,1,2,3,4,6,8};
      uint64_t u = 0;
      nData = aWidth[serial];
      if( nData > nRecord - dataOff ) return FSS_CORRUPT;
      for( uint32_t b = 0; b < nData; b++ ) u = (u << 8) | pRecord[dataOff + b];
      if( !fssSignExtend(u, nData, &v.u.i) ) return FSS_CORRUPT;
      v.type = nData == 1 ? FSS_TYPE_INT8 :
               nData == 2 ? FSS_TYPE_INT16 :
               nData <= 4 ? FSS_TYPE_INT32 : FSS_TYPE_INT64;
    }else if( serial == 7 ){
      nData = 8;
      if( nData > nRecord - dataOff ) return FSS_CORRUPT;
      v.type = FSS_TYPE_FLOAT64;
      v.u.r = fssGetF64(pRecord + dataOff);
    }else if( serial == 8 || serial == 9 ){
      v.type = FSS_TYPE_INT64;
      v.u.i = serial == 8 ? 0 : 1;
    }else if( serial == 10 || serial == 11 ){
      return FSS_CORRUPT;
    }else if( serial >= 12 ){
      uint64_t n64 = (serial - (serial & 1 ? 13 : 12)) / 2;
      if( n64 > UINT16_MAX ) return FSS_TYPE_MISMATCH;
      nData = (uint32_t)n64;
      if( nData > nRecord - dataOff ) return FSS_CORRUPT;
      v.type = serial & 1 ? FSS_TYPE_TEXT : FSS_TYPE_FIXED_BLOB;
      v.u.p = pRecord + dataOff;
      v.n_bytes = (uint16_t)nData;
    }else{
      return FSS_CORRUPT;
    }
    if( nData > nRecord - dataOff ) return FSS_CORRUPT;
    dataOff += nData;
    aValues[i] = v;
  }
  if( dataOff != nRecord ) return FSS_CORRUPT;
  *pnValues = nValues;
  return FSS_OK;
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

  if( pCols == NULL || nCols == 0 || nCols > FSS_MAX_COLUMNS
   || (flags & ~(FSS_FLAG_DENSE_ROWID|FSS_FLAG_NULLABLE))!=0
  ){
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
        else if( w != 1 ) return FSS_ERROR;
        break;
      case FSS_TYPE_INT16:
        if( w == 0 ) w = 2;
        else if( w != 2 ) return FSS_ERROR;
        break;
      case FSS_TYPE_INT32:
      case FSS_TYPE_FLOAT32:
        if( w == 0 ) w = 4;
        else if( w != 4 ) return FSS_ERROR;
        break;
      case FSS_TYPE_INT64:
      case FSS_TYPE_FLOAT64:
      case FSS_TYPE_TIMESTAMP_US:
        if( w == 0 ) w = 8;
        else if( w != 8 ) return FSS_ERROR;
        break;
      case FSS_TYPE_FIXED_BLOB:
        if( w == 0 ) return FSS_ERROR;
        break;
      default:
        return FSS_ERROR;
    }
    dataBytes += w;
    if( dataBytes + nullBytes > UINT16_MAX ) return FSS_ERROR;
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

  if( rowPayloadSize == 0 || nCols == 0 || nCols > FSS_MAX_COLUMNS
   || pageSize > 65536
   || hdrOffset > pageSize
   || (flags & ~(FSS_FLAG_DENSE_ROWID|FSS_FLAG_NULLABLE))!=0
  ) return 0;

  descSize = FSS_SCHEMA_HEADER_SIZE + (uint32_t)nCols * FSS_FIELD_DESC_SIZE;
  fixedHeaderAndDesc = FSS_HEADER_SIZE + descSize;

  if( pageSize < (uint32_t)hdrOffset + fixedHeaderAndDesc ){
    return 0;
  }

  avail = pageSize - hdrOffset - fixedHeaderAndDesc;

  if( flags & FSS_FLAG_DENSE_ROWID ){
    /* Dense sequential mode: index section is simply 8-byte min_rowid */
    if( avail <= 8 ) return 0;
    avail -= 8;
    avail /= rowPayloadSize;
  }else{
    /* General / sparse mode: each slot requires 8 bytes rowid + rowPayloadSize */
    uint32_t slotTotal = 8 + (uint32_t)rowPayloadSize;
    avail /= slotTotal;
  }
  return avail > UINT16_MAX ? UINT16_MAX : (uint16_t)avail;
}

int fssSchemaInit(
  FssSchema *pSchema,
  const FssFieldDesc *pCols,
  uint16_t nCols,
  uint8_t flags
) {
  uint16_t offset;
  int rc;

  if( pSchema == NULL || pCols == NULL || nCols == 0 || nCols > FSS_MAX_COLUMNS
   || (flags & ~(FSS_FLAG_DENSE_ROWID|FSS_FLAG_NULLABLE))!=0
  ){
    return FSS_ERROR;
  }

  for( uint16_t i = 0; i < nCols; i++ ){
    if( pCols[i].col_flags & ~(FSS_COL_FLAG_NOT_NULL|FSS_COL_FLAG_PRIMARY_KEY) ){
      return FSS_ERROR;
    }
    if( !(pCols[i].col_flags & FSS_COL_FLAG_NOT_NULL)
     && !(flags & FSS_FLAG_NULLABLE)
    ){
      return FSS_ERROR;
    }
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

void fssWriteSlot(
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
        }else if( v->type == FSS_TYPE_INT8 || v->type == FSS_TYPE_INT16 ||
                  v->type == FSS_TYPE_INT32 || v->type == FSS_TYPE_INT64 ){
          fssPutF32(pDest, (float)v->u.i);
        }else{
          fssPutF32(pDest, v->u.f);
        }
        break;
      case FSS_TYPE_FLOAT64:
        if( v->type == FSS_TYPE_INT8 || v->type == FSS_TYPE_INT16 ||
            v->type == FSS_TYPE_INT32 || v->type == FSS_TYPE_INT64 ){
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

  if( aData == NULL || pCols == NULL || pInitialValues == NULL || nCols == 0
   || pageSize < 512 || pageSize > 65536 || hdrOffset > pageSize
   || pageSize - hdrOffset < FSS_HEADER_SIZE
  ){
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
  pHdr[FSS_HDR_PAGE_TYPE] = FSS_PAGE_TYPE;
  pHdr[FSS_HDR_FLAGS] = flags;
  fssPutU16(pHdr + FSS_HDR_SCHEMA_VERSION, FSS_SCHEMA_VERSION);
  fssPutU16(pHdr + FSS_HDR_CELL_COUNT, 1); /* Pages are created with one row. */
  fssPutU16(pHdr + FSS_HDR_CAPACITY, capacity);
  fssPutU16(pHdr + FSS_HDR_ROW_PAYLOAD_SIZE, schema.row_payload_size);
  fssPutU16(pHdr + FSS_HDR_FIELD_DESC_OFFSET, FSS_HEADER_SIZE);
  fssPutU16(pHdr + FSS_HDR_DATA_AREA_OFFSET, dataAreaOffset);
  fssPutU16(pHdr + FSS_HDR_RESERVED, 0);

  /*
  ** Section 2: Field Type Descriptor
  */
  pDesc = pHdr + FSS_HEADER_SIZE;
  fssPutU16(pDesc + FSS_SCHEMA_NUM_COLUMNS, nCols);
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
  uint8_t *pHdr;
  const uint8_t *pDesc;
  uint16_t nCols;
  uint16_t descSize;
  uint16_t rowPayloadSize;
  uint32_t expectedCapacity;
  uint32_t expectedDataOffset;

  if( aData == NULL || pPage == NULL || pageSize < 512 || pageSize > 65536
   || hdrOffset > pageSize || pageSize - hdrOffset < FSS_HEADER_SIZE
  ){
    return FSS_ERROR;
  }

  pHdr = aData + hdrOffset;
  if( pHdr[FSS_HDR_PAGE_TYPE] != FSS_PAGE_TYPE ){
    return FSS_CORRUPT;
  }

  memset(pPage, 0, sizeof(*pPage));
  pPage->aData = aData;
  pPage->pageSize = pageSize;
  pPage->hdrOffset = hdrOffset;

  /* Read Header */
  pPage->hdr.page_type = pHdr[FSS_HDR_PAGE_TYPE];
  pPage->hdr.flags = pHdr[FSS_HDR_FLAGS];
  pPage->hdr.schema_version = fssGetU16(pHdr + FSS_HDR_SCHEMA_VERSION);
  pPage->hdr.cell_count = fssGetU16(pHdr + FSS_HDR_CELL_COUNT);
  pPage->hdr.capacity = fssGetU16(pHdr + FSS_HDR_CAPACITY);
  pPage->hdr.row_payload_size = fssGetU16(pHdr + FSS_HDR_ROW_PAYLOAD_SIZE);
  pPage->hdr.field_desc_offset = fssGetU16(pHdr + FSS_HDR_FIELD_DESC_OFFSET);
  pPage->hdr.data_area_offset = fssGetU16(pHdr + FSS_HDR_DATA_AREA_OFFSET);
  pPage->hdr.reserved = fssGetU16(pHdr + FSS_HDR_RESERVED);

  if( pPage->hdr.schema_version != FSS_SCHEMA_VERSION
   || pPage->hdr.cell_count == 0
   || pPage->hdr.cell_count > pPage->hdr.capacity
   || pPage->hdr.capacity == 0
   || (pPage->hdr.flags & ~(FSS_FLAG_DENSE_ROWID|FSS_FLAG_NULLABLE))!=0
   || pPage->hdr.reserved != 0
   || pPage->hdr.field_desc_offset != FSS_HEADER_SIZE
  ){
    return FSS_CORRUPT;
  }

  /* Read Schema */
  if( pPage->hdr.field_desc_offset > pageSize - hdrOffset
   || pageSize - hdrOffset - pPage->hdr.field_desc_offset < FSS_SCHEMA_HEADER_SIZE
  ){
    return FSS_CORRUPT;
  }
  pDesc = pHdr + pPage->hdr.field_desc_offset;
  nCols = fssGetU16(pDesc + FSS_SCHEMA_NUM_COLUMNS);
  if( nCols == 0 || nCols > FSS_MAX_COLUMNS ){
    return FSS_CORRUPT;
  }
  descSize = FSS_SCHEMA_HEADER_SIZE + nCols * FSS_FIELD_DESC_SIZE;
  if( descSize > pageSize - hdrOffset - pPage->hdr.field_desc_offset ){
    return FSS_CORRUPT;
  }

  FssFieldDesc cols[FSS_MAX_COLUMNS];
  for( uint16_t i = 0; i < nCols; i++ ){
    cols[i].type_id = pDesc[FSS_SCHEMA_COLUMNS + i * FSS_FIELD_DESC_SIZE];
    cols[i].col_flags = pDesc[FSS_SCHEMA_COLUMNS + i * FSS_FIELD_DESC_SIZE + 1];
    cols[i].byte_width = fssGetU16(pDesc + FSS_SCHEMA_COLUMNS + i * FSS_FIELD_DESC_SIZE + 2);
  }

  if( fssSchemaInit(&pPage->schema, cols, nCols, pPage->hdr.flags) != FSS_OK ){
    return FSS_CORRUPT;
  }

  if( pPage->schema.row_payload_size != pPage->hdr.row_payload_size ){
    return FSS_CORRUPT;
  }
  rowPayloadSize = pPage->schema.row_payload_size;
  expectedCapacity = fssCalculateCapacity(pageSize, hdrOffset, nCols,
                                           rowPayloadSize, pPage->hdr.flags);
  if( expectedCapacity == 0
   || expectedCapacity != pPage->hdr.capacity
  ){
    return FSS_CORRUPT;
  }
  expectedDataOffset = FSS_HEADER_SIZE + descSize;
  if( pPage->hdr.flags & FSS_FLAG_DENSE_ROWID ){
    expectedDataOffset += 8;
  }else{
    expectedDataOffset += 8 * expectedCapacity;
  }
  if( expectedDataOffset > UINT16_MAX
   || pPage->hdr.data_area_offset != expectedDataOffset
   || (uint32_t)hdrOffset + expectedDataOffset > pageSize
   || (uint32_t)pPage->hdr.cell_count * rowPayloadSize >
      pageSize - hdrOffset - expectedDataOffset
  ){
    return FSS_CORRUPT;
  }

  pPage->aRowIndex = pHdr + FSS_HEADER_SIZE + descSize;
  pPage->aDataArea = pHdr + pPage->hdr.data_area_offset;

  if( pPage->hdr.flags & FSS_FLAG_DENSE_ROWID ){
    pPage->min_rowid = fssGetI64(pPage->aRowIndex);
    if( pPage->min_rowid > INT64_MAX - (int64_t)(pPage->hdr.cell_count - 1) ){
      return FSS_CORRUPT;
    }
  }else{
    for( uint16_t i = 1; i < pPage->hdr.cell_count; i++ ){
      if( fssGetI64(pPage->aRowIndex + (i - 1) * 8) >=
          fssGetI64(pPage->aRowIndex + i * 8) ){
        return FSS_CORRUPT;
      }
    }
    for( uint32_t i = pPage->hdr.cell_count; i < pPage->hdr.capacity; i++ ){
      if( fssGetI64(pPage->aRowIndex + i * 8) != 0 ) return FSS_CORRUPT;
    }
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

/* Repack a dense page as sparse. Optionally omit one row while converting. */
static int fssPageDenseToSparse(FssPage *pPage, int iSkip){
  uint16_t oldCount = pPage->hdr.cell_count;
  uint16_t newCount = oldCount - (iSkip >= 0 ? 1 : 0);
  uint16_t nCols = pPage->schema.num_columns;
  uint16_t rowSize = pPage->hdr.row_payload_size;
  uint32_t descEnd = FSS_HEADER_SIZE + FSS_SCHEMA_HEADER_SIZE
                   + (uint32_t)nCols * FSS_FIELD_DESC_SIZE;
  uint16_t newCapacity;
  uint32_t newDataOffset;
  uint8_t *savedRows;
  uint8_t *pHdr;
  uint8_t *pNewIndex;
  uint8_t *pNewData;
  uint16_t dst = 0;

  if( !(pPage->hdr.flags & FSS_FLAG_DENSE_ROWID)
   || (iSkip >= 0 && iSkip >= oldCount)
  ) return FSS_ERROR;

  newCapacity = fssCalculateCapacity(pPage->pageSize, pPage->hdrOffset,
                                     nCols, rowSize,
                                     pPage->hdr.flags & ~FSS_FLAG_DENSE_ROWID);
  if( newCapacity == 0 || newCount > newCapacity ) return FSS_FULL;
  savedRows = (uint8_t*)malloc((size_t)oldCount * rowSize);
  if( savedRows == NULL ) return FSS_ERROR;
  memcpy(savedRows, pPage->aDataArea, (size_t)oldCount * rowSize);

  newDataOffset = descEnd + (uint32_t)newCapacity * 8;
  if( newDataOffset > UINT16_MAX
   || (uint32_t)pPage->hdrOffset + newDataOffset +
      (uint32_t)newCount * rowSize > pPage->pageSize
  ){
    free(savedRows);
    return FSS_FULL;
  }

  pHdr = pPage->aData + pPage->hdrOffset;
  memset(pHdr + descEnd, 0, pPage->pageSize - pPage->hdrOffset - descEnd);
  pPage->hdr.flags &= (uint8_t)~FSS_FLAG_DENSE_ROWID;
  pPage->hdr.capacity = newCapacity;
  pPage->hdr.cell_count = newCount;
  pPage->hdr.data_area_offset = (uint16_t)newDataOffset;
  pHdr[FSS_HDR_FLAGS] = pPage->hdr.flags;
  fssPutU16(pHdr + FSS_HDR_CAPACITY, newCapacity);
  fssPutU16(pHdr + FSS_HDR_CELL_COUNT, newCount);
  fssPutU16(pHdr + FSS_HDR_DATA_AREA_OFFSET, (uint16_t)newDataOffset);

  pNewIndex = pHdr + descEnd;
  pNewData = pHdr + newDataOffset;
  for( uint16_t src = 0; src < oldCount; src++ ){
    if( src == iSkip ) continue;
    fssPutI64(pNewIndex + dst * 8, pPage->min_rowid + src);
    memcpy(pNewData + (size_t)dst * rowSize,
           savedRows + (size_t)src * rowSize, rowSize);
    dst++;
  }
  pPage->aRowIndex = pNewIndex;
  pPage->aDataArea = pNewData;
  free(savedRows);
  return FSS_OK;
}

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
    if( rowid < pPage->min_rowid ){
      *pSlotIndex = 0;
      return FSS_NOTFOUND;
    }
    uint64_t slot = (uint64_t)rowid - (uint64_t)pPage->min_rowid;
    if( slot < pPage->hdr.cell_count ){
      *pSlotIndex = (uint16_t)slot;
      return FSS_OK;
    }
    *pSlotIndex = pPage->hdr.cell_count;
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

  rc = fssPageFindRow(pPage, rowid, &targetSlot);
  if( rc == FSS_OK ) return FSS_DUPLICATE;

  rc = fssCheckRowMatchesSchema(&pPage->schema, pPage->hdr.flags, pValues, nValues);
  if( rc != FSS_OK ) return FSS_TYPE_MISMATCH;

  /* Check capacity */
  if( pPage->hdr.cell_count >= pPage->hdr.capacity ){
    return FSS_FULL;
  }

  uint16_t count = pPage->hdr.cell_count;
  uint16_t rsz = pPage->hdr.row_payload_size;

  if( pPage->hdr.flags & FSS_FLAG_DENSE_ROWID ){
    if( pPage->min_rowid <= INT64_MAX - (int64_t)count
     && rowid == pPage->min_rowid + (int64_t)count
    ){
      targetSlot = count;
    }else if( pPage->min_rowid > INT64_MIN
           && rowid == pPage->min_rowid - 1
    ){
      /* Prepend row */
      memmove(pPage->aDataArea + rsz, pPage->aDataArea, (size_t)count * rsz);
      pPage->min_rowid = rowid;
      fssPutI64(pPage->aRowIndex, rowid);
      targetSlot = 0;
    }else{
      rc = fssPageDenseToSparse(pPage, -1);
      if( rc != FSS_OK ) return rc;
      rc = fssPageFindRow(pPage, rowid, &targetSlot);
      if( rc == FSS_OK ) return FSS_DUPLICATE;
    }
  }

  if( !(pPage->hdr.flags & FSS_FLAG_DENSE_ROWID) ){
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
  fssPutU16(pPage->aData + pPage->hdrOffset + FSS_HDR_CELL_COUNT,
            pPage->hdr.cell_count);

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
      /* A middle delete breaks contiguity; convert while omitting this row. */
      return fssPageDenseToSparse(pPage, targetSlot);
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
  fssPutU16(pPage->aData + pPage->hdrOffset + FSS_HDR_CELL_COUNT,
            pPage->hdr.cell_count);

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
  FssValue row[FSS_MAX_COLUMNS];
  if( pPage == NULL || pVal == NULL ) return FSS_ERROR;
  if( slotIndex >= pPage->hdr.cell_count ) return FSS_NOTFOUND;
  if( colIndex >= pPage->schema.num_columns ) return FSS_ERROR;
  if( fssPageGetRow(pPage, slotIndex, NULL, row, FSS_MAX_COLUMNS) != FSS_OK ){
    return FSS_ERROR;
  }
  row[colIndex] = *pVal;
  if( fssCheckRowMatchesSchema(&pPage->schema, pPage->hdr.flags, row,
                               pPage->schema.num_columns) != FSS_OK ){
    return FSS_TYPE_MISMATCH;
  }
  fssWriteSlot(pPage->aDataArea + (size_t)slotIndex * pPage->hdr.row_payload_size,
               &pPage->schema, pPage->hdr.flags, row);
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
  uint32_t cellSize;
  uint8_t *aData;
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
  uint64_t aSerial[FSS_MAX_COLUMNS];
  uint32_t serialBytes = 0;
  uint32_t dataBytes = 0;
  uint32_t recordHeaderSize;
  uint32_t payloadSize;
  uint32_t rowidBytes;
  uint32_t payloadLenBytes;
  uint32_t cellSize;
  uint8_t *p;

  if( pValues == NULL || pCell == NULL || nValues > FSS_MAX_COLUMNS ) return FSS_ERROR;
  pCell->aData = NULL;
  pCell->cellSize = 0;
  pCell->rowid = rowid;

  for( uint16_t i = 0; i < nValues; i++ ){
    const FssValue *v = &pValues[i];
    uint32_t nData = 0;
    if( v->is_null ){
      aSerial[i] = 0;
    }else if( v->type == FSS_TYPE_INT8 || v->type == FSS_TYPE_INT16 ||
              v->type == FSS_TYPE_INT32 || v->type == FSS_TYPE_INT64 ||
              v->type == FSS_TYPE_TIMESTAMP_US ){
      int64_t val = v->u.i;
      if( val == 0 ) aSerial[i] = 8;
      else if( val == 1 ) aSerial[i] = 9;
      else if( val >= -128 && val <= 127 ){ aSerial[i] = 1; nData=1; }
      else if( val >= -32768 && val <= 32767 ){ aSerial[i] = 2; nData=2; }
      else if( val >= -8388608 && val <= 8388607 ){ aSerial[i] = 3; nData=3; }
      else if( val >= INT32_MIN && val <= INT32_MAX ){ aSerial[i] = 4; nData=4; }
      else if( val >= -140737488355328LL && val <= 140737488355327LL ){
        aSerial[i] = 5; nData=6;
      }else{ aSerial[i] = 6; nData=8; }
    }else if( v->type == FSS_TYPE_FLOAT64 || v->type == FSS_TYPE_FLOAT32 ){
      aSerial[i] = 7; nData = 8;
    }else if( v->type == FSS_TYPE_TEXT || v->type == FSS_TYPE_FIXED_BLOB ){
      if( v->n_bytes > 0 && v->u.p == NULL ) return FSS_ERROR;
      aSerial[i] = 2 * (uint64_t)v->n_bytes +
                   (v->type == FSS_TYPE_TEXT ? 13 : 12);
      nData = v->n_bytes;
    }else{
      return FSS_TYPE_MISMATCH;
    }
    if( UINT32_MAX - dataBytes < nData ) return FSS_FULL;
    dataBytes += nData;
    serialBytes += (uint32_t)fssVarintLen(aSerial[i]);
  }

  recordHeaderSize = serialBytes + 1;
  while( recordHeaderSize != serialBytes + (uint32_t)fssVarintLen(recordHeaderSize) ){
    recordHeaderSize = serialBytes + (uint32_t)fssVarintLen(recordHeaderSize);
  }
  if( UINT32_MAX - recordHeaderSize < dataBytes ) return FSS_FULL;
  payloadSize = recordHeaderSize + dataBytes;
  rowidBytes = (uint32_t)fssVarintLen((uint64_t)rowid);
  payloadLenBytes = (uint32_t)fssVarintLen(payloadSize);
  if( UINT32_MAX - payloadLenBytes < rowidBytes + payloadSize ) return FSS_FULL;
  cellSize = payloadLenBytes + rowidBytes + payloadSize;
  p = (uint8_t*)malloc(cellSize);
  if( p == NULL ) return FSS_ERROR;

  uint32_t off = 0;
  off += (uint32_t)fssPutVarint(p + off, payloadSize);
  off += (uint32_t)fssPutVarint(p + off, (uint64_t)rowid);
  off += (uint32_t)fssPutVarint(p + off, recordHeaderSize);
  for( uint16_t i = 0; i < nValues; i++ ){
    off += (uint32_t)fssPutVarint(p + off, aSerial[i]);
  }
  for( uint16_t i = 0; i < nValues; i++ ){
    const FssValue *v = &pValues[i];
    int64_t val;
    uint32_t nData = 0;
    if( v->is_null ) continue;
    if( v->type == FSS_TYPE_INT8 || v->type == FSS_TYPE_INT16 ||
        v->type == FSS_TYPE_INT32 || v->type == FSS_TYPE_INT64 ||
        v->type == FSS_TYPE_TIMESTAMP_US ){
      val = v->u.i;
      if( aSerial[i] == 1 ) p[off++] = (uint8_t)val;
      else if( aSerial[i] == 2 ){ fssPutU16(p + off, (uint16_t)val); off += 2; }
      else if( aSerial[i] == 3 ){
        uint64_t u = (uint64_t)val; p[off++] = (uint8_t)(u >> 16);
        p[off++] = (uint8_t)(u >> 8); p[off++] = (uint8_t)u;
      }else if( aSerial[i] == 4 ){ fssPutU32(p + off, (uint32_t)val); off += 4; }
      else if( aSerial[i] == 5 ){
        uint64_t u = (uint64_t)val;
        for( int b = 5; b >= 0; b-- ) p[off++] = (uint8_t)(u >> (8*b));
      }else if( aSerial[i] == 6 ){ fssPutI64(p + off, val); off += 8; }
    }else if( v->type == FSS_TYPE_FLOAT64 || v->type == FSS_TYPE_FLOAT32 ){
      fssPutF64(p + off, v->type == FSS_TYPE_FLOAT64 ? v->u.r : (double)v->u.f);
      off += 8;
    }else{
      nData = v->n_bytes;
      if( nData ) memcpy(p + off, v->u.p, nData);
      off += nData;
    }
  }

  if( off != cellSize ){
    free(p);
    return FSS_ERROR;
  }
  pCell->aData = p;
  pCell->cellSize = cellSize;
  return FSS_OK;
}

static void fssFreeDynCell(FssDynCell *pCell){
  free(pCell->aData);
  pCell->aData = NULL;
  pCell->cellSize = 0;
}

int fssValuesToSqliteCell(
  int64_t rowid,
  const FssValue *aValues,
  uint16_t nValues,
  uint8_t *pCell,
  uint32_t cellCapacity,
  uint32_t *pnCell
){
  FssDynCell cell;
  int rc;
  if( pnCell == NULL ) return FSS_ERROR;
  *pnCell = 0;
  rc = fssEncodeSqliteCell(rowid, aValues, nValues, &cell);
  if( rc != FSS_OK ) return rc;
  *pnCell = cell.cellSize;
  if( pCell != NULL ){
    if( cellCapacity < cell.cellSize ){
      fssFreeDynCell(&cell);
      return FSS_FULL;
    }
    memcpy(pCell, cell.aData, cell.cellSize);
  }
  fssFreeDynCell(&cell);
  return FSS_OK;
}

static int compareCells(const void *a, const void *b) {
  const FssDynCell *ca = (const FssDynCell*)a;
  const FssDynCell *cb = (const FssDynCell*)b;
  if( ca->rowid < cb->rowid ) return -1;
  if( ca->rowid > cb->rowid ) return 1;
  return 0;
}

static void fssFreeDynCells(FssDynCell *aCells, uint16_t nCells){
  if( aCells == NULL ) return;
  for( uint16_t i = 0; i < nCells; i++ ) fssFreeDynCell(&aCells[i]);
  free(aCells);
}

static int fssBuildDemotionCells(
  const FssPage *pPage,
  int64_t mismatched_rowid,
  const FssValue *pMismatchedValues,
  uint16_t nMismatchValues,
  FssDynCell **paCells,
  uint16_t *pnCells
){
  uint16_t count;
  uint32_t allocCount;
  uint16_t nOut = 0;
  int found = 0;
  FssDynCell *aCells;
  FssValue vals[FSS_MAX_COLUMNS];
  int rc = FSS_OK;

  if( pPage == NULL || paCells == NULL || pnCells == NULL ) return FSS_ERROR;
  if( pMismatchedValues != NULL &&
      (nMismatchValues != pPage->schema.num_columns ||
       nMismatchValues > FSS_MAX_COLUMNS) ){
    return FSS_ERROR;
  }
  count = pPage->hdr.cell_count;
  allocCount = (uint32_t)count + (pMismatchedValues != NULL ? 1 : 0);
  if( allocCount == 0 || allocCount > UINT16_MAX ) return FSS_FULL;
  aCells = (FssDynCell*)calloc(allocCount, sizeof(FssDynCell));
  if( aCells == NULL ) return FSS_ERROR;

  for( uint16_t i = 0; i < count; i++ ){
    int64_t rid;
    if( fssPageGetRow(pPage, i, &rid, vals, FSS_MAX_COLUMNS) != FSS_OK ){
      rc = FSS_CORRUPT;
      break;
    }
    if( pMismatchedValues != NULL && rid == mismatched_rowid ){
      rc = fssEncodeSqliteCell(rid, pMismatchedValues, nMismatchValues,
                               &aCells[nOut]);
      found = 1;
    }else{
      rc = fssEncodeSqliteCell(rid, vals, pPage->schema.num_columns,
                               &aCells[nOut]);
    }
    if( rc != FSS_OK ) break;
    nOut++;
  }
  if( rc == FSS_OK && pMismatchedValues != NULL && !found ){
    rc = fssEncodeSqliteCell(mismatched_rowid, pMismatchedValues,
                             nMismatchValues, &aCells[nOut]);
    if( rc == FSS_OK ) nOut++;
  }
  if( rc != FSS_OK ){
    fssFreeDynCells(aCells, (uint16_t)allocCount);
    return rc;
  }

  qsort(aCells, nOut, sizeof(FssDynCell), compareCells);
  for( uint16_t i = 1; i < nOut; i++ ){
    if( aCells[i - 1].rowid >= aCells[i].rowid ){
      fssFreeDynCells(aCells, (uint16_t)allocCount);
      return FSS_DUPLICATE;
    }
  }
  *paCells = aCells;
  *pnCells = nOut;
  return FSS_OK;
}

int fssCalculateDemotedSize(
  const FssPage *pPage,
  int64_t mismatched_rowid,
  const FssValue *pMismatchedValues,
  uint16_t nMismatchValues,
  uint32_t *pnBytesNeeded
) {
  if( pPage == NULL || pnBytesNeeded == NULL ) return FSS_ERROR;

  FssDynCell *aCells = NULL;
  uint16_t nCells = 0;
  uint64_t totalCellBytes = 0;
  uint64_t needed;
  int rc = fssBuildDemotionCells(pPage, mismatched_rowid,
                                  pMismatchedValues, nMismatchValues,
                                  &aCells, &nCells);
  if( rc != FSS_OK ) return rc;
  for( uint16_t i = 0; i < nCells; i++ ) totalCellBytes += aCells[i].cellSize;
  /* Standard 0x0D header, pointer array, and encoded cells. */
  needed = (uint64_t)pPage->hdrOffset + 8 + 2 * (uint64_t)nCells + totalCellBytes;
  fssFreeDynCells(aCells, nCells);
  if( needed > UINT32_MAX ) return FSS_FULL;
  *pnBytesNeeded = (uint32_t)needed;
  return FSS_OK;
}

int fssPageDemoteToDynamic(
  FssPage *pPage,
  int64_t mismatched_rowid,
  const FssValue *pMismatchedValues,
  uint16_t nMismatchValues
) {
  if( pPage == NULL ) return FSS_ERROR;

  FssDynCell *aCells = NULL;
  uint16_t totalCells = 0;
  uint64_t totalCellBytes = 0;
  uint64_t bytesNeeded;
  int rc = fssBuildDemotionCells(pPage, mismatched_rowid,
                                  pMismatchedValues, nMismatchValues,
                                  &aCells, &totalCells);
  if( rc != FSS_OK ) return rc;
  for( uint16_t i = 0; i < totalCells; i++ ) totalCellBytes += aCells[i].cellSize;
  bytesNeeded = (uint64_t)pPage->hdrOffset + 8 +
                2 * (uint64_t)totalCells + totalCellBytes;
  if( bytesNeeded > pPage->pageSize ){
    fssFreeDynCells(aCells, totalCells);
    return FSS_SPLIT_REQUIRED;
  }

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
  uint32_t contentOffset = pPage->pageSize;
  uint32_t cellPtrOffset = hdrOffset + 8;

  for( uint16_t i = 0; i < totalCells; i++ ){
    contentOffset -= aCells[i].cellSize;
    memcpy(aData + contentOffset, aCells[i].aData, aCells[i].cellSize);
    fssPutU16(aData + cellPtrOffset + i * 2, (uint16_t)contentOffset);
    fssFreeDynCell(&aCells[i]);
  }

  /* Update start of cell content area in header */
  fssPutU16(aData + hdrOffset + 5,
            (uint16_t)(contentOffset == 65536 ? 0 : contentOffset));

  free(aCells);
  return FSS_OK;
}

/*
** Validate the FSS database compatibility marker in the 100-byte SQLite header.
** If aDbHdr is null, returns FSS_ERROR.
** If bytes 72-79 are all zero, returns FSS_MARKER_NONE and sets *pVersion to 0 (if pVersion!=NULL).
** If bytes 72-75 match "FSS1":
**   reads big-endian 32-bit version into *pVersion (if pVersion!=NULL).
**   if version == FSS_DB_VERSION, returns FSS_MARKER_VALID.
**   else returns FSS_MARKER_UNSUPPORTED_VER.
** If bytes 72-79 are non-zero but bytes 72-75 != "FSS1", returns FSS_MARKER_INVALID.
*/
int fssValidateDbMarker(const uint8_t *aDbHdr, uint32_t *pVersion){
  const uint8_t *p;
  uint32_t ver;
  int allZero = 1;
  int i;

  if( !aDbHdr ) return FSS_ERROR;
  p = aDbHdr + FSS_DB_MARKER_OFFSET;

  for( i = 0; i < FSS_DB_MARKER_SIZE; i++ ){
    if( p[i] != 0 ){
      allZero = 0;
      break;
    }
  }
  if( allZero ){
    if( pVersion ) *pVersion = 0;
    return FSS_MARKER_NONE;
  }

  if( memcmp(p, FSS_DB_MAGIC, FSS_DB_MAGIC_SIZE) == 0 ){
    ver = ((uint32_t)p[4] << 24) |
          ((uint32_t)p[5] << 16) |
          ((uint32_t)p[6] << 8)  |
          ((uint32_t)p[7]);
    if( pVersion ) *pVersion = ver;
    if( ver == FSS_DB_VERSION ){
      return FSS_MARKER_VALID;
    }else{
      return FSS_MARKER_UNSUPPORTED_VER;
    }
  }

  if( pVersion ) *pVersion = 0;
  return FSS_MARKER_INVALID;
}

/*
** Write the FSS database compatibility marker into the 100-byte SQLite header.
** Writes "FSS1" at offset 72 and the 32-bit big-endian version at offset 76.
** Returns FSS_OK on success, or FSS_ERROR if aDbHdr is null.
*/
int fssWriteDbMarker(uint8_t *aDbHdr, uint32_t version){
  uint8_t *p;
  if( !aDbHdr ) return FSS_ERROR;
  p = aDbHdr + FSS_DB_MARKER_OFFSET;
  memcpy(p, FSS_DB_MAGIC, FSS_DB_MAGIC_SIZE);
  p[4] = (uint8_t)((version >> 24) & 0xFF);
  p[5] = (uint8_t)((version >> 16) & 0xFF);
  p[6] = (uint8_t)((version >> 8) & 0xFF);
  p[7] = (uint8_t)(version & 0xFF);
  return FSS_OK;
}

/*
** Clear the FSS database compatibility marker in the 100-byte SQLite header (sets bytes 72-79 to 0).
** Returns FSS_OK on success, or FSS_ERROR if aDbHdr is null.
*/
int fssClearDbMarker(uint8_t *aDbHdr){
  if( !aDbHdr ) return FSS_ERROR;
  memset(aDbHdr + FSS_DB_MARKER_OFFSET, 0, FSS_DB_MARKER_SIZE);
  return FSS_OK;
}

/*
** Returns 1 if aDbHdr has the "FSS1" magic at offset 72, 0 otherwise.
*/
int fssHasDbMarker(const uint8_t *aDbHdr){
  if( !aDbHdr ) return 0;
  return (memcmp(aDbHdr + FSS_DB_MARKER_OFFSET, FSS_DB_MAGIC, FSS_DB_MAGIC_SIZE) == 0);
}

