#pragma once

// Connects to a running Carabiner instance on localhost and maintains the
// current Ableton Link session BPM in a lock-free atomic that the audio
// thread can read without blocking.
//
// Carabiner must be running on the same host:
//   ./Carabiner-linux-arm64        (default port 17000)
//
// Usage:
//   link_sync_start();             // spawns background thread
//   float bpm = link_sync_get_bpm(); // 0.0f = not connected / no peers
//   link_sync_stop();              // join thread on shutdown

void link_sync_start(int carabiner_port = 17000);
void link_sync_stop();

// Returns the current Link session BPM, or 0.0f when Carabiner is not
// reachable or there are no peers.
float link_sync_get_bpm();

// Returns true when a TCP connection to Carabiner is active.
bool link_sync_connected();
