Missing Edge Cases
The EmuNAND / SysNAND Collision: Using the MAC address as the sole hardware root of trust (device_id) fails if the user dual-boots SysNAND and EmuNAND, or swaps SD cards. Both environments share the MAC address but possess divergent local filesystem states. This will trigger constant, confusing conflict exceptions on the server. You need a unique OS-installation salt combined with the MAC.

Captive Portals & Network Lies: Nintendo Switches travel. The 5-minute background heartbeat will inevitably hit captive portals (hotels, airports). These networks return HTTP 200 OK with HTML payloads. If your client blindly parses this as a valid POST /sync_exchange response, the FSM will crash or incorrectly discard payloads.

Intentional Local Data Wipes: If a user deliberately deletes their local save data via the Horizon OS Data Management menu, what does the next pmdmnt exit or heartbeat do? Does it upload an empty "Proposal" that overwrites the server HEAD, or does it trigger an inbound restore of the deleted data?

Dead Battery Epoch Resets: If the Switch battery dies completely, the hardware clock resets to 1970. While you rely on snapshot_sequence for linear ordering, your Snapshot ID format (YYYYMMDD_HHMMSS-...) enforces lexical sorting. A 1970 timestamp will break chronological UX in the dashboard and complicate divergence detection.