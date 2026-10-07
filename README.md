# SQLite Fixed-Schema Storage (FSS) Leaf Page

A high-density data leaf page format for SQLite that stores the column schema **once per page** (rather than repeating type metadata inside every individual row) and **completely eliminates the per-page row pointer table** (`aCellIdx`) by using direct $O(1)$ fixed-slot addressing.

---

## 🚀 Key Benefit: Smaller Database Size (Up to ~20%)

The primary and practical benefit of Fixed-Schema Storage (FSS) is **smaller database file size (up to ~20% reduction)** for structured, fixed-width workloads.

> [!NOTE]
> **Performance Reality:** According to latest 1,000,000-row benchmarks, **FSS does not provide runtime performance gains over standard SQLite**. Execution throughput across INSERT (~0.91x), UPDATE (~0.92x), and SELECT (~1.01x) remains at near-parity. The sole motivation and concrete advantage of FSS is **storage compression and reduced disk footprint**.

### 1. Why Standard SQLite Databases Are Larger

Standard SQLite table leaf pages (`0x0D`) adhere to SQLite's signature "manifest typing." While this offers flexible typing, it comes at a steep storage cost: **every single row redundantly serializes its own type metadata**.

In standard SQLite:
- Every cell stores a payload length varint (1–4 bytes).
- Every cell stores a record header length varint (1–4 bytes).
- Every cell stores a **serial-type varint for every single column** (1–3 bytes per column).
- Every cell requires a 2-byte offset in the cell pointer array.

For a table with 6 columns, standard SQLite burns **10 to 18 bytes of pure metadata overhead per row**.

### 2. How FSS Achieves Up to ~20% Smaller Database Size

- **Schema Stored Once:** The field types, byte widths, and nullability flags are written exactly once in the page preamble (~14–20 bytes total for the whole 4KB page).
- **Zero Per-Row Serial Types:** Individual rows contain only raw binary data values (plus an optional null bitmask).
- **Elimination of the Row Offset Table (Cell Pointer Array):** Standard SQLite allocates a 2-byte integer offset (`aCellIdx`) for every row at the top of the page. FSS removes that table. Because FSS rows have a fixed, identical payload size, the byte location of row $i$ is calculated directly via pointer arithmetic:
  $$\text{RowOffset}(i) = \text{DataAreaOffset} + (i \times \text{RowPayloadSize})$$
  This eliminates SQLite's **2-byte cell-pointer entry per row**.
- **Net Storage Savings:** Databases storing structured or time-series data achieve **up to ~20% reduction in total database file size** (e.g., -17.21% space reduction on 1M IoT records).

| Scenario (4096-Byte Page) | Standard SQLite (`0x0D`) | Fixed-Schema Leaf (`0x0E`) | Storage Benefit |
|---|---|---|---|
| 3-Column Numeric Table (`INT32`, `DOUBLE`, `INT16`) | ~27 bytes / row | **22 bytes / row** | **~18% space saved** |
| 4-Column IoT/Sensor Table (Timestamp, ID, Float, Status) | ~29.5 bytes / row | **24.4 bytes / row** | **~17.2% space saved** |
| 6-Column IoT/Sensor Table (Timestamps, Floats) | ~48 bytes / row | **38 bytes / row** | **Up to ~20% space saved** |
| Rows Fitted per 4KB Page | ~85–140 rows | **105–170 rows** | **+20% to +25% capacity** |

---

### 3. Architectural Characteristics & Compatibility

Beyond storage density, the fixed-slot architecture provides specific structural properties:

- **$O(1)$ Direct Memory Addressing:** Field offsets within a slot are known statically, eliminating varint traversal loops per column.
- **Lower Page Footprint:** Because each page holds 20% to 25% more records, fewer pages reside on disk or in the pager cache.
- **Zero In-Page Fragmentation:** Fixed-width slots avoid freeblock fragmentation chains on updates.
- **100% Backward Compatibility & Graceful Degradation:** SQLite guarantees dynamic typing:
  - If incoming data matches the schema: fast-path FSS insert.
  - If incoming data **does not match** (e.g., text inserted into an integer column):
    - **Fits in page:** The page is automatically morphed in-place into a standard SQLite `0x0D` page ("data type per row").
    - **Exceeds page size:** A new `0x0D` page is allocated via standard SQLite B-tree balancing.
  - **No query or insert ever fails.**

---

## 📊 Summary Comparison

| Metric / Feature | Standard SQLite (`0x0D`) | Fixed-Schema Leaf (`0x0E`) |
|---|---|---|
| **Database File Size** | Baseline (repeats type tags every row) | **Up to ~20% smaller (-17.2% measured)** |
| **Runtime Performance** | Baseline (optimized dynamic format) | **Parity (~0.9x–1.0x; no performance gain)** |
| **Schema Representation** | Dynamic per row (varints) | Once per page header |
| **Row Offset Table (Cell Pointers)** | 2 bytes per row (`aCellIdx`) | **Eliminated** ($O(1)$ arithmetic calculation) |
| **Row Count Invariant** | 0 or more cells | **1 or more data rows** |
| **Column Projection** | $O(N)$ varint traversal per row | **$O(1)$ direct pointer arithmetic** |
| **Page Defragmentation** | Freeblock compaction may be needed | Fixed slots; no variable-cell freeblocks |
| **Dynamic Type Handling** | Native | **Graceful in-place demotion / split** |

---

## ⚙️ Enabling Fixed-Schema Storage (PRAGMA Syntax)

By default, Fixed-Schema Storage is **disabled (`OFF`)** to preserve strict standard SQLite page behavior.

To inspect or toggle the feature at runtime, use the following PRAGMA syntax:

```sql
-- Query current status (returns 0 for OFF, 1 for ON)
PRAGMA fixed_schema;
-- or using the alias:
PRAGMA fss;

-- Enable the feature
PRAGMA fixed_schema = ON;   -- also accepts 1, YES, TRUE
-- or using the alias:
PRAGMA fss = ON;

-- Disable the feature (default)
PRAGMA fixed_schema = OFF;  -- also accepts 0, NO, FALSE
-- or using the alias:
PRAGMA fss = OFF;
```

---

## 🛠️ Automated Build & Benchmark (`build.sh`)

The repository includes a turnkey `build.sh` script that:
1. Clones/checks out upstream SQLite at commit `d54522a5b832422ab893a1e9fa7a99ab314ea4e2`.
2. Applies the FSS patches (`patches/sqlite-fss.patch` and `src/fss.{c,h}`).
3. Re-generates `pragma.h` and compiles SQLite amalgamation and binaries.
4. Compiles standalone FSS unit tests, PRAGMA tests, and the 1,000,000-operation benchmark.
5. Executes the test suite and prints the executive benchmark summary.

### Running the Build:
```bash
./build.sh
```

---

## 📈 Benchmark Results (1,000,000 Records, Best of 3 Runs)

Workload: 1,000,000 structured IoT/Sensor records with schema `(timestamp INT64, sensor_id INT32, reading FLOAT64, status INT32)` inside an explicit transaction (`PRAGMA page_size = 4096`, `synchronous = OFF`, `journal_mode = MEMORY`).

| Metric | Standard SQLite (`0x0D`) | Fixed-Schema Storage (`0x0E`) | Benefit / Comparison |
|---|---|---|---|
| **Database File Size** | **29,536,256 bytes** | **24,453,120 bytes** | **-17.21% SPACE (-5,083,136 bytes saved)** |
| **Total 4KB Pages Allocated** | 7,211 pages | 5,970 pages (5,953 FSS leaves, 16 interior) | **-1,241 pages saved** |
| **INSERT Throughput** | ~3,222,000 ops/sec | ~2,932,000 ops/sec | **0.91x (Near parity; no throughput gain)** |
| **SELECT Point Lookups** | ~191,500 queries/sec | ~192,600 queries/sec | **1.01x (Parity; no throughput gain)** |
| **UPDATE In-Place Throughput** | ~3,030,000 updates/sec | ~2,782,000 updates/sec | **0.92x (Near parity; no throughput gain)** |
| **Data Consistency** | 1,000,000 rows | 1,000,000 rows | **100% Match Verified (`integrity_check` ok)** |

> [!IMPORTANT]
> **Key Takeaway:** As shown above, the sole concrete advantage of Fixed-Schema Storage is **smaller database size (up to ~20% reduction)**. Performance benchmarks demonstrate that FSS does **not** provide throughput speedups over standard SQLite; throughput remains at near parity (~0.9x–1.0x).

---

## ⚙️ Direct Streaming Pipeline (Minimizing Overhead)

To eliminate intermediate allocation overhead and stay on near-parity with standard SQLite's highly optimized engine, FSS incorporates a direct write pipeline:

1. **Direct Single-Pass Streaming (`fssRecordToSlot`):**
   - Eliminates intermediate `FssValue` array allocations. SQLite dynamic varint records stream directly into on-page fixed-width slots.
   - Fast-paths single-byte serial varints (`!(pRecord[off] & 0x80)`) and directly reads 1-, 2-, 4-, and 8-byte integers with zero-iteration bitwise ops.
   - Only zeroes the null bitmask header instead of zeroing the full slot buffer on each write.

2. **$O(1)$ Sequential Dense Append:**
   - Detects sequential rowid increments (`rowid == min_rowid + cell_count`) and streams directly into slot `cell_count` without rowid search or page re-indexing.

3. **Direct Overwrite on UPDATE:**
   - Targets the current cursor slot `pCur->ix` directly without full-page re-parsing or redundant binary searches.

4. **Lightweight Header & Schema Extraction (`fssFastSchemaExtract`, `fssPageQuickParse`):**
   - Replaces multi-kilobyte struct `memset`s with inline column descriptor iteration (~5 nanoseconds), bypassing redundant integrity checks on known valid pages during runtime writes.

---

## 📖 Detailed Specification

For complete binary layout offsets, header field structures, wire-format specifications, and demotion state-machine details, see [DESIGN.md](file:///home/boris/projects/sqlite-fss/DESIGN.md).
