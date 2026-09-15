#define PROGRAM_NAME "ls"
#define FAILURE_CODE 2
#include "tiny.h"
#include "glob.h"
#include <winioctl.h>

/* No CRT: all storage, text conversion and I/O use dynamically linked Win32. */
typedef struct Entry {
    struct Entry *next;
    DWORD attributes;
    DWORD tag;
    FILETIME modified;
    ULONGLONG size;
    WCHAR name[1];
} Entry;

static int all, almost, detailed, reverse, bytime, bysize, directory, unsorted;
static int recursive;

static int compare(const Entry *a, const Entry *b) {
    int c = 0;
    if (bytime) c = -CompareFileTime(&a->modified, &b->modified);
    else if (bysize) c = a->size > b->size ? -1 : a->size < b->size ? 1 : 0;
    if (!c) c = CompareStringOrdinal(a->name, -1, b->name, -1, FALSE) - CSTR_EQUAL;
    return reverse ? -c : c;
}

/* In-place linked-list merge sort: no second entry array or quadratic sorting. */
static Entry *sort(Entry *head) {
    Entry *slow, *fast, *right, *tail, *result = 0;
    if (!head || !head->next) return head;
    slow = head; fast = head->next;
    while (fast && fast->next) { slow = slow->next; fast = fast->next->next; }
    right = slow->next; slow->next = 0;
    head = sort(head); right = sort(right); tail = 0;
    while (head || right) {
        Entry *e;
        if (!right || (head && compare(head, right) <= 0)) { e = head; head = head->next; }
        else { e = right; right = right->next; }
        if (tail) tail->next = e; else result = e;
        tail = e;
    }
    tail->next = 0;
    return result;
}

/* Read the link itself, so dangling links and relative targets remain visible. */
static WCHAR *linktarget(const WCHAR *path) {
    HANDLE file = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             0, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, 0);
    BYTE *data;
    WCHAR *result = 0;
    DWORD returned;
    if (file == INVALID_HANDLE_VALUE) { error(path, GetLastError()); return 0; }
    data = allocate(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    if (!DeviceIoControl(file, FSCTL_GET_REPARSE_POINT, 0, 0, data, MAXIMUM_REPARSE_DATA_BUFFER_SIZE, &returned, 0))
        error(path, GetLastError());
    else {
        DWORD base = returned >= 8 && *(DWORD *)data == IO_REPARSE_TAG_SYMLINK ? 20 : 16;
        DWORD offset = returned >= 16 ? *(USHORT *)(data + 12) : 0;
        DWORD length = returned >= 16 ? *(USHORT *)(data + 14) : 0;
        int substitute = !length;
        if (substitute && returned >= 16) { offset = *(USHORT *)(data + 8); length = *(USHORT *)(data + 10); }
        if (returned < base || base + offset + length > returned || ((offset | length) & 1))
            error(path, ERROR_INVALID_REPARSE_DATA);
        else {
            WCHAR *target = (WCHAR *)(data + base + offset);
            if (substitute && length >= 8 && target[0] == L'\\' && target[1] == L'?' && target[2] == L'?' && target[3] == L'\\') {
                target += 4; length -= 8;
            }
            result = allocate(length + sizeof(WCHAR));
            for (DWORD i = 0; i < length / 2; ++i) result[i] = target[i];
            result[length / 2] = 0;
        }
    }
    HeapFree(heap, 0, data); CloseHandle(file);
    return result;
}

static void printentry(const Entry *e, const WCHAR *parent) {
    WCHAR *target = 0;
    int islink = e->tag == IO_REPARSE_TAG_SYMLINK || e->tag == IO_REPARSE_TAG_MOUNT_POINT;
    if (detailed && islink) {
        SIZE_T p = parent ? (SIZE_T)lstrlenW(parent) : 0, n = (SIZE_T)lstrlenW(e->name);
        WCHAR *path = allocate((p + n + 2) * sizeof(WCHAR));
        for (SIZE_T i = 0; i < p; ++i) path[i] = parent[i];
        if (p && path[p - 1] != L'\\' && path[p - 1] != L'/') path[p++] = L'\\';
        for (SIZE_T i = 0; i <= n; ++i) path[p + i] = e->name[i];
        target = linktarget(path); HeapFree(heap, 0, path);
    }
    if (detailed) {
        SYSTEMTIME st;
        FILETIME local;
        int isdir = (e->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        put(islink ? 'l' : isdir ? 'd' : '-');
        for (int i = 0; i < 3; ++i) {
            put('r'); put(e->attributes & FILE_ATTRIBUTE_READONLY ? '-' : 'w');
            put(isdir ? 'x' : '-');
        }
        text(" 1 - - ");
        number(target ? (ULONGLONG)WideCharToMultiByte(CP_UTF8, 0, target, lstrlenW(target), 0, 0, 0, 0) : e->size, 10); put(' ');
        if (FileTimeToLocalFileTime(&e->modified, &local) && FileTimeToSystemTime(&local, &st)) {
            number(st.wYear, 4); put('-'); put((char)('0' + st.wMonth / 10)); number(st.wMonth % 10, 0);
            put('-'); put((char)('0' + st.wDay / 10)); number(st.wDay % 10, 0); put(' ');
            put((char)('0' + st.wHour / 10)); number(st.wHour % 10, 0); put(':');
            put((char)('0' + st.wMinute / 10)); number(st.wMinute % 10, 0);
        } else text("????-??-?? ??:??");
        put(' ');
    }
    wide(e->name);
    if (target) { text(" -> "); wide(target); HeapFree(heap, 0, target); }
    put('\n');
}

static Entry *entry(const WIN32_FIND_DATAW *data, const WCHAR *name) {
    SIZE_T length = (SIZE_T)lstrlenW(name);
    Entry *e = allocate((SIZE_T)FIELD_OFFSET(Entry, name) + (length + 1) * sizeof(WCHAR));
    e->next = 0; e->attributes = data->dwFileAttributes;
    e->tag = data->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ? data->dwReserved0 : 0;
    e->modified.dwLowDateTime = data->ftLastWriteTime.dwLowDateTime;
    e->modified.dwHighDateTime = data->ftLastWriteTime.dwHighDateTime;
    e->size = ((ULONGLONG)data->nFileSizeHigh << 32) | data->nFileSizeLow;
    for (SIZE_T i = 0; i <= length; ++i) e->name[i] = name[i];
    return e;
}

static void list(const WCHAR *path, int heading) {
    DWORD attr = GetFileAttributesW(path), code;
    WIN32_FIND_DATAW data;
    HANDLE search;
    Entry *head = 0, *tail = 0;
    WCHAR *pattern;
    SIZE_T length;
    if (attr == INVALID_FILE_ATTRIBUTES) { error(path, GetLastError()); return; }
    data.dwReserved0 = 0;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
        search = FindFirstFileW(path, &data);
        if (search != INVALID_HANDLE_VALUE) FindClose(search);
    }
    if (directory || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
        (detailed && (data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT))) {
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) { error(path, GetLastError()); return; }
        data.dwFileAttributes = info.dwFileAttributes;
        data.ftLastWriteTime.dwLowDateTime = info.ftLastWriteTime.dwLowDateTime;
        data.ftLastWriteTime.dwHighDateTime = info.ftLastWriteTime.dwHighDateTime;
        data.nFileSizeHigh = info.nFileSizeHigh; data.nFileSizeLow = info.nFileSizeLow;
        head = entry(&data, path); printentry(head, 0); HeapFree(heap, 0, head); return;
    }
    if (heading) { wide(path); text(":\n"); }
    length = (SIZE_T)lstrlenW(path);
    pattern = allocate((length + 3) * sizeof(WCHAR));
    for (SIZE_T i = 0; i < length; ++i) pattern[i] = path[i];
    if (length && path[length - 1] != L'\\' && path[length - 1] != L'/') pattern[length++] = L'\\';
    pattern[length++] = L'*'; pattern[length] = 0;
    search = FindFirstFileW(pattern, &data);
    code = GetLastError(); HeapFree(heap, 0, pattern);
    if (search == INVALID_HANDLE_VALUE) { if (code != ERROR_FILE_NOT_FOUND) error(path, code); return; }
    do {
        const WCHAR *name = data.cFileName;
        int dot = name[0] == L'.' && (!name[1] || (name[1] == L'.' && !name[2]));
        Entry *e;
        if (!all && dot) continue;
        if (!all && !almost && (name[0] == L'.' || (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))) continue;
        e = entry(&data, name);
        if (unsorted) { printentry(e, path); HeapFree(heap, 0, e); }
        else { if (tail) tail->next = e; else head = e; tail = e; }
    } while (FindNextFileW(search, &data));
    code = GetLastError(); FindClose(search);
    if (code != ERROR_NO_MORE_FILES) error(path, code);
    head = sort(head);
    while (head) { Entry *next = head->next; printentry(head, path); HeapFree(heap, 0, head); head = next; }
    /* Re-enumerate after freeing the sorted listing, retaining no ancestor lists. */
    if (recursive) {
        length = (SIZE_T)lstrlenW(path);
        pattern = allocate((length + 3) * sizeof(WCHAR));
        for (SIZE_T i = 0; i < length; ++i) pattern[i] = path[i];
        if (length && path[length - 1] != L'\\' && path[length - 1] != L'/') pattern[length++] = L'\\';
        pattern[length] = L'*'; pattern[length + 1] = 0;
        search = FindFirstFileW(pattern, &data);
        code = GetLastError();
        if (search == INVALID_HANDLE_VALUE) {
            HeapFree(heap, 0, pattern);
            if (code != ERROR_FILE_NOT_FOUND) error(path, code);
            return;
        }
        do {
            SIZE_T n;
            WCHAR *child;
            const WCHAR *name = data.cFileName;
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (name[0] == L'.' && (!name[1] || (name[1] == L'.' && !name[2]))) continue;
            if (!all && !almost && (name[0] == L'.' || (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))) continue;
            n = (SIZE_T)lstrlenW(name);
            child = allocate((length + n + 1) * sizeof(WCHAR));
            for (SIZE_T i = 0; i < length; ++i) child[i] = pattern[i];
            for (SIZE_T i = 0; i <= n; ++i) child[length + i] = name[i];
            put('\n'); list(child, 1); HeapFree(heap, 0, child);
        } while (FindNextFileW(search, &data));
        code = GetLastError(); FindClose(search); HeapFree(heap, 0, pattern);
        if (code != ERROR_NO_MORE_FILES) error(path, code);
    }
}

void mainCRTStartup(void) {
    int argc, operands = 0, end = 0;
    WCHAR **argv;
    const WCHAR *command = GetCommandLineW();
    WCHAR *storage;
    heap = GetProcessHeap(); output = GetStdHandle(STD_OUTPUT_HANDLE);
    argc = arguments(command, 0, 0);
    argv = allocate((SIZE_T)argc * sizeof(WCHAR *));
    storage = allocate(((SIZE_T)lstrlenW(command) + 1) * sizeof(WCHAR));
    arguments(command, storage, argv);
    for (int i = 1; i < argc; ++i) {
        WCHAR *arg = argv[i];
        if (!end && !lstrcmpW(arg, L"--")) { end = 1; argv[i] = 0; continue; }
        if (!end && !lstrcmpW(arg, L"--help")) {
            text("Usage: ls [OPTION]... [FILE]...\n"
                 "  -a  include hidden entries and . and ..\n  -A  include hidden entries except . and ..\n"
                 "  -l  long listing (Windows attribute approximations)\n  -1  one entry per line (default)\n"
                 "  -r  reverse sort\n  -t  sort newest first\n  -S  sort largest first\n"
                 "  -d  list directories themselves\n  -U  stream without sorting\n  -R  recursively list subdirectories (skip reparse points)\n"
                 "  --  end options\n");
            flush(); ExitProcess(failed ? 1 : 0);
        }
        if (!end && arg[0] == L'-' && arg[1]) {
            for (int j = 1; arg[j]; ++j) switch (arg[j]) {
                case L'a': all = 1; break;
                case L'A': almost = 1; all = 0; break;
                case L'l': detailed = 1; break;
                case L'1': detailed = 0; break;
                case L'r': reverse = 1; break;
                case L't': bytime = 1; bysize = unsorted = 0; break;
                case L'S': bysize = 1; bytime = unsorted = 0; break;
                case L'd': directory = 1; break;
                case L'R': recursive = 1; break;
                case L'U': unsorted = 1; bytime = bysize = 0; break;
                default: error(arg, ERROR_INVALID_PARAMETER); flush(); ExitProcess(2);
            }
            argv[i] = 0;
        } else ++operands;
    }
    if (!operands) list(L".", recursive);
    else {
        SIZE_T count;
        GlobPath *paths = glob_operands(argc, argv, &count);
        Entry *files = 0, *filetail = 0, *dirs = 0, *dirtail = 0;
        int seen = 0;
        while (paths) {
            GlobPath *next = paths->next;
            WIN32_FILE_ATTRIBUTE_DATA info;
            WIN32_FIND_DATAW data;
            if (!GetFileAttributesExW(paths->value, GetFileExInfoStandard, &info))
                error(paths->value, GetLastError());
            else {
                Entry *e;
                int contents;
                data.dwReserved0 = 0;
                if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    HANDLE search = FindFirstFileW(paths->value, &data);
                    if (search != INVALID_HANDLE_VALUE) FindClose(search);
                }
                data.dwFileAttributes = info.dwFileAttributes;
                data.ftLastWriteTime = info.ftLastWriteTime;
                data.nFileSizeHigh = info.nFileSizeHigh; data.nFileSizeLow = info.nFileSizeLow;
                e = entry(&data, paths->value);
                contents = !directory && (e->attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    !(detailed && (e->tag == IO_REPARSE_TAG_SYMLINK || e->tag == IO_REPARSE_TAG_MOUNT_POINT));
                if (contents) {
                    if (dirtail) dirtail->next = e; else dirs = e;
                    dirtail = e;
                } else {
                    if (filetail) filetail->next = e; else files = e;
                    filetail = e;
                }
            }
            HeapFree(heap, 0, paths); paths = next;
        }
        if (!unsorted) { files = sort(files); dirs = sort(dirs); }
        while (files) {
            Entry *next = files->next;
            printentry(files, 0); seen = 1;
            HeapFree(heap, 0, files); files = next;
        }
        while (dirs) {
            Entry *next = dirs->next;
            if (seen++) put('\n');
            list(dirs->name, count > 1 || recursive);
            HeapFree(heap, 0, dirs); dirs = next;
        }
    }
    flush(); HeapFree(heap, 0, storage); HeapFree(heap, 0, argv); ExitProcess(failed ? 1 : 0);
}
