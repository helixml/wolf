# Wolf Debugging Guide

## Building Wolf with Debug Symbols

### Quick Debug Build

For investigating deadlocks, crashes, or analyzing core dumps, build Wolf with full debugging symbols:

```bash
# In Wolf source directory
mkdir -p build-debug
cd build-debug

# Configure with debug symbols and no optimization
cmake .. \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-g3 -O0 -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-g3 -O0 -fno-omit-frame-pointer"

# Build
make -j$(nproc)

# Binary with symbols at: build-debug/wolf
```

**Debug flags explained**:
- `-g3`: Maximum debug information (includes macro definitions)
- `-O0`: No optimization (easier to debug, matches source code exactly)
- `-fno-omit-frame-pointer`: Preserve frame pointers for better backtraces

### Installing GStreamer Debug Symbols

The Wolf container uses Ubuntu. To get symbols for GStreamer libraries:

```bash
# Inside Wolf container
apt update
apt install -y \
  libgstreamer1.0-0-dbgsym \
  libgstbase-1.0-0-dbgsym \
  gstreamer1.0-plugins-base-dbgsym \
  gstreamer1.0-plugins-good-dbgsym

# For custom builds, install debug packages
apt install -y gstreamer1.0-tools gstreamer1.0-plugins-base-apps
```

**Note**: Ubuntu debug symbol packages may not be available for all versions. Alternative:

```bash
# Build GStreamer from source with symbols
git clone https://gitlab.freedesktop.org/gstreamer/gstreamer.git
cd gstreamer
meson setup build --buildtype=debug
meson compile -C build
```

### Docker Debug Build

To build Wolf Docker image with debug symbols:

```dockerfile
# In Dockerfile, change:
FROM ...
RUN cmake -DCMAKE_BUILD_TYPE=Release ...

# To:
FROM ...
RUN cmake -DCMAKE_BUILD_TYPE=Debug \
          -DCMAKE_CXX_FLAGS="-g3 -O0" \
          -DCMAKE_C_FLAGS="-g3 -O0" ...

# Keep debug symbols (don't strip)
# RUN strip /wolf/wolf  ← Comment this out

# Install debug symbol packages
RUN apt update && apt install -y \
    gdb \
    libgstreamer1.0-0-dbgsym \
    libgstbase-1.0-0-dbgsym || true
```

---

## Analyzing Core Dumps

### Generating Core Dumps

```bash
# From host (if gcore not available in container)
PID=$(docker inspect -f '{{.State.Pid}}' helixml-wolf-1)
gcore -o /tmp/wolf-core $PID

# Copy into container for analysis
docker cp /tmp/wolf-core.$PID helixml-wolf-1:/tmp/wolf-core
docker cp helixml-wolf-1:/wolf/wolf /tmp/wolf-binary

# Analyze with symbols
docker exec helixml-wolf-1 gdb /wolf/wolf /tmp/wolf-core
```

### Useful GDB Commands

```gdb
# Thread analysis
info threads                    # List all threads
thread <N>                      # Switch to thread N
thread apply all bt             # Backtrace of all threads
thread apply all bt full        # Full backtrace with locals

# Mutex analysis
info symbol 0x<address>         # Find what symbol owns address
p *(pthread_mutex_t*)0x<addr>   # Dump mutex structure
x/10xw 0x<addr>                 # Dump raw memory

# Finding stuck threads
thread apply all bt | grep -A 10 "futex\|mutex_lock"
```

### Core Dump from Production (Current Deadlock)

**Location**: `/home/luke/wolf-core-code.helix.ml-2025-11-14.core` (17GB)

**Findings**:
- Thread 99 (HTTPS): Stuck on mutex 0x70537c0062b0 in libgstbase
- Mutex owner: Thread 43209 (TID at offset +8 = 0xa8c9)
- Thread 43209: DEAD (not in live process, registers corrupted)
- Abandoned mutex held for 15+ hours

---

## Thread Monitoring API

### GET /api/v1/system/health

Returns heartbeat status of all monitored threads.

**Response**:
```json
{
  "success": true,
  "threads": [
    {
      "tid": 1345727,
      "name": "GStreamer-Pipeline",
      "details": "pulsesrc device=... ! queue ! interpipesink...",
      "seconds_since_heartbeat": 2,
      "seconds_alive": 3600,
      "heartbeat_count": 180000,
      "is_stuck": false
    }
  ],
  "stuck_thread_count": 0,
  "total_thread_count": 12,
  "overall_status": "healthy"
}
```

**Status values**:
- `healthy`: All threads responding
- `degraded`: Some threads stuck (< 50%)
- `critical`: Most threads stuck (≥ 50%)

**Stuck threshold**: Thread with >30 seconds since last heartbeat

### Heartbeat Mechanism

Threads automatically send heartbeats:
- **GStreamer pipelines**: Every 100ms during buffer processing
- **HTTPS/HTTP servers**: (Future) Every request processed
- **Cleanup**: Automatic unregister on thread exit

---

## Common Debugging Scenarios

### Scenario 1: HTTPS Deadlock

**Symptoms**:
- `/api/v1/system/health` shows HTTPS thread stuck >30s
- HTTPS requests time out
- HTTP still works

**Investigation**:
```bash
# Get thread state
docker exec helixml-wolf-1 cat /proc/1/task/<TID>/stack

# Find what mutex
# (Requires core dump or live gdb attach)
```

### Scenario 2: Abandoned Mutex

**Symptoms**:
- Thread exits unexpectedly (logs show THREAD_EXIT)
- Other threads start blocking on mutex
- `/api/v1/system/health` shows increasing stuck_thread_count

**Investigation**:
```bash
# Check logs for thread death
docker logs helixml-wolf-1 | grep "THREAD_EXIT\|exited UNEXPECTEDLY"

# If thread died recently, check last operations
docker logs helixml-wolf-1 | grep "TID=<tid>"
```

**Recovery**:
- Restart Wolf (only way to clear abandoned mutex)
- OR: Implement PTHREAD_MUTEX_ROBUST (future work)

### Scenario 3: GStreamer Internal Thread Death

**Symptoms**:
- NO "THREAD_EXIT" log (thread not ours, created by GStreamer internally)
- Mutex abandoned (detected via stuck threads)
- No crash/signal logged

**Investigation**:
- Cannot easily debug (thread created by GStreamer internals)
- Need GStreamer debug build with symbols
- May require patching GStreamer/interpipe for robust mutexes

---

## Future Improvements

### 1. Robust Mutexes (Prevents Abandoned Mutex Deadlocks)

Patch gst-interpipe to use PTHREAD_MUTEX_ROBUST:

```c
pthread_mutexattr_t attr;
pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
pthread_mutex_init(&listeners_mutex, &attr);

// All lock sites:
int ret = pthread_mutex_lock(&listeners_mutex);
if (ret == EOWNERDEAD) {
    GST_ERROR("Abandoned mutex detected - making consistent");
    pthread_mutex_consistent(&listeners_mutex);
}
```

### 2. Watchdog Thread

Monitor health endpoint and restart if critical:

```cpp
std::thread watchdog([]() {
  while (true) {
    sleep(30);
    // Check /api/v1/system/health
    // If overall_status == "critical" for >60s: exit(1) for restart
  }
});
```

### 3. Mutex Timeout Wrapper

For critical operations, add timeout detection:

```cpp
auto start = std::chrono::steady_clock::now();
gst_element_send_event(...);  // Or other blocking operation
auto elapsed = std::chrono::steady_clock::now() - start;

if (elapsed > std::chrono::seconds(5)) {
    logs::log(logs::error, "[DEADLOCK] Operation took {}s - potential deadlock",
              std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
}
```

---

## Production Debugging Checklist

When Wolf hangs in production:

1. **Check health endpoint**: `curl --unix-socket /var/run/wolf/wolf.sock http://localhost/api/v1/system/health`
2. **Check stuck threads**: Look for `is_stuck: true` in response
3. **Generate core dump**: `gcore -o /tmp/wolf-core <PID>`
4. **Backup core dump**: Copy out of container before restart
5. **Check logs**: `docker logs helixml-wolf-1 | grep -E "THREAD_EXIT|THREAD_LIFECYCLE|DEADLOCK"`
6. **Restart Wolf**: `docker restart helixml-wolf-1` (clears abandoned mutexes)
7. **Monitor recurrence**: Watch health endpoint after restart

**Do NOT**:
- Restart without gathering core dump and logs
- Clear logs before copying them out
- Skip health endpoint check (provides critical context)
