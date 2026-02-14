<!--
Sync Impact Report:
- Version Change: 0.0.0 → 1.0.0
- Type: MAJOR (Initial constitution ratification)
- Modified Principles: N/A (new constitution)
- Added Sections:
  * I. Real-Time Performance Guarantee
  * II. Audio Quality First
  * III. Resource Efficiency
  * IV. Deterministic Timing
  * V. Fail-Safe Operation
  * Hardware Constraints
  * Development Standards
- Removed Sections: N/A
- Templates Requiring Updates:
  ✅ plan-template.md - Constitution Check section aligned
  ✅ spec-template.md - Performance/audio requirements added to context
  ✅ tasks-template.md - Audio/performance testing gates added to checklist
- Follow-up TODOs: None
-->

# Turntable ESP32 UPnP Streamer Constitution

## Core Principles

### I. Real-Time Performance Guarantee (NON-NEGOTIABLE)

Audio capture and streaming MUST operate with deterministic, bounded latency. All
operations in the audio path MUST complete within their allocated time budget:

- ADC sampling MUST occur at the configured sample rate (±0.1% tolerance)
- Audio buffer processing MUST complete before the next buffer fills
- Network transmission MUST NOT block audio capture under any circumstances
- ISR handlers MUST complete in <10μs
- Critical tasks (audio processing) MUST run on dedicated CPU cores with pinned affinity

**Rationale**: Audio is a real-time workload. Buffer underruns, overruns, or timing
jitter cause audible artifacts that destroy user experience. The ESP32's dual-core
architecture and FreeRTOS must be leveraged to guarantee timing constraints.

### II. Audio Quality First

Audio fidelity is the primary product quality metric. All design decisions MUST
preserve signal integrity from ADC input to network output:

- Sample depth MUST be ≥16-bit; prefer 24-bit or 32-bit floating-point where supported
- Sample rate MUST be ≥44.1kHz for music applications; 48kHz preferred
- Signal chain MUST introduce <0.1% Total Harmonic Distortion (THD)
- Dynamic range MUST be ≥90dB (SNR ≥90dB)
- No lossy compression unless explicitly user-configurable with clear quality impact disclosure
- Anti-aliasing filters MUST be applied before sample rate conversion
- Dithering MUST be used when reducing bit depth

**Rationale**: The purpose of this device is to digitize analog audio with transparency.
Compromising audio quality defeats the product's value proposition. Users expect
near-CD-quality output from high-fidelity turntables.

### III. Resource Efficiency

The ESP32 has constrained resources (520KB SRAM, dual 240MHz cores). All code MUST
operate within these hard limits:

- Heap allocations in audio path are FORBIDDEN; use static buffers or FreeRTOS pools
- Stack usage MUST be profiled and documented per task (default 4KB, audio tasks 8KB max)
- DMA MUST be used for all ADC and I²S transfers to minimize CPU overhead
- Zero-copy buffers MUST be used where possible (DMA → network, no memcpy)
- Polling loops are FORBIDDEN; use interrupts or FreeRTOS notifications
- Flash usage MUST stay <80% to allow OTA updates
- CPU utilization targets: audio core ≤60%, network core ≤70% under load

**Rationale**: Resource exhaustion causes crashes, reboots, or audio glitches. Embedded
systems require disciplined resource management. Static allocation and DMA maximize
throughput while maintaining predictability.

### IV. Deterministic Timing

Code behavior MUST be time-predictable. Unbounded operations and variable-time
algorithms are prohibited in the audio path:

- Fixed-size ring buffers MUST be used for all audio data
- Lock-free algorithms MUST be used for producer-consumer patterns between cores
- Critical sections MUST be <1ms; document any >100μs sections
- Mutex usage in audio tasks is FORBIDDEN; use atomic operations or lock-free structures
- Network operations MUST be asynchronous and non-blocking
- Watchdog timers MUST monitor audio task liveness (<5s timeout)
- Task priorities MUST be documented with rationale (audio ISR highest, networking lowest)

**Rationale**: Priority inversions, unbounded waits, and blocking I/O are incompatible
with real-time constraints. The scheduler must guarantee audio tasks meet deadlines.

### V. Fail-Safe Operation

System errors MUST NOT corrupt audio or crash the device. Defensive programming and
graceful degradation are mandatory:

- Input validation MUST occur on all external data (network configs, user settings)
- Buffer overflows/underflows MUST be detected and logged, then recovered via silence insertion
- Network disconnection MUST NOT stop audio capture; buffer and recover gracefully
- Hardware errors (ADC, I²S) MUST trigger soft resets, NOT system reboots
- Persistent configuration MUST have checksums; corrupt data triggers factory defaults
- Over-temperature/voltage conditions MUST throttle performance before shutdown
- Logging MUST NOT allocate heap or block; use fixed-size ring buffers and async flush

**Rationale**: Embedded devices often run in unattended environments. Silent failures
or crashes are unacceptable. Users expect the device to "just work" and recover from
transient issues automatically.

## Hardware Constraints

The ESP32 platform imposes non-negotiable physical limits that govern all design:

- **Processor**: Dual-core Xtensa LX6 @ 240MHz (160MHz low-power mode supported)
- **Memory**: 520KB SRAM (DRAM + IRAM), 4MB Flash typical
- **ADC**: 12-bit SAR ADC (effective ~10-bit due to noise), 11dB attenuation steps
- **Audio Path**: External I²S codec REQUIRED for >16-bit audio; internal ADC insufficient
- **Network**: WiFi 802.11 b/g/n, 150Mbps PHY rate (~80Mbps TCP throughput realistic)
- **Power**: 80-240mA typical, 500mA peak; power budget MUST accommodate continuous WiFi TX
- **Thermal**: 85°C junction max; enclose designs MUST maintain <70°C ambient

**Design Implications**:
- Use external high-resolution ADC/codec (e.g., PCM186x, AK5394A) via I²S for ≥24-bit audio
- Partition tasks: Core 0 → audio ISR + processing, Core 1 → networking + control
- Double-buffer DMA: one buffer filling while other is consumed
- Network buffering MUST tolerate WiFi jitter (≥1 second of audio buffered)

## Development Standards

### Testing Requirements

- **Unit Tests**: All audio processing functions MUST have unit tests verifying correctness
  and timing (use ESP-IDF Unity test framework)
- **Integration Tests**: Full audio path MUST be tested with known sine wave inputs to
  measure THD, SNR, and latency
- **Performance Tests**: Task high-water marks, heap usage, and CPU utilization MUST be
  logged during stress tests (4+ hours streaming)
- **Hardware-in-Loop**: Final validation MUST occur on physical ESP32 hardware, NOT just
  QEMU emulator
- **Test Signals**: Use 1kHz sine, swept sine (20Hz-20kHz), pink noise, and silence for
  audio path validation

### Code Quality

- **Static Analysis**: ESP-IDF component builds MUST pass with no warnings (`-Wall -Werror`)
- **Documentation**: All FreeRTOS tasks MUST document priority, stack size, and core affinity
- **Profiling**: Use ESP-IDF trace tools to measure ISR timing, task CPU %, and heap stats
- **Logging**: Use `ESP_LOG` macros with appropriate levels (ERROR for audio path failures,
  INFO for state changes)
- **Version Control**: Tag releases with audio quality metrics (THD, SNR, latency) in release notes

### Configuration Management

- **Kconfig**: Use ESP-IDF `sdkconfig` for compile-time settings (buffer sizes, sample rates)
- **Runtime Config**: Store user settings in NVS (Non-Volatile Storage) with defaults
- **OTA Updates**: Maintain two firmware partitions; rollback on boot failure
- **Build Reproducibility**: Pin ESP-IDF version in `idf_component.yml` or use Docker build environment

## Governance

This constitution supersedes all coding practices and design decisions. Amendments require:

1. Documented justification (why change is necessary)
2. Impact analysis on existing audio quality/performance metrics
3. Approval from project maintainer(s)
4. Migration plan if breaking changes affect deployed devices
5. Version increment per semantic versioning rules

**Compliance Enforcement**:

- All pull requests MUST pass audio quality benchmarks (automated THD/SNR tests)
- Code reviews MUST verify adherence to resource limits (heap, stack, CPU)
- Performance regressions >5% (latency, CPU usage) REQUIRE explicit justification
- Constitution violations MUST be documented in commit messages and resolved before merge

**Version**: 1.0.0 | **Ratified**: 2026-02-13 | **Last Amended**: 2026-02-13
