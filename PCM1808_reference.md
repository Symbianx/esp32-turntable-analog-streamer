# PCM1808 Audio ADC Reference

## Device Overview
- **Part Number**: PCM1808 
- **Type**: Single-Ended Analog-Input, 24-Bit Stereo ADC
- **Manufacturer**: Texas Instruments
- **Max Sample Rate**: 96 kHz
- **Resolution**: 24-bit

## Key Specifications

### Audio Performance
- **Dynamic Range**: 98 dB (A-weighted, typ)
- **THD+N**: -85 dB (typ at 1 kHz)
- **Sample Rates**: 8 kHz to 96 kHz
- **Input Type**: Single-ended analog
- **Channels**: 2 (Stereo)

### Power Supply
- **Analog Supply (VDD)**: 5V (4.5V to 5.5V)
- **Digital Supply (VD)**: 3.3V (2.7V to 5.5V)
- **Power Consumption**: ~45 mW typical

## Pin Configuration (20-pin TSSOP)

### Power Pins
- **VDD** (Pin 1, 20): Analog power supply
- **VD** (Pin 10): Digital power supply  
- **DGND** (Pin 9): Digital ground
- **AGND** (Pin 2, 19): Analog ground

### Audio Input Pins
- **VINL** (Pin 18): Left channel analog input
- **VINR** (Pin 3): Right channel analog input

### Digital Output Pins
- **LRCK** (Pin 13): Left/Right clock output
- **BCK** (Pin 14): Bit clock output
- **DOUT** (Pin 12): Serial audio data output
- **SCKI** (Pin 15): System clock input

### Control Pins
- **FMT0, FMT1** (Pins 7, 8): Audio format selection
- **MD0, MD1** (Pins 5, 6): Mode selection
- **OSR** (Pin 16): Oversampling ratio select
- **RST** (Pin 11): Reset (active low)
- **ZERO** (Pin 4): Zero flag output

## Audio Format Modes (FMT1, FMT0)

| FMT1 | FMT0 | Format |
|------|------|--------|
| 0    | 0    | I2S (24-bit) |
| 0    | 1    | Left-Justified (24-bit) |
| 1    | 0    | Right-Justified (16-bit) |
| 1    | 1    | Right-Justified (24-bit) |

## System Mode Selection (MD1, MD0)

| MD1 | MD0 | Mode | Description |
|-----|-----|------|-------------|
| 0   | 0   | Slave mode | External SCKI, BCK, LRCK |
| 0   | 1   | Master mode | 256fs, ADC generates BCK/LRCK |
| 1   | 0   | Master mode | 384fs, ADC generates BCK/LRCK |
| 1   | 1   | Master mode | 512fs, ADC generates BCK/LRCK |

## Clock Requirements

### System Clock (SCKI)
- **Typical**: 256fs, 384fs, or 512fs (where fs = sample rate)
- **Example**: For 48 kHz sampling at 256fs: SCKI = 12.288 MHz

### Supported Sample Rates
- 8 kHz, 16 kHz, 32 kHz, 44.1 kHz, 48 kHz, 88.2 kHz, 96 kHz

## Typical Application Circuit

### Basic Single-Ended Input
Audio Source → AC Coupling Cap (1-10µF) → VINL/VINR

VDD → 10µF ceramic cap → GND
VD → 10µF ceramic cap → DGND

### Recommended External Components
- **Input coupling caps**: 1-10 µF (film or ceramic)
- **Power supply bypass**: 10 µF + 0.1 µF ceramic caps per supply
- **Clock source**: Crystal oscillator or external master clock

## Reset and Initialization
- **RST pin**: Active low, pull low to reset
- **Startup**: Hold RST low for minimum 4 SCKI cycles
- **Normal operation**: Pull RST high after power stabilizes

## Output Data Format (I2S Example)
- MSB first, 24-bit data
- LRCK low = Left channel, high = Right channel
- Data valid on BCK rising edge
- Data changes on BCK falling edge

## Operating Conditions
- **Temperature Range**: -40°C to +85°C (Industrial)
- **Analog Input Range**: 0V to VDD (single-ended)
- **Input Impedance**: ~10 kΩ (typical)

## Common Use Cases
1. Professional audio recording interfaces
2. Automotive audio systems  
3. Portable audio recorders
4. USB audio interfaces
5. Multi-channel audio capture systems

## Design Considerations
- Use separate analog and digital ground planes when possible
- Place bypass capacitors close to power pins
- Keep digital traces away from analog input traces
- Use low-jitter clock source for best audio performance
- Consider anti-aliasing filter for wide-bandwidth sources

## Register/Pin Settings for Quick Start

### Slave Mode, I2S Format, 48 kHz
- MD1, MD0 = 0, 0 (Slave)
- FMT1, FMT0 = 0, 0 (I2S)
- SCKI = 12.288 MHz (256fs)
- OSR = typical default

### Master Mode, 24-bit Left-Justified, 44.1 kHz  
- MD1, MD0 = 0, 1 (Master 256fs)
- FMT1, FMT0 = 0, 1 (LJ)
- SCKI = 11.2896 MHz
