Build a native C++ OBS Studio Video Filter plugin using this obs-plugintemplate repository. The plugin captures video from any attached OBS source/scene, downscales and quantizes it on the GPU for a 120x56 tricolor (Red/Green/Amber) Alpha Director LED matrix, packs each frame into a 1,680-byte binary format, and streams it over UDP to an ESP32.

### 1. Plugin & Filter Registration
- Register a video filter (OBS_SOURCE_TYPE_FILTER, OBS_SOURCE_VIDEO) named "Alpha Director Output" (id = "alpha_director_filter").
- Update buildspec.json and CMakeLists.txt cleanly while preserving existing obs-plugintemplate CMake presets and GitHub Actions CI builds.
- Link ws2_32 on Windows for Winsock2 UDP support (use standard POSIX sockets on Linux/macOS so CI builds cleanly across all platforms).

### 2. OBS Filter Properties (obs_properties_t)
- host (string, default "espressif.local"): Destination hostname or IPv4 address.
- port (int, 1-65535, default 5005): Destination UDP port.
- max_fps (int, 0-120, default 0): Optional transmit throttle (0 = match OBS canvas FPS).
- preview_output (bool, default false): When false, pass the source video through to OBS untouched. When true, display the 4-color quantized output in OBS (using Black, Red, Green, and Amber #FFFF00 for visual preview).
- blank_on_disable (bool, default true): Send a single all-zero 1,680-byte UDP frame when the filter is disabled, hidden, deactivated, or destroyed.
- 4 optional color pickers for the reference quantization palette:
  - color_off (default #000000) -> Red=0, Green=0
  - color_red (default #FF0000) -> Red=1, Green=0
  - color_green (default #00FF00) -> Red=0, Green=1
  - color_amber (default #FFFFFF) -> Red=1, Green=1
  
### 3. GPU Pipeline (src/alpha-filter.cpp + data/alpha-quantize.effect)
Inside video_render:
1. Retrieve target = obs_filter_get_target(source) and its base cx, cy. If invalid or cx == 0 || cy == 0, call obs_source_skip_video_filter(source) and return.
2. Render target once into texrender_src (cx x cy, GS_RGBA) using gs_texrender_reset + gs_texrender_begin + gs_ortho(0, cx, 0, cy, -100, 100) + obs_source_video_render(target) + gs_texrender_end.
3. Downscale texrender_src into texrender_virtual (143 x 84, GS_RGBA) using obs_get_base_effect(OBS_EFFECT_BICUBIC) (fallback to OBS_EFFECT_DEFAULT if null).
4. Render texrender_virtual into texrender_leds (120 x 56, GS_RGBA) using data/alpha-quantize.effect:
   - Physical LED coordinate: col = clamp(floor(uv.x * 120.0), 0.0, 119.0), row = clamp(floor(uv.y * 56.0), 0.0, 55.0).
   - Virtual sample coordinate (skipping 1px gap every 5 cols and 4px gap every 7 rows):
     - vx = col + floor((col + 0.5) / 5.0) (maps 0..119 into 0..142)
     - vy = row + floor((row + 0.5) / 7.0) * 4.0 (maps 0..55 into 0..83)
     - sample_uv = (float2(vx, vy) + 0.5) / float2(143.0, 84.0) using point/clamp sampling.
   - Quantize sampled RGB (no dithering) to the nearest of the 4 reference palette colors using squared Euclidean RGB distance, breaking ties in order: color_off, color_red, color_green, color_amber.
   - Output float4(led_red, led_green, 0.0, 1.0) where led_red and led_green are 0.0 or 1.0.
5. Draw the visible filter output to the main OBS render target:
   - If preview_output is false: draw texrender_src unchanged using OBS_EFFECT_DEFAULT.
   - If preview_output is true: draw texrender_leds using OBS_EFFECT_DEFAULT (point-sampled if possible) so Red, Green, and Amber (R=1, G=1) show directly in OBS.
6. Non-blocking GPU-to-CPU readback (respecting max_fps if > 0):
   - Maintain two 120 x 56 GS_RGBA gs_stagesurface_t buffers (staging[0], staging[1]) plus write_idx and has_staged.
   - Stage gs_texrender_get_texture(texrender_leds) into staging[write_idx].
   - If has_staged is true, map staging[1 - write_idx] via gs_stagesurface_map, pass (data, linesize) to pack_alpha_frame, unmap immediately, and hand the 1,680-byte buffer to the async UDP sender.
   - Toggle write_idx ^= 1 and set has_staged = true.
7. In video_tick, track whether obs_source_enabled(source) transitioned from true to false (as well as in deactivate/hide/destroy), and if blank_on_disable is true, submit an all-zero 1,680-byte frame.

### 4. Standalone 1,680-Byte Bit-Packer (src/alpha-packer.hpp)
Must have NO OBS dependencies so it can be unit-tested standalone:
- Exact row mapping table ALPHA_ROW[56]:
    static constexpr uint8_t ALPHA_ROW[56] = {
        32, 36, 40, 44, 48, 52,  0,  4,  8, 12, 16, 20, 24, 28,
        33, 37, 41, 45, 49, 53,  1,  5,  9, 13, 17, 21, 25, 29,
        34, 38, 42, 46, 50, 54,  2,  6, 10, 14, 18, 22, 26, 30,
        35, 39, 43, 47, 51, 55,  3,  7, 11, 15, 19, 23, 27, 31,
    };
- Output buffer is std::array<uint8_t, 1680>:
  - Bytes 0..839: Green plane. Row r starts at ALPHA_ROW[r] * 15 (15 bytes = 120 bits).
  - Bytes 840..1679: Red plane. Row r starts at 840 + ALPHA_ROW[r] * 15 (15 bytes = 120 bits).
  - Within each 15-byte row, columns 0..119 are packed MSB-first: column col maps to byte (col / 8), bit mask (0x80 >> (col % 8)).
  - Input pixel (col, row) at rgba_data + row * linesize + col * 4: Red LED is on if px[0] >= 128, Green LED is on if px[1] >= 128.
  
### 5. Async UDP Sender (src/udp-sender.hpp, src/udp-sender.cpp)
- Runs a single background std::thread with a mutex + std::condition_variable and a single overwriteable 1,680-byte pending-frame slot (never grows an unbounded queue, never blocks OBS's render thread).
- Resolves host via getaddrinfo (AF_INET, SOCK_DGRAM) on the worker thread whenever host/port changes, caching the resolved sockaddr_in. If resolution fails (e.g., ESP32 offline), back off and retry every 2 seconds without spamming logs or blocking OBS.
- Properly initializes/cleans up Winsock (WSAStartup/WSACleanup) on Windows.
- Cleanly joins the thread and closes the socket in the destructor (flushing one final blank frame if requested before exit).

### 6. Unit Tests (tests/test-packer.cpp)
- Add a standalone C++ test executable (built and run via CTest or directly with g++/clang++ in your environment) that verifies pack_alpha_frame:
  1. All-black frame produces 1,680 zero bytes.
  2. Single Red pixel at (col=0, row=0) sets ONLY byte 840 + 32 * 15 = 1320 to 0x80.
  3. Single Green pixel at (col=7, row=6) sets ONLY byte 0 * 15 = 0 to 0x01.
  4. Single Amber pixel (R=255, G=255) at (col=119, row=55) sets ONLY byte 31 * 15 + 14 = 479 (Green) and byte 840 + 479 = 1319 (Red) to 0x01.
  5. Non-tight linesize padding (e.g. linesize = 512 instead of 480) is handled accurately.
- Run the unit tests to confirm they pass before finishing.

Thank you!!
