You are absolutely right. Missing deterministic canonicalization is the exact kind of trap that turns a "perfectly synced" system into a nightmare of hash mismatches and infinite re-uploads.

If the filesystem traversal order or timestamp metadata changes the archive's byte layout, the entire `xxHash32` checkpoint ledger invalidates itself, destroying the deduplication and content-addressed storage capabilities. Canonicalizing the ZIP generation is a mandatory requirement.

Furthermore, renaming it from "OMPK-ZIP" to **"Deterministic ZIP(STORE) Streaming Profile"** is a critical communication fix. It signals to developers and users that their data is not trapped in a proprietary format.

Here is the finalized specification, integrating your canonicalization and naming improvements to achieve a true content-addressed architecture.

---

# OmniSave V1: Deterministic ZIP(STORE) Streaming Profile

## 1. Architectural Objective

Define a custom, highly constrained archive generation and extraction engine for the Nintendo Switch sysmodule. The objective is to produce a **100% standard-compliant `.zip` file** using a deterministic, streaming writer. This guarantees interoperability with standard PC tooling while strictly enforcing the zero-heap, low-CPU, and deterministic byte-layout constraints required by the sysmodule and the broader content-addressed sync architecture.

## 2. Core Constraints & Memory Profile

The sysmodule MUST adhere to strict embedded systems programming paradigms during the archival phase.

* **The Zero-Heap Rule:** The engine MUST NOT use dynamic memory allocation. All buffers and metadata arrays must be statically allocated at initialization.
* **Compression Ban:** The engine MUST strictly use the ZIP `STORE` method (Compression Method 0). `Deflate` is strictly prohibited.
* **Bounded I/O Buffer:** File streaming MUST utilize a single, fixed-size static buffer (e.g., 64KB or 128KB).
* **Bounded Metadata Array:** Central Directory metadata MUST be tracked via a statically sized array, establishing an upper bound on file count per save.

## 3. Deterministic Canonicalization (Content-Addressing Rule)

To enable deduplication, immutable snapshot storage, and stable hashing, identical save states MUST produce bit-for-bit identical ZIP archives. The sysmodule MUST enforce the following canonicalization rules before writing any archive headers.

### 3.1 Lexicographical Path Ordering

The sysmodule MUST enumerate the save filesystem, normalize the paths, and sort the entire file list lexicographically. Files MUST be streamed into the archive in this exact sorted order.

### 3.2 Metadata Normalization

All ZIP headers (Local File Header and Central Directory) MUST strictly normalize environmental variables:

* **Path Separators:** All relative paths MUST use forward slashes (`/`).
* **Encoding:** All filenames MUST be explicitly encoded as UTF-8.
* **Timestamps:** All file modification/creation timestamps MUST be zeroed or set to a fixed canonical constant (e.g., Unix Epoch `0` / Jan 1, 1970).
* **Extended Attributes:** OS-specific extended attributes (e.g., permissions, symlink data) MUST be stripped or set to deterministic POSIX defaults.

## 4. The Write Pipeline (Archival & Staging)

The system uses a streaming ZIP topology with **LFH CRC back-patching** (flags = 0, no Data Descriptor). Sizes are known at enumeration time (from `FsDirectoryEntry.file_size`), so Data Descriptors are unnecessary.

### Step 1: Sequential File Processing

Iterating through the **lexicographically sorted** file list:

1. **Write Local File Header:** Write CRC-32 as `0` (placeholder). Write the known Compressed and Uncompressed sizes from enumeration. General Purpose Bit 3 is NOT set. Apply canonical metadata (flags=0, method=0, zeroed timestamps).
2. **Stream Data:** Read the source file in chunks matching the bounded I/O buffer. Pass each chunk through a rolling CRC-32 calculator, then write the chunk directly to `tmp_out/save.zip`.
3. **Back-patch CRC:** After streaming the file, seek back to `lhdr_off + 14` and write the finalized CRC-32 into the Local File Header in-place.
4. **Cache Metadata:** Store the normalized file path, CRC-32, sizes, and `lhdr_off` in the bidirectional arena (entries from front, path strings from back; collision = PACK_LIMIT_EXCEEDED).

### Step 2: Finalization

1. **Write Central Directory:** Iterate through the bounded metadata array and write the Central Directory Headers.
2. **Write EOCD:** Write the End of Central Directory record to terminate the archive.

## 5. The Read Pipeline (Delivery & Extraction)

During the `DELIVERING` phase, the sysmodule extracts the fully mathematically validated `tmp_in/save.zip` into the live save mount.

* **Sequential Unpacking:** The extraction engine MUST read the archive linearly from byte 0.
* **Header Parsing:** Read the Local File Header to determine the file name and expected size.
* **Stream to Mount:** Stream the file bytes directly into the live Horizon OS save mount using the bounded I/O buffer.
* **Forward-Only Stream:** Because the network pipeline (`checkpoint_ledger`) has already mathematically proven the entire archive blob is uncorrupted, the extraction engine MAY safely ignore reading the Central Directory, operating strictly as a forward-only parser.

## 6. Transport Pipeline Integration

* **Opaque Blob Rule:** The network layer (`/range` endpoints, `verified_bytes`, and the `checkpoint_ledger`) treats `save.zip` as a single, opaque binary blob.
* **Symmetric Hashing:** The `xxHash32` checkpoints generated by the server are calculated against the *finalized, canonical ZIP archive bytes*, NOT the unarchived files.
* **Content-Addressed Foundation:** Because the ZIP generation is strictly deterministic, the server CAN rely on the archive's top-level hash to accurately identify conflicts, skip unchanged uploads, and deduplicate storage without inspecting the interior files.