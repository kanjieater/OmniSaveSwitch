Understood. Here is a strict, logic-only specification document designed for an agentic coder. It defines the exact architectural constraints, operational order, and failure handling without containing a single line of implementation code.

---

# OmniSave Restore Semantics - Agentic Planning Specification

## 1. Architectural Objective

Transition the OmniSave injection pipeline from a "merge-overwrite" semantic to a "full materialization" semantic. The system must deterministically clear the root of the live save data mount immediately before unpacking the incoming archive. This operation must execute entirely within the existing OS-level save data journal boundary to guarantee crash-safety and rollback capabilities.

## 2. Target Scope

* **Primary File:** `source/save_ops.cpp`
* **Primary Function to Modify:** The main injection routine.
* **Strict Boundary:** Do not modify the archive packing schema or the FSM logic.

## 3. Core Constraints & Safety Invariants

* **No Namespace Recreation:** Do not destroy and recreate the save data space via high-level OS APIs. The reset must happen via directory iteration inside the existing, open mount.
* **Iterator Stability:** Nintendo's filesystem iterators are not guaranteed to remain stable if the underlying directory is mutated during traversal. The purge logic must materialize all targets before executing deletions.
* **Transaction Integrity:** The purge operation must fall under the same deferred-commit umbrella as the unpack operation. Any failure must trigger an immediate abort, bypassing the final commit to ensure the OS drops the journaled changes.

## 4. Execution Plan

### Phase 1: Construct the Deterministic Purge Helper

Create a new static helper function above the main injection routine designed to empty the root of the save filesystem.

**Required Logic Flow:**

1. Open the root directory (`/`) of the provided filesystem handle for both files and directories.
2. Declare an array using the existing batch size macro to hold the directory entries.
3. Read the contents of the root directory into this array.
4. **Critical:** Close the directory handle immediately after reading. Do not proceed to deletion while the handle remains open.
5. Initialize a success tracker.
6. Iterate over the populated array:
* Construct an absolute path for the target by prepending a forward slash to the current entry's name.
* Evaluate the entry type.
* If it is a directory, invoke the existing recursive directory deletion API.
* If it is a file, invoke the standard file deletion API.
* If any deletion API returns a failure code, mark the success tracker as false.


7. Return the final boolean success state.

### Phase 2: Inject the Purge Pipeline

Integrate the new helper into the main injection routine to enforce the clean-slate invariant.

**Required Logic Flow:**

1. Locate the segment in the injection routine immediately after the save filesystem is opened and optionally extended, but strictly *before* the archive unpacking function is invoked.
2. Invoke the new purge helper, passing the active save filesystem handle.
3. Evaluate the result of the purge operation.
4. **Short-Circuit Requirement:** * If the purge succeeds, proceed to invoke the archive unpacking function.
* If the purge fails, the unpacking function must *not* be executed.


5. Evaluate the combined success of the pipeline. The final transaction commit must only be executed if *both* the purge and the unpack operations completed successfully.
6. Ensure the filesystem handle is closed and the routine exits, returning success only if the entire atomic block succeeded.

## 5. Acceptance Criteria

* **Ghost File Elimination:** Local files not present in the incoming cloud archive are verifiably destroyed during injection.
* **Safe Abort:** Forcing a failure in either the purge helper or the unpacker cleanly bypasses the transaction commit, leaving the original save data completely untouched.
* **Memory/Iterator Safety:** The system does not crash or skip files due to mutating a directory while an active read handle is open.