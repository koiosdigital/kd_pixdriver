# LED Effect Preview - WASM Build

WebAssembly module for previewing LED effects in a web browser.

## Prerequisites

- [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)

## Building

```bash
# Activate Emscripten environment
source /path/to/emsdk/emsdk_env.sh

# Create build directory
mkdir build && cd build

# Configure with Emscripten
emcmake cmake ..

# Build
emmake make

# Output files will be:
# - pixel_preview.js
# - pixel_preview.wasm
```

## Usage

### JavaScript API

```javascript
// Load the module
const Module = await createPixelPreview();

// Create a preview instance (60 LEDs, RGB, 60Hz)
const preview = new Module.PixelPreview(60, false, 60);

// Configure the effect
preview.setEffect("RAINBOW");
preview.setColor(255, 0, 0, 0);  // RGBW
preview.setSpeed(5);             // 1-10
preview.setBrightness(255);      // 0-255

// Optional: set seed for reproducible animations
preview.setRandomSeed(12345);

// Animation loop
function animate() {
    preview.tick();

    // Get frame data as Uint8Array view (RGBA format, 4 bytes per LED)
    const frameData = preview.getFrameData();
    const ledCount = preview.getLedCount();

    // Process frame data
    for (let i = 0; i < ledCount; i++) {
        const r = frameData[i * 4];
        const g = frameData[i * 4 + 1];
        const b = frameData[i * 4 + 2];
        const a = frameData[i * 4 + 3];  // Alpha or W channel
        // Render LED...
    }

    requestAnimationFrame(animate);
}

animate();
```

### Available Effects

- SOLID - Static color
- BLINK - On/off toggling
- BREATHE - Fade in/out
- CYCLIC - Rotating dot with trail
- RAINBOW - Color spectrum across strip
- COLOR_WIPE - Progressive fill
- THEATER_CHASE - Every-third-pixel pattern
- SPARKLE - Random pixel illumination
- COMET - Moving bright trail
- FIRE - Heat-based fire simulation
- WAVE - Sine wave brightness
- TWINKLE - Fading random sparkles
- GRADIENT - Morphing color gradient
- PULSE - Expanding pulse from center
- METEOR - Fast-moving meteor with trail
- RUNNING_LIGHTS - Animated sine wave

### Get Available Effects

```javascript
const effects = Module.PixelPreview.getEffectList();
for (let i = 0; i < effects.size(); i++) {
    console.log(effects.get(i));
}
```

## Testing

After building, serve the files with a local HTTP server:

```bash
cd build
python -m http.server 8080
```

Open `http://localhost:8080/index.html` in a browser.

## API Reference

### Constructor

```javascript
new PixelPreview(led_count, is_rgbw, update_rate_hz = 60)
```

- `led_count`: Number of LEDs (1-65535)
- `is_rgbw`: true for RGBW strips, false for RGB
- `update_rate_hz`: Animation update rate (default 60Hz)

### Methods

| Method | Description |
|--------|-------------|
| `setEffect(name)` | Set effect by name (case-insensitive) |
| `setColor(r, g, b, w)` | Set effect color (0-255 each) |
| `setBrightness(value)` | Set brightness (0-255) |
| `setSpeed(value)` | Set animation speed (1-10) |
| `tick()` | Advance animation by one frame |
| `reset()` | Reset to initial state |
| `setRandomSeed(seed)` | Set PRNG seed for reproducibility |
| `getFrameData()` | Get current frame as Uint8Array view |
| `getFrameSize()` | Get frame buffer size in bytes |
| `getLedCount()` | Get LED count |

### Static Methods

| Method | Description |
|--------|-------------|
| `PixelPreview.getEffectList()` | Get list of available effect names |
