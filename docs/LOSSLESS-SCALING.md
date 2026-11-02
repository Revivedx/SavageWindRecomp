# Lossless Scaling (LSFG) & External Capture Guide for Savage Wind

This guide explains how to configure **Lossless Scaling** (Spatial Upscaling and LSFG Frame Generation) or other external capture tools (OBS Studio, Discord Screen Share) with **Savage Wind** for optimal performance, smooth frame pacing, and minimum latency across all hardware configurations.

---

## 1. Quick Start: Recommended Settings

### In Savage Wind (`savagewind.ini` or in-game Video Options)

| Setting | Recommended Value | Why |
| :--- | :--- | :--- |
| **VSync** | **Adaptive** (`vsync = adaptive`) when supported; otherwise try **Fast** (`vsync = fast`) | Check the swapchain log for the actual present mode. Adaptive falls back to FIFO when `FIFO_RELAXED` is unavailable on some drivers/surfaces (e.g. certain Intel displays); Fast can use Mailbox or Immediate instead. |
| **Window Mode** | **Borderless Fullscreen** (for Native Res FG) or **Windowed** (for External Upscaling) | Required by screen capture APIs (WGC/DXGI). For spatial upscaling (LS1/FSR), run in **Windowed** mode at the lower resolution so the capture API captures the smaller buffer directly. Exclusive Fullscreen bypasses DWM and cannot be captured reliably. |
| **Frame Rate** | **Uncapped** or **Target Refresh Rate** | Eliminates engine-side artificial frame caps while allowing smooth pacing. |
| **Upscaler** | **Off** (or Native) | If upscaling through Lossless Scaling (LS1/FSR), leave internal upscalers off to save GPU cycles. |

---

### In Lossless Scaling

#### **Frame Generation (Geração de Quadros - LSFG 3.1)**
- **Mode (Modo):**
  - **Adaptive (Adaptativo):** Best if your base FPS fluctuates or doesn't evenly divide into your monitor refresh rate. Set **Target (Alvo)** to your monitor's refresh rate (e.g., `60`, `75`, `120`, `144`).
  - **Fixed Multiplier (X2 / X3):** Best if your base framerate is locked and stable (e.g., stable 30 FPS $\to$ 60 FPS, or stable 60 FPS $\to$ 120 FPS).
- **Performance Mode (Modo de Desempenho):**
  - **ON (Ligado):** Strongly recommended on entry-level GPUs, integrated GPUs, and dual-GPU laptops. Reduces AI compute passes and memory bandwidth consumption by ~50%.
  - **OFF (Desligado):** High-end discrete desktop GPUs (RTX 3070+, RX 6700XT+) with ample GPU headroom.
- **Flow Scale (Escala de fluxo):** 50% (Default).

#### **Upscaling (Redimensionamento)**
- **Native Resolution Gaming:** Set to **Off (Desativado)** if running the game at your monitor's native resolution and only using Frame Generation. Borderless Fullscreen can be used here.
- **Lower Base Resolution (Performance Boost):** Run Savage Wind in **Windowed** mode at the selected lower resolution, then set Lossless Scaling to **LS1** or **AMD FSR**, Mode: **Fullscreen (Tela cheia)** with Sharpness at 2–3. Borderless Fullscreen scales the image to desktop size before capture; use it for native-resolution frame generation rather than external spatial upscaling.

#### **Rendering Options (Opções de Renderização)**
- **Sync Mode (Modo de sincronização):** `Default (Padrão)`.
- **Max Frame Latency (Latência máxima dos quadros):** `1` (Keeps the presentation queue lean and minimizes input lag).
- **G-Sync Support (Suporte ao G-Sync):**
  - **OFF (Desligado):** Recommended for standard fixed-refresh monitors (60 Hz, 75 Hz) or laptops where the external monitor runs through an iGPU without active G-Sync. Prevents frame pacing jitter.
  - **ON (Ligado):** Only if you have a true G-Sync/FreeSync VRR display actively enabled in Windows Display Settings.
- **Draw FPS (Exibir FPS):** `ON` (Displays `Base FPS / Generated FPS` overlay).

#### **Capture (Captura)**
- **Capture API:** **WGC (Windows Graphics Capture)**. On Windows 10 (version 1903+) and Windows 11, WGC offers the lowest capture latency and highest throughput. DXGI may be used as a fallback if WGC exhibits issues on older builds.
- **Queue Target (Alvo da fila):** `1` (Balanced smoothness and latency).

#### **GPU Selection (GPU / Exibição)**
- **Single GPU Systems (Desktops):** Leave as `Auto` (Default dGPU).
- **Dual-GPU Laptops (NVIDIA Optimus / AMD Switchable):**
  - Select your **Integrated GPU (Intel Iris Xe / AMD Radeon 600M/700M)** as the **Preferred GPU** in Lossless Scaling.
  - This offloads the AI frame interpolation shaders entirely to the iGPU, leaving your dedicated GPU (NVIDIA/AMD dGPU) 100% free to render Savage Wind.

---

## 2. Technical Architecture: How Savage Wind Handles Presentation

### The VSync Trap: FIFO vs. Mailbox vs. Adaptive
Savage Wind exposes three primary Vulkan presentation modes:
1. **FIFO (`vsync = on`):**  
   Strict VSync. When an external capture utility (Lossless Scaling, OBS, Discord) hooks the window, the Windows Desktop Window Manager (DWM) locks swapchain surfaces during duplication. Under FIFO, Vulkan's `vkAcquireNextImageKHR` blocks until the next VBLANK, causing 30–45 ms pipeline stalls and severely penalizing framerates.
2. **Mailbox (`vsync = fast`):**  
   Triple-buffering with frame replacement. While ideal on some desktop configurations, on hybrid laptops (Optimus) or displays with fractional refresh rates (e.g. 75 Hz), missing a single VBLANK window can trigger driver-level throttling down to sub-multiples ($75 / 3 = 25\text{ FPS}$).
3. **Adaptive / FIFO Relaxed (`vsync = adaptive`):**  
   **The optimal mode.** If a frame renders in time, it waits for VBLANK (tear-free). If a frame is late (e.g. takes 20 ms / 50 FPS on a 75 Hz monitor), **it is presented immediately without stalling for the next refresh cycle.** This avoids the 25 FPS trap and ensures unthrottled GPU delivery.

### Swapchain Buffer Headroom (5 Images)
Savage Wind creates swapchains with `caps.minImageCount + 3` images (clamped to 8):
- 1 image being scanned out to the display.
- 1 image held by Windows Graphics Capture / DWM during frame copy.
- 1 image being processed by the GPU in flight.
- 1 image in the submission queue.
- 1 free image ready for immediate acquisition.

The extra images reduced capture-related acquisition stalls on the tested configuration, but do not guarantee an absolute acquisition-time bound. The image count is also limited by the surface's `maxImageCount`, so five images may not be available on all drivers/surfaces.

---

## 3. Resolution Matrix & Upscaling Guide

Rendering at a lower base resolution in **Windowed** mode and letting Lossless Scaling (LS1/FSR) upscale to your target display resolution provides significant framerate gains while reducing DWM capture bandwidth (in Borderless Fullscreen, Savage Wind stretches the image to full desktop resolution internally before capture):

### 16:9 Displays (Standard 1080p / 1440p / 4K)
| Target Display | Suggested Game Resolution | Render Load Reduction | Typical Base FPS Gain |
| :--- | :--- | :--- | :--- |
| **1920 x 1080** | **1280 x 720** (720p) | ~55% fewer pixels | +40% to +60% FPS |
| **2560 x 1440** | **1920 x 1080** (1080p) | ~44% fewer pixels | +35% to +50% FPS |
| **3840 x 2160** | **2560 x 1440** (1440p) | ~55% fewer pixels | +45% to +65% FPS |

### 21:9 Ultra-Wide Displays
| Target Display | Suggested Game Resolution | Render Load Reduction | Typical Base FPS Gain |
| :--- | :--- | :--- | :--- |
| **2560 x 1080** | **2134 x 900** | ~30% fewer pixels | +20% to +30% FPS |
| **2560 x 1080** | **1920 x 810** | ~44% fewer pixels | +35% to +50% FPS |
| **2560 x 1080** | **1708 x 720** | ~55% fewer pixels | +50% to +70% FPS |
| **3440 x 1440** | **2560 x 1080** | ~44% fewer pixels | +35% to +50% FPS |

---

## 4. Case Study: Low-VRAM Dual-GPU Laptop Benchmark

- **Hardware:** Intel Core i5/i7 (Tiger Lake) + Intel Iris Xe Graphics + NVIDIA GeForce MX350 (2 GB GDDR5, Pascal)
- **Display:** 2560 x 1080 Ultra-Wide @ 75 Hz
- **Configuration:** Savage Wind on MX350 (`vsync = adaptive`), Lossless Scaling on Iris Xe (`WGC`, `Performance Mode = ON`, `Target = 75`)

### Results:
| Resolution | Game VSync Mode | Base FPS | Lossless Scaling FPS | Presentation Stalls (`acq`) | Result |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **2560x1080** | `FIFO` (`vsync = on`) | 31–35 FPS | 60–70 FPS | 36–42 ms stalls | High input latency, frame pacing drops |
| **2560x1080** | `Mailbox` (`vsync = fast`) | 24–25 FPS | 50 FPS | 0 ms | Trapped at $75/3$ refresh rate divisor |
| **2560x1080** | `Adaptive` (`vsync = adaptive`) | 45–48 FPS | **75 FPS (Solid)** | **0.02 ms** | Smooth, tear-free, full target reached |
| **1920x810** | `Adaptive` + LS1 Upscaling | 60–65 FPS | **75–120 FPS** | **0.02 ms** | Maximum fluidity, lowest thermals |

---

## 5. Troubleshooting Checklist

1. **Framerate locks at 25 or 30 FPS:**
   - In Savage Wind, set **Video > VSync** to **Adaptive**. Do not use *Fast* on 75 Hz monitors.
2. **Sudden drop in base FPS when activating Lossless Scaling:**
   - Verify that **API de Captura** is set to **WGC** and **Modo de Desempenho** is **Ligado**.
   - Ensure Savage Wind is updated to version 0.4.1+ (includes 5-buffer swapchain).
3. **Micro-stutters or jitter during camera panning:**
   - Set **Suporte ao G-Sync** to **Desligado** in Lossless Scaling unless FreeSync/G-Sync is verified active in Windows display settings.
   - Set **Latência máxima dos quadros** to `1`.
4. **Window not scaling:**
   - Ensure Savage Wind is running in **Borderless Fullscreen** or **Windowed** mode (not Exclusive Fullscreen).
