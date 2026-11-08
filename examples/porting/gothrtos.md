---
title: "GothRTOS integration"
description: "Integrate upstream LVGL with GothRTOS GothGfx."
order: 10
---

This repository is the upstream LVGL source. The local GothRTOS tree currently
vendors LVGL 9.2.2 and connects it to GothGfx through a Lua module. The
[`lv_port_gothrtos.c`](./lv_port_gothrtos.c) example provides a replacement
bridge updated for the current upstream LVGL APIs. It is intentionally not part
of LVGL's normal library build: it is compiled by the GothRTOS userland
alongside GothGfx and Lua.

## What this port supports

GothRTOS is a small non-POSIX nanokernel with a Lua-oriented userland. Its
documented kernel responsibilities are task scheduling, clock ticks, and
message endpoints; it does not provide a POSIX thread or display API. The
existing GothGfx module supplies the display and input boundary used here:

- The display can use a Linux framebuffer or a GothGfx memory surface.
- GothGfx translates LVGL's RGB565 flushes into the destination format.
- GothGfx queues pointer/touch events; this bridge registers an LVGL pointer
  input device. The Lua caller must run `display:poll(0)` before
  `context:loop()` to pump the Linux input descriptors into that queue.
  Keyboard events are not mapped.
- LVGL's tick callback uses `CLOCK_MONOTONIC`, matching GothGfx's Linux
  user-mode clock source. `context:loop()` runs `lv_timer_handler()` and
  returns its suggested delay for the Lua caller to sleep.
- LVGL is configured with `LV_OS_NONE`. Keep all LVGL calls on the serialized
  Lua event loop; do not call LVGL from other tasks or interrupt handlers.
  The GothRTOS kernel's cooperative scheduler is not an LVGL mutex backend.
- The example allocates a 20-row RGB565 draw buffer through `lv_malloc()` and
  releases it on `close()` or Lua garbage collection. Set `LV_MEM_SIZE` in the
  target's LVGL configuration to cover that buffer and the UI.

This is the integration supported by the current local GothGfx implementation,
not a bare-metal GothRTOS display driver. It requires the Linux framebuffer
and evdev support used by GothGfx; GothRTOS ARM, Cortex-M, and AMD64
bare-metal images have no display/input adapter here and are unsupported.
Big-endian RGB565 surfaces are also unsupported by GothGfx's current pixel
conversion.

## Using the adapter

The bridge uses the GothGfx-internal functions
`gothgfx_lvgl_size()`, `gothgfx_lvgl_copy_area()`, and
`gothgfx_lvgl_poll_event()`. They are not declared by the public `gothgfx.h`
API, so this adapter intentionally matches the local GothGfx implementation
and must be kept in sync if those functions or the event structure change.

To use it in the GothRTOS tree:

1. Use this upstream LVGL source as the `LVGL_SOURCES` input in
   `GNUmakefile`; keep a GothRTOS-specific `lv_conf.h` with 16-bit color and
   `LV_USE_OS` set to `LV_OS_NONE`.
2. Compile `lv_port_gothrtos.c` with the GothRTOS GothGfx, Lua, and LVGL include
   paths, then link it into the existing GothGfx Lua module in place of its
   current `gothgfx_lvgl.c` bridge.
3. Keep the existing `liblvgl.a`, Lua, framebuffer/evdev, and GothGfx link
   dependencies. No changes to the GothRTOS build tree are made by this
   upstream LVGL repository.

The Lua API remains `require("gothlv").open(display)`, followed by
`context:loop()` from the application's event loop; call `context:close()` to
release the LVGL display/input objects and draw buffer. A GothGfx event loop
can use:

```lua
local gfx = require("gothgfx")
local gothlv = require("gothlv")
local display = assert(gfx.open())
local ui = gothlv.open(display)

while true do
    display:poll(0)
    local wait_ms = ui:loop()
    gfx.sleep(math.max(1, math.min(wait_ms, 10)))
end
```

The 10 ms cap keeps input polling responsive even when LVGL has no timer ready.
For headless integration tests, `gfx.open{memory = true, width = 320, height =
240}` creates a memory surface; it does not represent a physical display.
