// Software keyboard (naming Link on a new file, ...). On the Switch nn::swkbd::ShowKeyboard launches the keyboard
// library applet; our am service has no applets, so the game crashed. ShowKeyboard is replaced here: the overlay
// (runtime/ui/menu.mm menu_keyboard_run) asks for the text - typed directly with a keyboard, an on-screen keyboard
// with a controller - while this (game) thread keeps re-presenting the last frame.
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL_stdinc.h>

#include "../hle.h"

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap);  // runtime/ui/menu.mm

// nn::swkbd::ShowKeyboard(nn::swkbd::String *out, const nn::swkbd::ShowKeyboardArg &arg) -> nn::Result
// String = {void *ptr; size_t bufSize}
// ShowKeyboardArg (probed 2026-10-08, naming Link): +0x3ac text max length, +0x3b0 min length, +0x3c0 / +0x3c4
// initial text offset / length (UTF-16) in the work buffer at +0x4d0.
static void keyboard_convert(const char *to, const char *from, const char *input,
                             size_t input_size, char *output, size_t capacity, size_t terminator) {
    memset(output, 0, capacity);
    if (capacity < terminator) return;
    SDL_iconv_t converter = SDL_iconv_open(to, from);
    if (converter == (SDL_iconv_t)-1) return;
    size_t available = capacity - terminator;
    SDL_iconv(converter, &input, &input_size, &output, &available);
    SDL_iconv_close(converter);
}
HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint64_t out = c->x[0], arg = c->x[1];
    uint64_t ptr = LD64(out), size = LD64(out + 8);
    int max_len = (int)*(const int32_t *)(uintptr_t)(arg + 0x3ac);
    uint32_t off = *(const uint32_t *)(uintptr_t)(arg + 0x3c0), n = *(const uint32_t *)(uintptr_t)(arg + 0x3c4);
    uint64_t wb = LD64(arg + 0x4d0);
    char initial[96] = "";
    if (wb && n) {
        if (n > sizeof initial - 1) n = sizeof initial - 1;
        keyboard_convert("UTF-8", "UTF-16LE", (const char *)(uintptr_t)(wb + off),
                         2 * n, initial, sizeof initial, 1);
    }
    char text[96];
    int ok = menu_keyboard_run(initial, max_len, text, sizeof text);
    if (!ok) snprintf(text, sizeof text, "%s", initial);  // cancelled: keep what was there
    LOG("[swkbd] name entry (max %d, initial \"%s\"): %s \"%s\"\n", max_len, initial, ok ? "entered" : "cancelled", text);
    if (ptr && size)
        keyboard_convert("UTF-16LE", "UTF-8", text, strlen(text),
                         (char *)(uintptr_t)ptr, size, 2);
    c->x[0] = 0;
}
