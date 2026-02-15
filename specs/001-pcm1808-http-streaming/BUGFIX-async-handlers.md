# Bug Fix: HTTP Server Blocking During Streaming

**Date**: 2026-02-15  
**Task**: T053  
**Severity**: Critical  
**Status**: Fixed

## Problem Description

When a client is actively streaming audio via `/stream.wav`, all other HTTP requests (like `/status`) fail or timeout.

## Root Cause Analysis

### Symptoms
- Status page (`/status`) becomes unresponsive when 1+ clients are streaming
- 4th concurrent request hangs or times out
- HTTP server appears frozen during active streams

### Technical Analysis

The ESP-IDF HTTP server uses a **worker thread pool** to handle incoming requests. The default configuration (`HTTPD_DEFAULT_CONFIG()`) creates a limited number of worker threads matching `max_open_sockets = 4`.

**Original Implementation Problem:**
```cpp
static esp_err_t stream_handler(httpd_req_t *req)
{
    // ... initialization ...
    
    while (clients[client_id].is_active)  // LINE 116: BLOCKING LOOP
    {
        // Read audio data
        // Send to client
        vTaskDelay(pdMS_TO_TICKS(2));
        // Loop continues for hours...
    }
    
    return ESP_OK;  // Never reached until stream ends
}
```

**Problem Flow:**
1. Client 1 connects → Worker Thread 1 enters `stream_handler()` → **BLOCKS in while loop**
2. Client 2 connects → Worker Thread 2 enters `stream_handler()` → **BLOCKS in while loop**
3. Client 3 connects → Worker Thread 3 enters `stream_handler()` → **BLOCKS in while loop**
4. Status request arrives → Worker Thread 4 (or queued/rejected) → **CANNOT RESPOND**

With 3 streaming clients, all worker threads are monopolized by blocking loops. When a 4th request comes in (e.g., `/status`), there's no free worker thread available.

### ESP-IDF HTTP Server Architecture

- Configuration: `max_open_sockets = 4` (line 377 in http_server.cpp)
- Each socket requires a worker thread from the thread pool
- Handler functions execute **synchronously** on worker threads
- Long-running handlers **monopolize** worker threads
- No free threads = no new requests can be processed

## Solution: Asynchronous Request Handling

ESP-IDF provides **async handler APIs** (since ESP-IDF v4.x) specifically designed for long-running requests like streaming:

### Key APIs
1. **`httpd_req_async_handler_begin(req, &async_req)`**
   - Creates a copy of the request that can be used on a separate thread
   - Returns immediately, freeing the worker thread
   - The async request copy remains valid until completed

2. **`httpd_req_async_handler_complete(async_req)`**
   - Marks the async request as finished
   - Frees associated memory and socket ownership
   - **CRITICAL**: Must be called or server will eventually refuse new connections

### Implementation Pattern

```cpp
struct StreamTaskContext {
    httpd_req_t *req;  // Async request copy
    int client_id;
};

static void stream_task(void *arg)
{
    StreamTaskContext *ctx = (StreamTaskContext *)arg;
    httpd_req_t *req = ctx->req;
    
    // Do the long-running streaming work here
    while (is_active) {
        // Read audio, send chunks, etc.
    }
    
    // CRITICAL: Mark request complete
    httpd_req_async_handler_complete(req);
    free(ctx);
    vTaskDelete(NULL);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    // 1. Prepare initial response (headers, WAV header)
    httpd_resp_set_type(req, "audio/wav");
    // ... send headers and WAV header ...
    
    // 2. Create async request copy
    httpd_req_t *async_req = nullptr;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        return ESP_FAIL;
    }
    
    // 3. Allocate context
    StreamTaskContext *ctx = malloc(sizeof(StreamTaskContext));
    ctx->req = async_req;
    ctx->client_id = client_id;
    
    // 4. Spawn separate FreeRTOS task
    xTaskCreatePinnedToCore(
        stream_task,
        "stream",
        16384,  // Stack size
        ctx,
        6,      // Priority
        nullptr,
        1       // Core 1 (network core)
    );
    
    // 5. Return IMMEDIATELY - worker thread is now FREE
    return ESP_OK;
}
```

## Changes Made

### File: `main/network/http_server.cpp`

**Added:**
- `struct StreamTaskContext` to pass data to async task (lines 24-28)
- `stream_task()` function containing the streaming loop (lines 51-141)

**Modified:**
- `stream_handler()` now creates async request, spawns task, and returns immediately (lines 143-257)
- Moved streaming loop logic from handler into separate task
- Added proper error handling for async handler creation failures
- Added cleanup in all error paths

### Key Differences

| Aspect | Before (Blocking) | After (Async) |
|--------|------------------|---------------|
| Worker thread usage | Held for stream duration (hours) | Released immediately (<1ms) |
| Concurrent streams | 3 streams = 3 blocked threads | 3 streams = 3 separate tasks |
| Status requests | Fail when 3+ clients streaming | Always responsive |
| Task creation | Handler runs on worker thread | Spawns dedicated task per stream |
| Memory | Stack on worker thread | 16KB stack per stream task |

## Testing & Validation

### Test Scenarios

1. **Single stream + status requests**
   - Start 1 stream
   - Continuously poll `/status` every 1 second
   - **Expected**: Status page always responds within 100ms

2. **Multiple concurrent streams**
   - Start 3 simultaneous streams
   - Poll `/status` from 4th client
   - **Expected**: Status page remains responsive

3. **Max clients + overflow**
   - Start 3 streams (max)
   - 4th stream request should get HTTP 503
   - 5th request to `/status` should succeed
   - **Expected**: Status works even when stream slots full

4. **Stream stability**
   - Run 3 concurrent streams for 1 hour
   - Monitor for task stack overflows, memory leaks
   - **Expected**: No crashes, audio quality unchanged

### Performance Impact

**Benefits:**
- ✅ Worker threads remain available for quick requests
- ✅ Supports 3 concurrent streams + unlimited status/config requests
- ✅ No change needed to `max_open_sockets` or buffer sizes
- ✅ Maintains real-time streaming performance

**Costs:**
- Additional 16KB stack per streaming client (3 × 16KB = 48KB total)
- Minimal CPU overhead for task creation (~1ms per connection)
- One additional malloc/free per stream connection

## References

- **ESP-IDF Documentation**: [HTTP Server - Asynchronous Handlers](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_http_server.html#asynchronous-handlers)
- **Example**: `protocols/http_server/async_handlers` in ESP-IDF examples
- **API**: `httpd_req_async_handler_begin()`, `httpd_req_async_handler_complete()`
- **Issue**: User report "When a client is streaming, all other http requests fail"

## Verification

To verify the fix:

```bash
# Terminal 1: Start 3 streams
curl http://[esp32-ip]/stream.wav > stream1.wav &
curl http://[esp32-ip]/stream.wav > stream2.wav &
curl http://[esp32-ip]/stream.wav > stream3.wav &

# Terminal 2: Poll status (should not block)
while true; do
  curl http://[esp32-ip]/status
  sleep 1
done
```

**Expected behavior**: Status requests return within 100ms even with 3 active streams.

## Related Tasks

- **T019**: Original HTTP server implementation
- **T020**: Original stream handler implementation
- **T039/T040**: Status page implementation (affected by this bug)
- **T053**: This bug fix (async handler refactor)

## Lessons Learned

1. **Long-running handlers in HTTP servers are an anti-pattern** - they monopolize limited worker threads
2. **Always use async patterns for streaming** - whether HTTP, WebSockets, or similar
3. **Thread pools have limits** - blocking in handlers effectively reduces pool size
4. **ESP-IDF provides the right tool** - async handlers are designed for exactly this use case
5. **Test concurrent access patterns early** - this bug only manifests under concurrent load

## Migration Notes

**For future features:**
- Any handler that runs for >100ms should use async pattern
- WebSocket handlers should also use async if implementing long-lived connections
- SSE (Server-Sent Events) would need similar async treatment

**No changes required for:**
- `/status` handler (quick response)
- `/config` handlers (quick response)
- Other short-lived request handlers
