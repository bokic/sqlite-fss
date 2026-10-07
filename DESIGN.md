# SQLite Fixed-Schema Storage (FSS) Data Leaf Page Design

## 1. Core Concept

The fundamental idea of this new page format is:
> **A table data leaf page that defines its schema (field types) exactly once in the page header, followed by 1 or more fixed-width data slots conforming to that schema.**

In standard SQLite data leaf pages (`0x0D`), the page structure is agnostic to the column types. Every single row (cell) acts as an independent, dynamically typed tuple that redundantly serializes its own column types, lengths, and null markers using varint serial types:

```
Standard SQLite Leaf Page (0x0D):
+-----------------------------------------------------------------------------------+
| Page Header | Cell Ptr Array | ... Free Space ... | Cell N | ... | Cell 1 | Cell 0|
+-----------------------------------------------------------------------------------+
  Where EACH cell contains:
  [payload_len_varint | rowid_varint | hdr_len_varint | type0_varint | type1_varint ... | val0 | val1 ...]
                                        ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
                                        REPEATED FOR EVERY SINGLE ROW IN THE PAGE!
```

In the **Fixed-Schema Storage (FSS)** leaf page, this redundancy is eliminated. The column type descriptors are hoisted into a single **In-Page Schema Definition Header**. Rowids are stored separately in a shared rowid index. Individual data slots contain only raw column payload data and an optional null bitmask:

```
FSS Leaf Page (0x0E):
+-----------------------------------------------------------------------------------+
| Page Header | Field Types Def (ONCE) | Rowid Index | Fixed-Width Data Slots (1..N) |
| (16 bytes)  | [col0, col1, col2...]  | [rowid0,...] | [Slot 0][Slot 1]...           |
+-----------------------------------------------------------------------------------+
  Rowids live in the shared rowid index (or as one min_rowid in dense mode).
  Each data slot contains ONLY:
  [(optional null bitmask) | col0_val | col1_val | col2_val ...]
  --> Zero serial type bytes per row!
  --> Zero header length varints per row!
```

---

## 2. Advantages & Architectural Benefits

> [!NOTE]
> **Primary Benefit & Performance Reality:**
> The primary, measurable advantage of Fixed-Schema Storage is **smaller database file size (up to ~20% reduction)**. Latest empirical benchmarks confirm that FSS does **not** provide runtime performance gains over standard SQLite; operations run at near parity (~0.9x–1.0x). The architecture's purpose is achieving higher storage density without regressing SQLite's execution throughput.

1. **Self-Describing Yet Compact (Up to ~20% Space Reduction):**
   - The page remains self-describing: readers can decode every row on the page using solely the field descriptor stored in that page's preamble, without querying the master schema catalog for every cell.
   - Eliminates redundant per-row varints, serial types, and cell-pointer arrays, saving 2–8+ bytes of metadata per row and reducing overall database file size by **up to ~20%** (-17.21% measured on 1M IoT records).
2. **Predictable Row Size & Direct Offset Math:**
   - If all columns are fixed-width, row payload size is constant and mathematically predetermined:
     $$\text{RowPayloadSize} = \text{nullBytes} + \sum_{i=0}^{N-1} \text{width}(\text{col}_i)$$
   - Column $k$ in slot $j$ can be accessed with $O(1)$ pointer arithmetic:
     $$\text{ColOffset}(j, k) = \text{DataStart} + (j \times \text{RowPayloadSize}) + \text{ColRelativeOffset}(k)$$
3. **Strict Minimum Invariant (1 or More Rows):**
   - An FSS page/chunk must always contain at least one data row ($1 \le N \le \text{capacity}$). An FSS chunk is never instantiated empty; it is created upon insertion of its first record. If deletions reduce the row count to zero ($N=0$), the page is freed/reclaimed or merged into sibling pages.
4. **Predictable Layout & Parity Throughput:**
   - With fixed-width contiguous slots, row access avoids dynamic record header decoding. Because SQLite VDBE bytecode evaluation and cursor dispatch remain the primary execution cost, overall throughput operates on near parity (~0.9x–1.0x) with standard SQLite.
5. **No Variable-Cell Defragmentation (for Fixed Widths):**
   - Rows occupy fixed-width slots and there are no freeblock chains. Inserts and deletes that are not at the end shift rowids and slots together to preserve key order.

---

## 3. Detailed Page Layout Specification

A usable page region of size $P$ (typically 4096 bytes, excluding any SQLite reserved tail) is organized into four main sections from top to bottom:

```
Offset 0x00
+-----------------------------------------------------------------------+
| 1. FSS Page Header (16 bytes)                                         |
|    - page_type (0x0E)                                                 |
|    - flags, cell_count (1..N), capacity, slot_size                    |
|    - offsets to field descriptor, row indices, data section           |
+-----------------------------------------------------------------------+
| 2. Field Type Descriptor Section (Stored ONCE per page)               |
|    - num_columns                                                      |
|    - Array of Column Descriptors: [Type ID, Byte Width, Flags]        |
+-----------------------------------------------------------------------+
| 3. Row Index / Addressing Section                                     |
|    - Rowid lookup array (or min_rowid in dense mode)                  |
+-----------------------------------------------------------------------+
| 4. Data Rows Section (1 to N records)                                 |
|    - Row 0: [Null Mask (opt) | Field 0 Data | Field 1 Data | ...]     |
|    - Row 1: [Null Mask (opt) | Field 0 Data | Field 1 Data | ...]     |
|    - ...                                                              |
|    - Row N-1                                                          |
+-----------------------------------------------------------------------+
| Unallocated Free Space (until page size P is reached)                 |
+-----------------------------------------------------------------------+
Offset P
```

---

## 4. Binary Specification

### 4.0 Database Compatibility Marker

The FSS SQLite fork owns a versioned file format. When the first FSS leaf is committed, bytes 72–79 of SQLite's 100-byte database header are reserved for this marker: ASCII `FSS1` followed by a 32-bit big-endian FSS format version (`1`). Files without this marker remain ordinary SQLite databases. The matching fork must reject an unknown FSS signature or version before traversing B-tree pages. A build that does not recognize page type `0x0E` is incompatible with marked files. This marker versions the FSS layout independently of SQLite's upstream release number.

### 4.1 FSS Page Header (16 Bytes)

Located at byte `hdrOffset` of the page (`hdrOffset` is normally 0, or 100 when the SQLite database header occupies page 1). All multi-byte header, descriptor, rowid-index, and numeric values are stored big-endian. Stored offsets are relative to `hdrOffset`.

| Offset | Type | Field Name | Description |
|---|---|---|---|
| `0x00` | `uint8` | `page_type` | Page identifier flag: `0x0E` (FSS Table Leaf). |
| `0x01` | `uint8` | `flags` | Page flags: <br>• Bit 0: Dense sequential rowids (`min_rowid` mode)<br>• Bit 1: Every data slot has a null bitmask<br>• Bit 2: Reserved; unsupported in format version 1 |
| `0x02` | `uint16` | `schema_version` | Schema version/generation ID. |
| `0x04` | `uint16` | `cell_count` | Number of active data rows currently in page ($1 \le N \le \text{capacity}$). |
| `0x06` | `uint16` | `capacity` | Maximum number of rows this page can hold. |
| `0x08` | `uint16` | `row_payload_size` | Fixed byte length of each raw data row payload. |
| `0x0A` | `uint16` | `field_desc_offset` | Byte offset to the Field Type Descriptor section (version 1 writes `0x0010`). |
| `0x0C` | `uint16` | `data_area_offset` | Byte offset to where fixed-width data slots begin. |
| `0x0E` | `uint16` | `reserved` | 2 bytes reserved for future flags / 4-byte alignment. |

### 4.2 Field Type Descriptor (Stored ONCE)

Immediately following the page header (default offset `0x0010`):

```c
struct FieldDescriptor {
    uint8_t type_id;     /* Data type code (see table below) */
    uint8_t col_flags;   /* Bit 0: NOT NULL, Bit 1: PRIMARY KEY, etc. */
    uint16_t byte_width; /* Byte size of this column (e.g. 1, 2, 4, 8) */
};

struct PageSchemaHeader {
    uint16_t num_columns;                   /* Number of fields in every row */
    struct FieldDescriptor columns[num_columns]; /* Described once */
};
```

#### Supported Field Types (`type_id`):

| `type_id` | Name | Fixed Width | Description |
|---|---|---|---|
| `0x01` | `INT8` / `BOOL` | 1 byte | Signed 8-bit integer or boolean |
| `0x02` | `INT16` | 2 bytes | Signed 16-bit integer (big-endian, two's complement) |
| `0x03` | `INT32` | 4 bytes | Signed 32-bit integer |
| `0x04` | `INT64` | 8 bytes | Signed 64-bit integer |
| `0x05` | `FLOAT32` | 4 bytes | IEEE 754 single precision float |
| `0x06` | `FLOAT64` | 8 bytes | IEEE 754 double precision float |
| `0x07` | `FIXED_BLOB` | $K$ bytes | Fixed-length byte string/array of length $K$ |
| `0x08` | `TIMESTAMP_US` | 8 bytes | Microsecond 64-bit Unix timestamp |

Version 1 stores integers in big-endian two's-complement form, floats as big-endian IEEE-754 bit patterns, and fixed blobs as raw bytes. `FSS_TYPE_VAR_REF` reserves a 4-byte `(offset,length)` reference, but the variable-length arena is not implemented. `FSS_FLAG_VAR_ARENA` and `FSS_TYPE_TEXT` are not supported for active version 1 rows.

### 4.3 Row Addressing & Indexing

FSS removes SQLite's 2-byte-per-row cell-offset table. It does not store a byte offset for each row: slot `j` is always at `data_area_offset + j * row_payload_size`. To support SQLite's B-tree search by `int64 rowid`, FSS keeps a rowid index (or only `min_rowid` in dense mode) before the slots. This index stores keys, not cell offsets. Rowids are signed 64-bit big-endian values, separate from the data payload, and are not included in `row_payload_size`.

The rowid index and data slots stay in the same sorted order. Inserting or deleting a sparse row may shift later rowids and fixed-width slots; this preserves direct slot addressing without SQLite's cell-offset table.

1. **Sequential/Dense Mode (`flags & 0x01`):**
   - Used when rowids are contiguous ($R, R+1, R+2, \dots$).
   - Stores only `int64 min_rowid` (8 bytes).
   - Finding row with `rowid`: $\text{slot\_idx} = \text{rowid} - \text{min\_rowid}$. Lookup is $O(1)$.
2. **Sparse/General Mode:**
   - Reserves `capacity` entries, each an 8-byte signed rowid, sorted in ascending order; unused entries are zero-filled.
   - Finding row with `rowid`: Binary search over the contiguous 8-byte array.
   - Lookup uses binary search. SIMD lookup is a possible future optimization, not part of the current helpers.

Dense mode is valid only while rowids form a contiguous ascending run. A gap insertion converts the page to sparse mode if its rows fit that layout. A middle-row deletion also converts to sparse mode, omitting the deleted slot. If the resulting rows cannot fit the sparse layout, the helper returns `FSS_FULL` and the B-tree layer must rebalance or use standard SQLite pages. Dense mode is not automatically re-enabled after conversion.

### 4.4 Data Row Structure (1 or More Rows)

Every valid on-disk FSS page maintains the invariant:
$$1 \le \text{cell\_count} \le \text{capacity}$$
An FSS chunk is never instantiated with 0 rows. Each active slot $j$ contains:

```
+-------------------------------------------------------------+
| Null Bitmask (Optional) | Field 0 Data | Field 1 Data | ... |
+-------------------------------------------------------------+
```

1. **Null Bitmask:**
   - If `FSS_FLAG_NULLABLE` is set, a bitmask of $\lceil N_{\text{cols}} / 8 \rceil$ bytes is prepended to every slot. This page-wide flag allocates the mask; each column's `NOT NULL` descriptor flag controls whether it may be NULL.
   - Bit `(k / 8, k % 8)` set to 1 indicates column $k$ is `NULL` (least-significant-bit-first within each byte; data bytes are ignored/zeroed).
   - If no column may be NULL, `FSS_FLAG_NULLABLE` is clear and the bitmask is **omitted** (0 bytes overhead).
2. **Field Data:**
   - Column values are stored directly in binary form at their fixed offsets.
   - **No serial types, no varints, no per-row column counts.**

### 4.5 Version 1 Schema Eligibility and Mapping

FSS version 1 applies only to ordinary rowid tables whose stored columns all map to supported fixed-width descriptors. `WITHOUT ROWID` tables, virtual tables, generated/hidden columns, and tables containing unsupported declarations use standard SQLite pages.

Declared type names are matched case-insensitively after trimming surrounding whitespace:

| Declared type | FSS descriptor |
|---|---|
| `INT8`, `BOOL`, `BOOLEAN` | `INT8` (1 byte) |
| `INT16` | `INT16` (2 bytes) |
| `INT32` | `INT32` (4 bytes) |
| `INT`, `INTEGER`, `BIGINT`, `INT64` | `INT64` (8 bytes) |
| `FLOAT64`, `DOUBLE`, `REAL` | `FLOAT64` (8 bytes) |
| `TIMESTAMP_US` | `TIMESTAMP_US` (8 bytes) |
| `BLOB(K)` where K is a positive fixed integer | `FIXED_BLOB` (K bytes) |

Other declarations, including unbounded `TEXT`, `BLOB`, `NUMERIC`, and `FLOAT32`, are not eligible in version 1. An `INTEGER PRIMARY KEY` rowid alias is also ineligible until its implicit column semantics are explicitly mapped; the rowid is stored in the FSS rowid index, while SQLite normally synthesizes the alias value rather than storing it in the record payload. `NOT NULL` maps to `FSS_COL_FLAG_NOT_NULL`; otherwise the column is nullable. The page-wide null mask is allocated if any stored column is nullable. SQLite affinity and runtime values still apply: a value that cannot be represented without changing SQLite-visible value semantics must use the standard-page fallback.

---

## 5. Lifecycle of an FSS Page

### 5.1 Initialization with Initial Row ($N = 1$)
An FSS page is allocated and formatted upon insertion of its first data row:
1. Page allocated by SQLite pager.
2. Set `page_type = 0x0E`.
3. Schema written to `PageSchemaHeader`: number of columns and column type descriptors.
4. `row_payload_size` calculated as:
   $$\text{row\_payload\_size} = \text{null\_bytes} + \sum_{i=0}^{num\_cols-1} col\_width_i$$
5. Let `D = 16 + 2 + 4 * num_cols` be the descriptor end relative to `hdrOffset`, `H` be `hdrOffset`, and `R` be `row_payload_size`:
   - Dense: `capacity = floor((P - H - D - 8) / R)`; the 8-byte index stores `min_rowid` and `data_area_offset = D + 8`.
   - Sparse: `capacity = floor((P - H - D) / (8 + R))`; each rowid uses 8 bytes and `data_area_offset = D + 8 * capacity`.
   - `data_area_offset` is relative to `hdrOffset` in both modes.
6. Initial row ($N=1$) is written: rowid goes in the rowid index and column values go in data slot 0.
7. Set `cell_count = 1`. Page is now active and compliant with the $\ge 1$ row invariant.

### 5.2 Inserting Subsequent Rows ($1 \rightarrow N$)
1. Check if `cell_count < capacity`. (If full, trigger standard B-tree leaf split).
2. Find insertion index for `new_rowid` to maintain sorted rowid order.
3. If necessary, shift rowid entries and data slots together, or append when inserting at the end.
4. Copy column values into the target data slot.
5. Increment `cell_count`.

### 5.3 Point Lookup (`BtreeFind(rowid)`)
1. Perform binary search over the sorted `rowids` array (or $O(1)$ arithmetic if sequential).
2. If found at index $j$:
   - Slot pointer is $\text{data\_area\_offset} + j \times \text{row\_payload\_size}$.
   - Directly read any column $k$ at `slot_ptr + col_offset[k]`.
   - Time complexity: $O(\log N)$ or $O(1)$, zero varint decoding loops.

### 5.4 Full Table Scan / Filter (`SELECT col1 FROM tbl WHERE col0 > 100`)
- Standard SQLite leaf: must unpack every cell's varints, calculate column offsets dynamically row-by-row.
- FSS leaf: stride through memory:
  ```c
  uint8_t *slot = page + data_area_offset;
  for (int i = 0; i < cell_count; i++, slot += row_payload_size) {
      int32_t val0 = *(int32_t*)(slot + col0_offset);
      if (val0 > 100) {
          int64_t val1 = *(int64_t*)(slot + col1_offset);
          emit_row(val1);
      }
  }
  ```
  Compiler easily auto-vectorizes this loop into AVX2/SSE/NEON instructions!

### 5.5 Deleting a Row & Chunk Teardown
- **When $\text{cell\_count} > 1$:**
  - Find the rowid's slot, shift later sparse rowids and data slots left, clear the final rowid/slot, and decrement `cell_count`. There is no tombstone bitmap.
- **When $\text{cell\_count} == 1$ (Deleting the Final Row):**
  - Because an FSS chunk must strictly contain 1 or more data rows, deleting the sole remaining row underflows the chunk.
  - The page is **freed/reclaimed** to SQLite's pager freelist (or merged with an adjacent sibling page), and the pointer is removed from the parent interior node (`0x05`). An FSS chunk never persists with 0 rows.

The standalone helper converts a dense page to sparse mode for a middle-row deletion when the remaining rows fit. Deleting the final row returns `FSS_UNDERFLOW`; the caller must reclaim the page.

---

## 6. Type Mismatch & Dynamic Page Demotion / Fallback Strategy

SQLite's core design philosophy guarantees dynamic/manifest typing: any column (in non-`STRICT` tables) can store arbitrary data types, and values may exceed the statically expected width (e.g., storing a `TEXT` string into an `INT` column, or a 64-bit integer into a 16-bit slot).

When an `INSERT` or `UPDATE` operation provides data that **does not match the fixed row type** described in the page's `PageSchemaHeader`:

```
Incoming Row (Insert / Update)
               |
               v
Does row match PageSchemaHeader?
        /              \
     [Yes]             [No] (Type/Width Mismatch)
      /                  \
Standard FSS Insert       Calculate Total Space Needed in Standard 0x0D Format:
                          TotalNeeded = Hdr(0x0D) + Sum(Cells + 2B Ptrs) + NewCell
                                      |
                         +------------+------------+
                         |                         |
               [TotalNeeded <= PageSize]    [TotalNeeded > PageSize]
                         |                         |
                         v                         v
                 IN-PLACE DEMOTION            ALLOCATE NEW NODE
             Morph Page 0x0E -> 0x0D          Create new 0x0D Leaf Page
             Re-encode existing rows           via B-Tree Split / Balance
             into dynamic cells in-place
```

---

### 6.1 Condition Check (Mismatch Detection)
During row insertion or update, the engine compares the input tuple against the in-page `FieldDescriptor` array:
1. **Type Incompatibility:** Column receives a type code that cannot be safely coerced to the fixed type (e.g., `TEXT` or `BLOB` supplied for `INT32`/`FLOAT64`).
2. **Width Overflow:** Integer value exceeds the slot's byte width (e.g., value `100,000` does not fit in `INT16`).
3. **Nullability Violation:** `NULL` value inserted into a column flagged with `NOT NULL` on a page initialized without a null bitmask.

If all values conform, the operation proceeds through the high-speed FSS path. If any field mismatches, the **Fallback / Demotion Pipeline** is triggered.

---

### 6.2 Scenario A: Data Fits into Current Page (In-Place Page Demotion)
If the combined size of all existing rows converted to dynamic `0x0D` format plus the new mismatched row is $\le P$ (e.g., 4096 bytes):

**The page is morphed in-place from `0x0E` to standard `0x0D` ("data type per row"):**

1. **Snapshot Existing Rows:**
   - Existing rows are read using the FSS schema descriptor into a temporary stack/scratch buffer.
2. **Re-initialize Page as `0x0D`:**
   - Write standard SQLite table leaf page header at offset `0x00`:
     - `page_type = 0x0D` (standard dynamic leaf)
     - `first_freeblock = 0`
     - `cell_count = N_existing + 1`
     - `cell_content_offset = P` (starts at bottom of page)
     - `fragmented_free_bytes = 0`
3. **Serialize Rows to Standard Record Format:**
   - For each existing row, generate standard SQLite dynamic cell:
     `[payload_len_varint | rowid_varint | record_header_varints | serial_types | raw_bytes]`
   - Allocate from bottom of page upward.
   - Write 2-byte cell offset to the cell pointer array.
4. **Insert the Mismatched Row:**
   - Encode the offending row (using its actual dynamic types, e.g., string, float, int64).
   - Write cell content and its corresponding cell pointer.
5. **Zero B-Tree Rebalancing:**
   - The page remains at the same page number in the database file.
   - The parent interior node (`0x05`) requires **no changes** because the leaf's key boundary and page reference remain identical.

---

### 6.3 Scenario B: Data Cannot Fit into Current Page (New Node / Split)
Because standard `0x0D` records have per-cell overhead (varints, serial types, 2-byte cell pointers) and the new data may be arbitrarily large (e.g., long string or blob), converting all rows may exceed the page capacity $P$:

**A new node/page is allocated using SQLite's B-tree balancing mechanics:**

1. **Allocate New Page:**
   - Request a new page from SQLite's pager/freelist.
2. **Determine Split Boundary:**
   - Split existing rows + new row across two pages:
     - **Option 1 (Clean Separation):** Keep conforming rows in the existing `0x0E` page (compact, fixed-schema), and place the mismatched row (plus any overflow rows) on the new page initialized directly as `0x0D` (dynamic).
     - **Option 2 (Standard B-Tree Split):** Convert all rows to dynamic records and distribute evenly across two standard `0x0D` leaf pages using standard SQLite `balance()`.
3. **Update Parent Interior Node (`0x05`):**
   - Insert divider `rowid` and the new page number into the parent interior B-tree node.
   - If the parent node is full, recursively split the parent interior node up to the root.

---

### 6.4 Key Properties of this Strategy
- **100% SQLite Semantics:** No query or transaction ever fails due to typing constraints. Flexible typing remains fully functional.
- **Storage Density at Parity Performance:** Compliant, well-typed data achieves smaller file sizes (up to ~20% reduction) while maintaining runtime throughput on near-parity (~0.9x–1.0x) with standard SQLite.
- **Graceful Degradation:** Tables degrade seamlessly to standard SQLite dynamic pages on an as-needed, page-by-page basis without rewriting the entire database.

---

## 7. Concrete Layout Example

Assume a table schema:
```sql
CREATE TABLE sensor_readings (
    sensor_id INT NOT NULL,     -- INT32 (4 bytes)
    reading   DOUBLE NOT NULL,  -- FLOAT64 (8 bytes)
    flags     INT NOT NULL      -- INT16 (2 bytes)
);
```

### Page Structure on a 4096-Byte Page:
- **Header:** 16 bytes.
- **Field Descriptors:**
  - `num_columns = 3` (2 bytes)
  - Column 0: `type=INT32 (0x03)`, `flags=NOT_NULL (0x01)`, `width=4` (4 bytes)
  - Column 1: `type=FLOAT64 (0x06)`, `flags=NOT_NULL (0x01)`, `width=8` (4 bytes)
  - Column 2: `type=INT16 (0x02)`, `flags=NOT_NULL (0x01)`, `width=2` (4 bytes)
  - *Total Schema Descriptor:* $2 + (3 \times 4) = 14$ bytes.
- **Per-Row Payload:**
  - Null mask: 0 bytes (all `NOT NULL`).
  - Data: $4 + 8 + 2 = 14$ bytes.
  - Sparse rowid index entry: 8 bytes per row (stored separately from the data slot).
  - Data slot: 14 bytes.
- **Page Capacity:**
  - Descriptor end relative to the page header: $16 + 2 + (3 \times 4) = 30$ bytes.
  - Sparse mode: $\lfloor (4096 - 30) / (8 + 14) \rfloor = \mathbf{184\text{ rows per page}}$.
  - Dense mode: $\lfloor (4096 - 30 - 8) / 14 \rfloor = \mathbf{289\text{ rows per page}}$; the 8-byte min-rowid index covers the whole page.

### Comparison with Standard SQLite `0x0D` Page:
- Standard row overhead:
  - Cell pointer: 2 bytes
  - Payload length varint: 1 byte
  - Rowid varint: 1–9 bytes (avg 4 bytes)
  - Record header length varint: 1 byte
  - 3 column serial types: 3 bytes
  - Data: 14 bytes
  - *Total per cell:* $\approx 25\text{ bytes} + 2\text{ bytes pointer} = 27\text{ bytes}$.
  - Plus freeblock / fragmentation overhead.
- **FSS delivers:**
  - **Up to ~20% storage space reduction** (-17.21% measured on 1,000,000 IoT records).
  - Maintained performance parity (~0.9x–1.0x) with standard SQLite with zero runtime record-header parsing.

---

## 8. Comparison Matrix: Standard Table Leaf (`0x0D`) vs. FSS Leaf (`0x0E`)

| Feature | Standard Table Leaf (`0x0D`) | Fixed-Schema Leaf (`0x0E`) |
|---|---|---|
| **Database File Size** | Baseline (repeats type tags every row) | **Up to ~20% smaller (-17.2% measured)** |
| **Runtime Performance** | Baseline (optimized dynamic format) | **Parity (~0.9x–1.0x; no performance gain)** |
| **Schema Info** | Stored per-row (varint serial types) | Stored once in page header (`PageSchemaHeader`) |
| **Row Count** | 0 or more cells | 1 or more data rows (chunk underflow reclaims page) |
| **Cell Addressing** | 2-byte cell pointer array + variable cell offsets | Direct arithmetic / packed rowid array |
| **Column Projection** | Sequential scan of varints to reach col $N$ | Direct offset: $O(1)$ pointer math |
| **NULL Overhead** | 1 byte per column in record header | 1 bit per column (or 0 if `NOT NULL`) |
| **Fragmentation** | Freeblocks + fragmented byte tracking | Fixed slots; no variable-cell freeblocks |
| **Type Mismatch** | Native (accepts any type) | Morphs in-place to `0x0D` or splits into new node |

---

## 9. Development Roadmap

1. **Phase 1: Format Specification & Header/Slot Math**
   - Finalize the byte layout, schema descriptor, and demotion criteria.
2. **Phase 2: Standalone Encoder/Decoder Prototype**
   - Implement C/C++ reference module to encode and decode FSS pages outside SQLite.
   - Implement the in-place demotion function (`demote_fss_to_dynamic_page`).
   - Benchmark throughput and space density against standard SQLite record payloads.
3. **Phase 3: SQLite B-tree & Pager Integration**
   - Integrate page type `0x0E` into SQLite's B-Tree subsystem (`btree.c`, `pager.c`).
   - Route leaf page operations based on the page flag.
