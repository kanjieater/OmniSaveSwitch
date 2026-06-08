#!/bin/bash

echo "Injecting Server Database & State Machine..."

# 1. Update Docker Compose to persist the SQLite database
cat << 'DOCKER' > server/docker-compose.yml
version: '3.8'

services:
  omnisave-core:
    build: .
    container_name: omnisave_server
    restart: unless-stopped
    volumes:
      - ./src:/app/src
      - ./data:/app/data
DOCKER

# 2. Add Python requirements
cat << 'REQ' > server/requirements.txt
requests==2.31.0
REQ

# 3. Update Dockerfile to install dependencies
cat << 'DFILE' > server/Dockerfile
FROM python:3.11-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY src/ /app/src/
CMD ["python", "-u", "src/main.py"]
DFILE

# 4. Create the SQLite Database configuration
cat << 'DB' > server/src/database.py
import sqlite3
import os
import uuid

DB_PATH = "/app/data/omnisave.db"

def init_db():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('''
        CREATE TABLE IF NOT EXISTS lineage (
            transaction_id TEXT PRIMARY KEY,
            title_id TEXT NOT NULL,
            device_id TEXT NOT NULL,
            logical_version INTEGER NOT NULL,
            checksum_sha256 TEXT NOT NULL,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            state TEXT NOT NULL
        )
    ''')
    conn.commit()
    return conn

def log_transaction(title_id, device_id, version, checksum, state="RECEIVED"):
    conn = init_db()
    txn_id = str(uuid.uuid4())
    conn.cursor().execute(
        "INSERT INTO lineage (transaction_id, title_id, device_id, logical_version, checksum_sha256, state) VALUES (?, ?, ?, ?, ?, ?)",
        (txn_id, title_id, device_id, version, checksum, state)
    )
    conn.commit()
    conn.close()
    return txn_id
DB

# 5. Create the Explicit ACK State Machine
cat << 'STATE' > server/src/state_machine.py
from database import log_transaction

def process_inbound_save(title_id, device_id, manifest, raw_payload_path):
    print(f"[{title_id}] Processing inbound save from {device_id}...")
    
    # 1. Validate CRC32 manifest (Transport check)
    if not validate_manifest(manifest, raw_payload_path):
        print(f"[{title_id}] Manifest validation failed. Aborting ingestion.")
        return False
        
    # 2. Generate SHA256 aggregate hash
    aggregate_hash = compute_sha256(raw_payload_path)
    
    # 3. Determine next logical version
    next_version = get_latest_version(title_id) + 1
    
    # 4. Persist Metadata (SQLite)
    txn_id = log_transaction(title_id, device_id, next_version, aggregate_hash, "PERSISTED")
    
    # 5. Archive ZIP payload locally (Append-only history)
    archive_path = f"/app/data/archives/{title_id}/{txn_id}.zip"
    zip_payload(raw_payload_path, archive_path)
    
    # 6. Push to RomM (REST API)
    if not push_to_romm(title_id, archive_path):
        update_state(txn_id, "FAILED_ROMM")
        return False
        
    update_state(txn_id, "COMMITTED")
    
    # 7. EXPLICIT ACK: Now it is safe to delete the outbound/ folder on the Switch
    delete_switch_outbound(title_id)
    return True

# Stub functions for architecture demonstration
def validate_manifest(m, p): return True
def compute_sha256(p): return "hash_stub"
def get_latest_version(t): return 0
def zip_payload(src, dest): pass
def push_to_romm(t, p): return True
def update_state(txn, state): pass
def delete_switch_outbound(t): print("ACK: Deleted remote outbound folder")
STATE

echo "Injecting Switch Boot Recovery Rules..."

# 6. Inject the C++ Boot Recovery & Directory logic
cat << 'CPP' > sysmodule/source/main.cpp
#include <switch.h>
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>

u32 __nx_applet_type = AppletType_None;

#define INNER_HEAP_SIZE 0x200000
size_t nx_inner_heap_size = INNER_HEAP_SIZE;
char   nx_inner_heap[INNER_HEAP_SIZE];

void __libnx_initheap(void) {
    void* addr = nx_inner_heap;
    size_t size = nx_inner_heap_size;
    envSetHeapOverride(addr, size);
}

// Helper to remove directories recursively
void remove_dir(const char* path) {
    // Implementation of rm -rf equivalent for sdmc
}

void execute_boot_recovery() {
    DIR* dir;
    
    // Rule 1: Interrupted Local Extraction
    if ((dir = opendir("sdmc:/omnisave/tmp_out"))) {
        closedir(dir);
        remove_dir("sdmc:/omnisave/tmp_out");
    }
    
    // Rule 2: Interrupted Server Push
    if ((dir = opendir("sdmc:/omnisave/tmp_in"))) {
        closedir(dir);
        remove_dir("sdmc:/omnisave/tmp_in");
    }
    
    // Rule 3: Server missed the push signal
    if ((dir = opendir("sdmc:/omnisave/outbound"))) {
        closedir(dir);
        // TODO: Fire UDP Ping OUTBOUND_READY
    }
    
    // Rule 4: Pending Server injection
    if ((dir = opendir("sdmc:/omnisave/inbound"))) {
        closedir(dir);
        // TODO: Verify target game is closed, check manifest version, and inject
    }
}

int main(int argc, char **argv) {
    Result rc = fsInitialize();
    
    if (R_SUCCEEDED(rc)) {
        fsdevMountSdmc();
        
        // Ensure base directory structure exists
        mkdir("sdmc:/omnisave", 0777);
        mkdir("sdmc:/omnisave/tmp_out", 0777);
        mkdir("sdmc:/omnisave/outbound", 0777);
        mkdir("sdmc:/omnisave/tmp_in", 0777);
        mkdir("sdmc:/omnisave/inbound", 0777);
        
        execute_boot_recovery();
        
        fsdevUnmountDevice("sdmc");
        fsExit();
    }

    while(true) {
        // Main daemon loop (psm hooks and UDP listeners will go here)
        svcSleepThread(10000000000ull); 
    }

    return 0;
}
CPP

chmod +x implement_core.sh
./implement_core.sh
rm implement_core.sh
