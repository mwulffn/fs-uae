// Lua access to the emulated display (the video table).
//
// The functions read the buffer the chipset emulation draws into. The lines
// above the current beam position are from the frame being drawn, the rest
// from the previous frame. In warp mode only some of the frames are drawn.

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef WITH_LUA

#include "luaengine.h"

#include "uae/fs.h"

#include <png.h>
#include <vector>

struct video_frame {
    const uae_u8 *buffer;
    int stride;
    int width;
    int height;
};

static video_frame check_frame(lua_State *L)
{
    video_frame frame;
    if (!uae_fs_video_frame(&frame.buffer, &frame.stride, &frame.width, &frame.height)) {
        luaL_error(L, "no frame has been drawn yet");
    }
    return frame;
}

// video.size() returns the width and height of the frame in pixels.
static int l_video_size(lua_State *L)
{
    video_frame frame = check_frame(L);
    lua_pushinteger(L, frame.width);
    lua_pushinteger(L, frame.height);
    return 2;
}

// video.pixel(x, y) returns the red, green and blue values (0 to 255) of a
// pixel. The top left pixel is 0, 0.
static int l_video_pixel(lua_State *L)
{
    video_frame frame = check_frame(L);
    lua_Integer x = luaL_checkinteger(L, 1);
    lua_Integer y = luaL_checkinteger(L, 2);
    luaL_argcheck(L, x >= 0 && x < frame.width, 1, "outside the frame");
    luaL_argcheck(L, y >= 0 && y < frame.height, 2, "outside the frame");
    const uae_u8 *p = frame.buffer + y * frame.stride + x * 4;
    lua_pushinteger(L, p[2]);
    lua_pushinteger(L, p[1]);
    lua_pushinteger(L, p[0]);
    return 3;
}

// Copies one line to out as red, green and blue bytes.
static void copy_line_rgb(const video_frame &frame, int y, uae_u8 *out)
{
    const uae_u8 *p = frame.buffer + (size_t) y * frame.stride;
    for (int x = 0; x < frame.width; x++) {
        out[0] = p[2];
        out[1] = p[1];
        out[2] = p[0];
        out += 3;
        p += 4;
    }
}

// video.pixels() returns the frame as a string with three bytes (red,
// green, blue) per pixel, line by line from the top, and the width and
// height.
static int l_video_pixels(lua_State *L)
{
    video_frame frame = check_frame(L);
    size_t line_size = (size_t) frame.width * 3;
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, line_size * frame.height);
    for (int y = 0; y < frame.height; y++) {
        copy_line_rgb(frame, y, (uae_u8 *) out + y * line_size);
    }
    luaL_pushresultsize(&b, line_size * frame.height);
    lua_pushinteger(L, frame.width);
    lua_pushinteger(L, frame.height);
    return 3;
}

static bool write_png(const video_frame &frame, FILE *f)
{
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL) {
        return false;
    }
    png_infop info = png_create_info_struct(png);
    // Declared before setjmp, so it is destroyed properly on errors.
    std::vector<uae_u8> line((size_t) frame.width * 3);
    if (info == NULL || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return false;
    }
    png_init_io(png, f);
    png_set_IHDR(
        png, info, frame.width, frame.height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < frame.height; y++) {
        copy_line_rgb(frame, y, line.data());
        png_write_row(png, line.data());
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return true;
}

// video.screenshot(path) saves the frame as a PNG file, and returns the
// width and height.
static int l_video_screenshot(lua_State *L)
{
    video_frame frame = check_frame(L);
    const char *path = luaL_checkstring(L, 1);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return luaL_error(L, "could not open '%s' for writing", path);
    }
    bool ok = write_png(frame, f);
    fclose(f);
    if (!ok) {
        return luaL_error(L, "could not write '%s'", path);
    }
    lua_pushinteger(L, frame.width);
    lua_pushinteger(L, frame.height);
    return 2;
}

static const luaL_Reg video_functions[] = {
    {"pixel", l_video_pixel},
    {"pixels", l_video_pixels},
    {"screenshot", l_video_screenshot},
    {"size", l_video_size},
    {NULL, NULL},
};

void luaengine_open_video(lua_State *L)
{
    luaL_newlib(L, video_functions);
    lua_setglobal(L, "video");
}

#endif  // WITH_LUA
