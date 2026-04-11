# Tasks: ESP-DSP Parametric Equalization

**Input**: Design documents from `specs/003-esp-dsp-equalization/`  
**Branch**: `003-esp-dsp-equalization`  
**Prerequisites**: plan.md ✅, research.md ✅, data-model.md ✅, contracts/eq-api.yaml ✅, quickstart.md ✅

> No spec.md — user stories are derived from plan.md functional requirements and contract endpoints.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks)
- **[Story]**: Which user story this task belongs to (US1–US4)

---

## User Stories

| ID | Story | Priority | Goal |
|---|---|---|---|
| US1 | Core EQ Audio Processing | P1 🎯 MVP | EQ filter chain processes audio in the DMA loop |
| US2 | EQ Configuration Persistence | P2 | EQ settings survive reboots via NVS |
| US3 | EQ HTTP REST API | P3 | Read/write EQ config programmatically |
| US4 | EQ Web UI | P4 | Configure EQ via browser config portal |

---

## Phase 1: Setup

**Purpose**: Wire the esp-dsp component into the build system and register the new source file. Nothing compiles yet — just scaffolding.

- [X] T001 Add `espressif/esp-dsp: ">=1.0.0"` to the `dependencies` block in `main/idf_component.yml`
- [X] T002 Add `"audio/eq_processor.cpp"` to `SRCS` and `esp-dsp` to `PRIV_REQUIRES` in `main/CMakeLists.txt`

**Checkpoint**: `idf.py build` still builds cleanly (eq_processor.cpp doesn't exist yet — CMake will fail until T007; that is expected and will be resolved then)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Schema types and NVS key constants that every subsequent phase depends on. No runtime behavior yet — just data definitions.

**⚠️ CRITICAL**: All user story work depends on this phase being complete first.

- [X] T003 Add `EQFilterType` enum (`PEAKING=0`, `LOW_SHELF=1`, `HIGH_SHELF=2`, `LOW_PASS=3`, `HIGH_PASS=4`) to `main/config_schema.h`
- [X] T004 Add `EQBandConfig` struct (`bool enabled`, `EQFilterType filter_type`, `float frequency_hz`, `float gain_db`, `float q_factor`) to `main/config_schema.h`
- [X] T005 Add `EQConfig` block to `DeviceConfig` in `main/config_schema.h`: field `bool eq_enabled` and field `EQBandConfig eq_bands[10]`. Bump the `version` constant by 1 to force NVS re-init on existing devices.
- [X] T006 [P] Add EQ NVS key constants to the `NVSKeys` namespace in `main/config_schema.h`: `EQ_ENABLED = "eq_enabled"`, and per-band templates `EQ_BAND_EN = "eq_bN_en"`, `EQ_BAND_TYPE = "eq_bN_type"`, `EQ_BAND_FREQ = "eq_bN_freq"`, `EQ_BAND_GAIN = "eq_bN_gain"`, `EQ_BAND_Q = "eq_bN_q"` (where N is replaced at runtime with the digit 0–9)

**Checkpoint**: Project compiles with the new schema types (after T007 stubs the missing .cpp in Phase 3). `DeviceConfig` is complete.

---

## Phase 3: User Story 1 — Core EQ Audio Processing (Priority: P1) 🎯 MVP

**Goal**: The audio capture task processes each DMA block through the biquad EQ chain before writing to the ring buffer. With EQ disabled (default), audio is bit-identical to the current behavior. With EQ enabled and all bands at 0 dB, audio is also bit-identical within float rounding.

**Independent Test**: Flash the device. Confirm audio streams correctly with `curl http://<ip>:8080/stream`. Observe log: `I eq_processor: EQ bypassed (disabled)`. Enable EQ master switch via serial console or HTTP (US3). Observe `I eq_processor: EQ active, 0 bands enabled` — audio still streams. Enable one band with gain; confirm audible effect on playback.

### Implementation for User Story 1

- [X] T007 Create `main/audio/eq_processor.h`: declare `EQProcessor` class with `static bool init(const DeviceConfig& config, uint32_t sample_rate)`, `static bool process(const uint8_t* input_i2s, uint8_t* output_24, size_t frames)`, `static void update_band(uint8_t band_index, const EQBandConfig& band, uint32_t sample_rate)`, `static void set_enabled(bool enabled)`, `static bool is_enabled()`. Define `EQ_MAX_BANDS = 10` and `EQ_FRAMES_PER_BLOCK = 240` constants.

- [X] T008 [US1] Implement static `DRAM_ATTR` storage in `main/audio/eq_processor.cpp`: `float s_coef[EQ_MAX_BANDS][5]`, `float s_w[EQ_MAX_BANDS][4]` (stereo delay lines), `float s_float_buf[EQ_FRAMES_PER_BLOCK * 2]` (480 interleaved floats), `bool s_enabled`, `uint8_t s_active_bands`, `uint32_t s_sample_rate`. All arrays are `static DRAM_ATTR` to guarantee placement in internal SRAM.

- [X] T009 [US1] Implement `recompute_band_coef()` private helper in `main/audio/eq_processor.cpp`. For `PEAKING`: use Audio EQ Cookbook formula (`A = pow(10, gain_db/40)`, `w0 = 2π × freq/Fs`, `alpha = sin(w0)/(2Q)`) — do NOT use `dsps_biquad_gen_peakingEQ_f32` (it has no gain parameter). For `LOW_SHELF`: call `dsps_biquad_gen_lowShelf_f32(coef, freq/Fs, gain_db, q)`. For `HIGH_SHELF`: call `dsps_biquad_gen_highShelf_f32(coef, freq/Fs, gain_db, q)`. For `LOW_PASS`: call `dsps_biquad_gen_lpf_f32(coef, freq/Fs, q)`. For `HIGH_PASS`: call `dsps_biquad_gen_hpf_f32(coef, freq/Fs, q)`. Clamp frequency to `[20.0f, (sample_rate / 2.0f) - 1.0f]`, gain to `[-24.0f, 24.0f]`, Q to `[0.1f, 10.0f]` before computing. Log each band at `ESP_LOGD`.

- [X] T010 [US1] Implement `EQProcessor::init()` in `main/audio/eq_processor.cpp`: store sample_rate, copy `config.eq_enabled` to `s_enabled`, zero all delay lines with `memset(s_w, 0, sizeof(s_w))`, iterate all 10 bands — for enabled bands call `recompute_band_coef()`, count enabled bands into `s_active_bands`. Log at `ESP_LOGI`: band count, sample rate, enabled state.

- [X] T011 [US1] Implement `EQProcessor::process()` in `main/audio/eq_processor.cpp`. If `!s_enabled || s_active_bands == 0`, return `false` (caller uses legacy packing). Otherwise: (1) convert 32-bit I²S slots to normalized float32 LRLR — for each 32-bit word, cast to int32_t, shift right 8 to get 24-bit signed value, divide by `8388608.0f` (2²³) to normalize to [-1, +1]; (2) call `dsps_biquad_sf32(s_float_buf, s_float_buf, frames, s_coef[b], s_w[b])` for each enabled band b; (3) convert float32 LRLR back to 24-bit packed — clamp to [-1, +1], multiply by `8388607.0f`, cast to int32_t, write 3 bytes little-endian per sample. Return `true`.

- [X] T012 [US1] Implement `EQProcessor::update_band()` in `main/audio/eq_processor.cpp`: copy the band config, call `recompute_band_coef()` for that index only (do NOT zero its delay line `s_w[b]` — preserves continuity). Recount `s_active_bands` across all bands. Log the update at `ESP_LOGI`.

- [X] T013 [US1] Implement `EQProcessor::set_enabled()` in `main/audio/eq_processor.cpp`: store new state to `s_enabled`. On transition `false→true`: zero all delay lines then recount active bands and recompute all active-band coefficients (ensures no stale state). Log state change at `ESP_LOGI`.

- [X] T014 [US1] Integrate `EQProcessor` into `main/audio/audio_capture.cpp`: include `"eq_processor.h"`. In `audio_capture_task`, after `I2SMaster::read()` succeeds, call `EQProcessor::process(dma_buffer, converted_buffer, num_frames)`. If it returns `true`, use `converted_buffer` directly for `AudioBuffer::write()`. If it returns `false` (EQ bypassed), run the existing 32-bit slot → 24-bit packing path into `converted_buffer` as before.

- [X] T015 [US1] Call `EQProcessor::init(config, config.sample_rate)` in `main/main.cpp` after `NVSConfig::load(&config)` and before `AudioCapture::start()`, so coefficients are ready when the first DMA block arrives.

**Checkpoint**: Build and flash. Audio streams correctly. With EQ disabled (default), behavior is identical to before this feature. `idf.py build` produces zero warnings (`-Wall -Werror`).

---

## Phase 4: User Story 2 — EQ Configuration Persistence (Priority: P2)

**Goal**: EQ settings are saved to NVS on POST and restored on boot. Factory defaults produce a flat (0 dB, all bands disabled) EQ.

**Independent Test**: Set a band gain via POST /eq (US3 not yet implemented — test manually by calling `EQProcessor::update_band()` from a temporary test stub or verify NVS round-trip in isolation). Reboot device. Confirm `EQProcessor::init()` log shows the saved band parameters loaded from NVS.

### Implementation for User Story 2

- [X] T016 [US2] Add `load_eq_factory_defaults(DeviceConfig* config)` helper in `main/storage/nvs_config.cpp`: sets `config->eq_enabled = false`, populates `eq_bands[10]` with the canonical defaults from data-model.md (band 0: LOW_SHELF 32Hz 0dB Q=0.707; bands 1–8: PEAKING at 64/125/250/500/1k/2k/4k/8kHz 0dB Q=1.4; band 9: HIGH_SHELF 16kHz 0dB Q=0.707; all `enabled = false`).

- [X] T017 [US2] Extend `NVSConfig::load()` in `main/storage/nvs_config.cpp` to read EQ fields: read `eq_enabled` (uint8). For each band 0–9: build key strings like `"eq_b0_en"`, `"eq_b0_type"`, `"eq_b0_freq"`, `"eq_b0_gain"`, `"eq_b0_q"` (max 11 chars, within 15-char NVS limit). Read floats as uint32 via `memcpy` bitcast. If any key is missing, call `load_eq_factory_defaults()` for the EQ fields and continue (do not treat as full config corruption).

- [X] T018 [US2] Extend `NVSConfig::save()` in `main/storage/nvs_config.cpp` to write all EQ fields using the same key naming scheme as T017. Write floats as uint32 via `memcpy` bitcast. Log success/failure of EQ save at `ESP_LOGI`.

- [X] T019 [US2] Extend `NVSConfig::load_factory_defaults()` in `main/storage/nvs_config.cpp` to call `load_eq_factory_defaults()` so a full factory reset also resets EQ.

**Checkpoint**: EQ settings survive a device reboot. Factory reset produces flat EQ. `idf.py build` still clean.

---

## Phase 5: User Story 3 — EQ HTTP REST API (Priority: P3)

**Goal**: `GET /eq` returns full EQ state as JSON. `POST /eq` applies partial updates, persists to NVS, and calls `EQProcessor::update_band()` for each changed band. `POST /eq/reset` restores factory defaults.

**Independent Test**: 
```bash
curl http://<device_ip>:8080/eq
curl -X POST http://<device_ip>:8080/eq -H "Content-Type: application/json" \
  -d '{"eq_enabled":true,"bands":[{"index":2,"enabled":true,"filter_type":"PEAKING","frequency_hz":1000,"gain_db":6.0,"q_factor":1.4}]}'
curl http://<device_ip>:8080/eq   # verify change reflected
curl -X POST http://<device_ip>:8080/eq/reset
curl http://<device_ip>:8080/eq   # verify flat defaults restored
```

### Implementation for User Story 3

- [X] T020 [US3] Implement `GET /eq` handler in `main/network/http_server.cpp`: read current config via `NVSConfig::load()` (or a cached config struct), serialize to JSON matching the `EQStatusResponse` schema from `contracts/eq-api.yaml` (fields: `eq_enabled`, `sample_rate`, `bands[10]` array with `index`, `enabled`, `filter_type`, `frequency_hz`, `gain_db`, `q_factor`). Serialize `EQFilterType` enum to string (`"PEAKING"`, `"LOW_SHELF"`, etc.). Register route `GET /eq` in the HTTP server init function. Add CORS headers via existing `add_cors_headers()`.

- [X] T021 [US3] Implement `POST /eq` handler in `main/network/http_server.cpp`: parse JSON body (use existing `httpd_req_get_sockfd` + `httpd_req_recv` pattern from other POST handlers). Support partial update: `eq_enabled` field (optional) → call `EQProcessor::set_enabled()`. `bands` array (optional) → for each entry: validate `index` in [0,9], validate and clamp `frequency_hz`/`gain_db`/`q_factor` to allowed ranges, update the config struct, call `EQProcessor::update_band()`. Persist updated config with `NVSConfig::save()`. Return updated `EQStatusResponse` JSON. Return HTTP 400 with `{"error": "..."}` for JSON parse failures or out-of-range index.

- [X] T022 [US3] Implement `POST /eq/reset` handler in `main/network/http_server.cpp`: load config, call `NVSConfig::load_factory_defaults()` for EQ fields only (preserve WiFi/MQTT settings), call `EQProcessor::init()` with the default config, persist with `NVSConfig::save()`. Return `EQStatusResponse` showing flat defaults. Register route `POST /eq/reset`.

- [X] T023 [US3] Register all three EQ routes (`GET /eq`, `POST /eq`, `POST /eq/reset`) in the `HTTPServer::init()` function in `main/network/http_server.cpp`, alongside the existing `/status`, `/stream`, `/config` routes.

**Checkpoint**: All three endpoints return correct responses per `contracts/eq-api.yaml`. Live audio reflects parameter changes within one DMA block period (~5 ms).

---

## Phase 6: User Story 4 — EQ Web UI (Priority: P4)

**Goal**: The config portal at `http://<device_ip>:8080/` includes an EQ section with a 10-band editor. Changes are sent to `POST /eq` and take effect immediately.

**Independent Test**: Open the config portal in a browser. Navigate to the EQ section. Toggle the master EQ enable switch. Adjust band 5 gain slider to +6 dB. Click Apply. Verify the page reflects the saved values. Click Reset. Verify all bands return to 0 dB.

### Implementation for User Story 4

- [X] T024 [US4] Add an EQ section to the config portal HTML in `main/network/config_portal_html.h`: include a master "Enable EQ" toggle switch. Add a 10-row band editor table with columns: Band#, Enable checkbox, Filter Type dropdown (`PEAKING`, `LOW_SHELF`, `HIGH_SHELF`, `LOW_PASS`, `HIGH_PASS`), Frequency input (number, 20–20000 Hz), Gain slider (range -24 to +24 dB, step 0.5, with live numeric readout, hidden/disabled for LPF/HPF), Q factor input (number, 0.1–10.0, step 0.1). Add "Apply EQ" and "Reset to Flat" buttons.

- [X] T025 [US4] Add JavaScript to `main/network/config_portal_html.h` to populate the EQ table on page load: fetch `GET /eq`, iterate `bands` array, set each row's field values. Show/hide the gain slider based on `filter_type` (disable for LOW_PASS/HIGH_PASS). Bind "Apply EQ" button to collect all band values and POST to `/eq`. Bind "Reset to Flat" button to POST to `/eq/reset` then re-fetch and repopulate the table. Show a brief success/error toast notification after each action.

**Checkpoint**: Full end-to-end flow works in browser: load page → see current EQ state → adjust bands → Apply → hear audio change → Reset → bands return to flat defaults.

---

## Phase 7: Polish & Cross-Cutting Concerns

- [X] T026 [P] Add `EQProcessor` initialization guard: if `dsps_init()` is required by the esp-dsp library version in use, call it once at system startup in `main/main.cpp` before `EQProcessor::init()`. Confirm with `ESP_ERROR_CHECK`.
- [X] T027 [P] Verify `main/CMakeLists.txt` compiles cleanly with `-Wall -Werror` (no new warnings from eq_processor.cpp). Fix any signed/unsigned comparison, unused parameter, or narrowing conversion warnings.
- [X] T028 [P] Add EQ state to the `GET /status` JSON response in `main/network/http_server.cpp`: include `"eq_enabled": true/false` and `"eq_active_bands": N` fields in the existing system status payload so monitoring tools can see EQ state.
- [X] T029 Run the quickstart.md validation: flash device, exercise all three curl commands from the quickstart, confirm expected log output appears on serial monitor. Document any deviations in a brief note in `specs/003-esp-dsp-equalization/quickstart.md`.

---

## Dependencies & Execution Order

### Phase Dependencies

```
Phase 1 (Setup)
    └─► Phase 2 (Foundational) — schema types needed by all phases
            ├─► Phase 3 (US1 — Core EQ)        ← MVP — implement first
            │       └─► Phase 5 (US3 — HTTP API) ← needs EQProcessor::update_band()
            │               └─► Phase 6 (US4 — Web UI) ← needs HTTP endpoints
            └─► Phase 4 (US2 — NVS Persistence) ← parallel with US1, needed by US3
```

### User Story Dependencies

- **US1** (Core EQ): Depends only on Foundational. No dependency on US2/US3/US4. ← Start here.
- **US2** (NVS): Depends only on Foundational. Can be developed in parallel with US1.
- **US3** (HTTP API): Depends on US1 (`EQProcessor::update_band`) AND US2 (NVS save/load). Both must complete first.
- **US4** (Web UI): Depends on US3 (HTTP endpoints must exist). Sequential after US3.

### Within Each User Story

- T007 (header) → T008 (storage) → T009 (coef gen) → T010 (init) → T011 (process) → T012 (update) → T013 (set_enabled) → T014 (audio integration) → T015 (main.cpp wiring)

---

## Parallel Opportunities

### Phase 2 (Foundational)
```
T003 (EQFilterType enum)      ← no dependency
T004 (EQBandConfig struct)    ← no dependency  } run together in config_schema.h
T005 (DeviceConfig extension) ← needs T003, T004
T006 (NVS key constants)      ← no dependency
```

### Phase 3 + Phase 4 (after Phase 2 complete)
```
US1: T007 → T008 → T009 → T010 → T011 → T012 → T013 → T014 → T015
US2: T016 → T017 → T018 → T019
(these two streams touch different files and can proceed in parallel)
```

### Phase 7 (all parallel)
```
T026, T027, T028 — all touch different files, no inter-dependency
```

---

## Implementation Strategy

### MVP (US1 only — Core EQ Audio Processing)

1. Complete Phase 1: Setup (T001–T002)
2. Complete Phase 2: Foundational (T003–T006)
3. Complete Phase 3: US1 (T007–T015)
4. **STOP and VALIDATE**: Flash device, confirm audio streams with EQ bypassed (bit-identical), then enable EQ and verify it doesn't break streaming
5. This is a shippable increment — EQ runs but is config-only via serial/recompile

### Full Feature

1. Add US2 (NVS) → EQ settings survive reboots
2. Add US3 (HTTP API) → fully controllable at runtime
3. Add US4 (Web UI) → accessible to non-technical users
4. Polish Phase 7

---

## Notes

- `dsps_biquad_gen_peakingEQ_f32` has **no gain parameter** — T009 must use the custom AudioEQ Cookbook formula for PEAKING type (see `research.md` §2)
- All float buffers and delay lines must be `DRAM_ATTR` — PSRAM would cause cache miss latency in the audio loop, breaking real-time guarantees (Constitution §III)
- Coefficient updates from the HTTP handler (Core 1) are lock-free — Xtensa 32-bit float writes are atomic. Do NOT add a mutex to the audio path (Constitution §IV explicitly forbids it)
- `DeviceConfig` schema version bump in T005 is required to prevent stale NVS configs from misinterpreting the new `eq_bands[10]` field
- NVS key names must stay ≤15 characters (ESP-IDF NVS hard limit). Format `eq_bN_en` = 8 chars ✅
