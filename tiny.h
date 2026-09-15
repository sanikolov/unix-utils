#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

static HANDLE heap, output;
static int failed;
static char buffer[4096];
static DWORD used;

static void flush(void) {
    DWORD offset = 0, written;
    while (offset < used) {
        if (!WriteFile(output, buffer + offset, used - offset, &written, 0) || !written) {
            failed = 1;
            break;
        }
        offset += written;
    }
    used = 0;
}

static void put(char c) {
    if (used == sizeof(buffer)) flush();
    buffer[used++] = c;
}

static void text(const char *s) { while (*s) put(*s++); }

static void wide(const WCHAR *s) {
    char bytes[4];
    DWORD mode, written;
    int n, count;
    if (GetConsoleMode(output, &mode)) {
        DWORD length = (DWORD)lstrlenW(s);
        flush();
        while (length) {
            if (!WriteConsoleW(output, s, length, &written, 0) || !written) { failed = 1; break; }
            s += written; length -= written;
        }
        return;
    }
    while (*s) {
        count = s[0] >= 0xd800 && s[0] <= 0xdbff && s[1] >= 0xdc00 && s[1] <= 0xdfff ? 2 : 1;
        n = WideCharToMultiByte(CP_UTF8, 0, s, count, bytes, 4, 0, 0);
        for (int i = 0; i < n; ++i) put(bytes[i]);
        s += count;
    }
}

static void number(ULONGLONG value, int width) {
    char digits[20];
    int n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (width-- > n) put(' ');
    while (n) put(digits[--n]);
}

static void error(const WCHAR *path, DWORD code) {
    HANDLE saved = output;
    flush();
    output = GetStdHandle(STD_ERROR_HANDLE);
    text(PROGRAM_NAME ": "); wide(path); text(": Windows error "); number(code, 0); put('\n');
    flush(); output = saved; failed = 1;
}

static void *allocate(SIZE_T bytes) {
    void *p = HeapAlloc(heap, 0, bytes);
    if (!p) { error(L"out of memory", ERROR_NOT_ENOUGH_MEMORY); ExitProcess(FAILURE_CODE); }
    return p;
}

/* Windows quote/backslash rules, in two passes to allocate only argc pointers.
   Keeping this here avoids loading shell32 just to split the command line. */
static int arguments(const WCHAR *s, WCHAR *storage, WCHAR **argv) {
    int argc = 0;
    SIZE_T n = 0;
    while (*s) {
        int quoted = 0;
        while (*s == L' ' || *s == L'\t') ++s;
        if (!*s) break;
        if (argv) argv[argc] = storage + n;
        ++argc;
        while (*s && (quoted || (*s != L' ' && *s != L'\t'))) {
            SIZE_T slashes = 0;
            while (*s == L'\\') { ++slashes; ++s; }
            if (*s == L'"') {
                for (SIZE_T i = 0; i < slashes / 2; ++i) { if (storage) storage[n] = L'\\'; ++n; }
                if (slashes & 1) { if (storage) storage[n] = L'"'; ++n; }
                else if (quoted && s[1] == L'"') { if (storage) storage[n] = L'"'; ++n; ++s; }
                else quoted = !quoted;
                ++s;
            } else {
                for (SIZE_T i = 0; i < slashes; ++i) { if (storage) storage[n] = L'\\'; ++n; }
                if (!*s || (!quoted && (*s == L' ' || *s == L'\t'))) break;
                if (storage) storage[n] = *s;
                ++n; ++s;
            }
        }
        if (storage) storage[n] = 0;
        ++n;
    }
    return argc;
}
