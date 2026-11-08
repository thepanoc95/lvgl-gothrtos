/**
 * @file lv_port_gothrtos.c
 *
 * Lua bridge for the GothRTOS GothGfx display and pointer input module.
 * This example is built by the GothRTOS userland, not by LVGL itself.
 */

#define _GNU_SOURCE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "gothgfx.h"
#include "lauxlib.h"
#include "lvgl.h"

#if !defined(__linux__)
#error "The GothGfx adapter requires Linux framebuffer and evdev support."
#endif

#if LV_USE_OS != LV_OS_NONE
#error "The GothGfx Lua adapter requires LV_USE_OS == LV_OS_NONE."
#endif

#define GOTHGFX_EVENT_TOUCH 1
#define GOTHGFX_TOUCH_DOWN 0
#define GOTHGFX_TOUCH_MOVE 2
#define GOTHGFX_DRAW_BUFFER_ROWS 20

typedef struct GothGfxDisplay GothGfxDisplay;

/* Keep this layout in sync with GothGfx's private LVGL event bridge. */
typedef struct {
    int type;
    int action;
    int id;
    int x;
    int y;
    int raw_x;
    int raw_y;
    int code;
    uint64_t time_ms;
} GothGfxEvent;

typedef struct {
    GothGfxDisplay * surface;
    lv_display_t * display;
    lv_indev_t * pointer;
    uint8_t * draw_buffer;
    int32_t x;
    int32_t y;
    bool pressed;
    bool lvgl_initialized;
} GothLvContext;

static GothLvContext * active_context;
static uint32_t last_tick_ms;

extern void gothgfx_lvgl_copy_area(GothGfxDisplay * display, int x, int y,
                                   int width, int height, const uint8_t * pixels);
extern int gothgfx_lvgl_size(GothGfxDisplay * display, int * width, int * height);
extern int gothgfx_lvgl_poll_event(void * display, GothGfxEvent * event);

static uint32_t gothlv_tick_ms(void)
{
    struct timespec now;

    if(clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        LV_LOG_ERROR("GothLV: clock_gettime(CLOCK_MONOTONIC) failed");
        return last_tick_ms;
    }

    last_tick_ms = (uint32_t)((uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U);
    return last_tick_ms;
}

static void gothlv_flush(lv_display_t * display, const lv_area_t * area, uint8_t * pixels)
{
    int32_t width = area->x2 - area->x1 + 1;
    int32_t height = area->y2 - area->y1 + 1;
    GothLvContext * context = lv_display_get_user_data(display);

    gothgfx_lvgl_copy_area(context->surface, area->x1, area->y1, (int)width, (int)height, pixels);
    lv_display_flush_ready(display);
}

static void gothlv_read(lv_indev_t * indev, lv_indev_data_t * data)
{
    GothLvContext * context = lv_indev_get_user_data(indev);
    GothGfxEvent event;

    while(gothgfx_lvgl_poll_event(context->surface, &event)) {
        if(event.type != GOTHGFX_EVENT_TOUCH) continue;

        context->x = event.x;
        context->y = event.y;
        context->pressed = event.action == GOTHGFX_TOUCH_DOWN || event.action == GOTHGFX_TOUCH_MOVE;
        break;
    }

    data->point.x = context->x;
    data->point.y = context->y;
    data->state = context->pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void gothlv_close_context(GothLvContext * context)
{
    if(context == NULL || !context->lvgl_initialized) return;

    if(context->pointer != NULL) {
        lv_indev_delete(context->pointer);
        context->pointer = NULL;
    }

    if(context->display != NULL) {
        lv_display_delete(context->display);
        context->display = NULL;
    }

    lv_free(context->draw_buffer);
    context->draw_buffer = NULL;

    if(lv_is_initialized()) lv_deinit();
    context->lvgl_initialized = false;
    context->surface = NULL;

    if(active_context == context) active_context = NULL;
}

static int gothlv_loop(lua_State * state)
{
    GothLvContext * context = (GothLvContext *)luaL_checkudata(state, 1, "gothlv.context");

    if(context != active_context || !context->lvgl_initialized) {
        return luaL_error(state, "GothLV context is closed");
    }

    lua_pushinteger(state, (lua_Integer)lv_timer_handler());
    return 1;
}

static int gothlv_close(lua_State * state)
{
    GothLvContext * context = (GothLvContext *)luaL_checkudata(state, 1, "gothlv.context");

    gothlv_close_context(context);
    lua_pushboolean(state, 1);
    return 1;
}

static int gothlv_gc(lua_State * state)
{
    GothLvContext * context = (GothLvContext *)luaL_checkudata(state, 1, "gothlv.context");

    gothlv_close_context(context);
    return 0;
}

static const char * gothlv_init_context(GothLvContext * context, GothGfxDisplay * surface,
                                        int width, int height, size_t buffer_size)
{
    lv_obj_t * label;

    context->surface = surface;
    active_context = context;

    lv_init();
    context->lvgl_initialized = true;
    lv_tick_set_cb(gothlv_tick_ms);

    context->display = lv_display_create(width, height);
    if(context->display == NULL) return "cannot create LVGL display";

    context->draw_buffer = (uint8_t *)lv_malloc(buffer_size);
    if(context->draw_buffer == NULL) return "cannot allocate LVGL draw buffer";

    lv_display_set_user_data(context->display, context);
    lv_display_set_color_format(context->display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(context->display, gothlv_flush);
    lv_display_set_buffers(context->display, context->draw_buffer, NULL, (uint32_t)buffer_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    context->pointer = lv_indev_create();
    if(context->pointer == NULL) return "cannot create LVGL pointer input";

    lv_indev_set_type(context->pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(context->pointer, gothlv_read);
    lv_indev_set_user_data(context->pointer, context);
    lv_indev_set_display(context->pointer, context->display);

    label = lv_label_create(lv_screen_active());
    if(label == NULL) return "cannot create LVGL label";
    lv_label_set_text(label, "Hello from GothRTOS");

    return NULL;
}

static int gothlv_open(lua_State * state)
{
    GothGfxDisplay * surface = (GothGfxDisplay *)luaL_checkudata(state, 1, "gothgfx.display");
    GothLvContext * context;
    const char * error;
    int width;
    int height;
    size_t rows;
    size_t row_size;
    size_t buffer_size;

    if(active_context != NULL || lv_is_initialized()) {
        return luaL_error(state, "GothLV supports one LVGL instance per Lua state");
    }

    if(gothgfx_lvgl_size(surface, &width, &height) != 0) {
        return luaL_error(state, "GothGfx display is closed");
    }

    if(width <= 0 || height <= 0) {
        return luaL_error(state, "GothGfx display has invalid dimensions");
    }

    rows = (size_t)height < GOTHGFX_DRAW_BUFFER_ROWS ? (size_t)height : GOTHGFX_DRAW_BUFFER_ROWS;
    row_size = (size_t)width * sizeof(uint16_t);
    if(row_size > SIZE_MAX / rows || row_size > UINT32_MAX / rows) {
        return luaL_error(state, "GothGfx display dimensions are too large");
    }
    buffer_size = row_size * rows;

    context = (GothLvContext *)lua_newuserdatauv(state, sizeof(*context), 0);
    memset(context, 0, sizeof(*context));
    context->surface = surface;
    active_context = context;

    luaL_getmetatable(state, "gothlv.context");
    lua_setmetatable(state, -2);

    error = gothlv_init_context(context, surface, width, height, buffer_size);
    if(error != NULL) {
        gothlv_close_context(context);
        return luaL_error(state, "%s", error);
    }

    return 1;
}

int luaopen_gothlv(lua_State * state)
{
    static const luaL_Reg methods[] = {
        {"loop", gothlv_loop},
        {"close", gothlv_close},
        {"__gc", gothlv_gc},
        {NULL, NULL}
    };

    luaL_newmetatable(state, "gothlv.context");
    lua_pushvalue(state, -1);
    lua_setfield(state, -2, "__index");
    luaL_setfuncs(state, methods, 0);
    lua_pop(state, 1);

    lua_newtable(state);
    lua_pushcfunction(state, gothlv_open);
    lua_setfield(state, -2, "open");
    return 1;
}
