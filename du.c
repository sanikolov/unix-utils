#define PROGRAM_NAME "du"
#define FAILURE_CODE 1
#define _WIN32_WINNT 0x0602
#include "tiny.h"
#include <winioctl.h>

typedef struct { ULONGLONG volume, low, high; } Identity;
typedef struct { ULONGLONG size, time; } Total;
typedef struct {
    Identity id;
    Total total;
    int directory;
    DWORD links;
} Metadata;
typedef struct Frame {
    struct Frame *parent;
    HANDLE search;
    SIZE_T length;
    DWORD depth;
    Identity id;
    Total total;
} Frame;
typedef struct Pattern { struct Pattern *next; WCHAR value[1]; } Pattern;
typedef struct { Identity id; int occupied; } Slot;

static int opt_all, apparent, count_links, grand, separate, inodes, follow, onefs, nul;
static int human, suffix_output, grouping, time_kind, hash_all, threshold_negative;
static int time_style_environment = 1;
static ULONGLONG block = 1024, threshold;
static DWORD maxdepth = MAXDWORD;
static WCHAR suffix[5], *time_style, *files_from;
static Pattern *patterns;
static Total grand_total;
static Slot *seen;
static SIZE_T seen_capacity, seen_count;
static WCHAR *path, *native_buffer;
static SIZE_T path_capacity, native_capacity;

static int equal(const WCHAR *a, const WCHAR *b) { return lstrcmpW(a, b) == 0; }
static int prefix(const WCHAR *abbreviation, const WCHAR *word) {
    if (!*abbreviation) return 0;
    while (*abbreviation && *abbreviation == *word) { ++abbreviation; ++word; }
    return !*abbreviation;
}
static void release(void *p) { if (p) HeapFree(heap, 0, p); }
static void *resize(void *p, SIZE_T size) {
    void *r = p ? HeapReAlloc(heap, 0, p, size) : allocate(size);
    if (!r) { error(L"out of memory", ERROR_NOT_ENOUGH_MEMORY); ExitProcess(1); }
    return r;
}
static WCHAR *copy(const WCHAR *s) {
    SIZE_T n = (SIZE_T)lstrlenW(s) + 1;
    WCHAR *r = allocate(n * sizeof(WCHAR));
    for (SIZE_T i = 0; i < n; ++i) r[i] = s[i];
    return r;
}
static void invalid(const WCHAR *s) { error(s, ERROR_INVALID_PARAMETER); ExitProcess(1); }
static WCHAR *environment(const WCHAR *name) {
    DWORD n = GetEnvironmentVariableW(name, 0, 0);
    WCHAR *s;
    if (!n) return 0;
    s = allocate((SIZE_T)n * sizeof(WCHAR));
    if (!GetEnvironmentVariableW(name, s, n)) { release(s); return 0; }
    return s;
}

/* Checked integer arithmetic; disk usage never wraps around on overflow. */
static void add(Total *a, const Total *b) {
    ULONGLONG sum = a->size + b->size;
    a->size = sum < a->size ? ~(ULONGLONG)0 : sum;
    if (b->time > a->time) a->time = b->time;
}
static ULONGLONG quantity(const WCHAR *s, int allow_zero) {
    const WCHAR *original = s;
    ULONGLONG n = 0, base = 1024;
    int digits = 0, power = 0;
    if (*s == L'+') ++s;
    while (*s >= L'0' && *s <= L'9') {
        unsigned d = (unsigned)(*s++ - L'0');
        if (n > (~(ULONGLONG)0 - d) / 10) invalid(original);
        n = n * 10 + d; digits = 1;
    }
    if (!digits) n = 1;
    if (*s) {
        WCHAR c = *s++;
        if (c == L'b') { base = 512; power = 1; }
        else if (c == L'w') { base = 2; power = 1; }
        else if (c == L'c') power = 0;
        else {
            const WCHAR *units = L"KMGTPEZYRQ";
            if (c == L'k') c = L'K';
            while (units[power] && units[power] != c) ++power;
            if (!units[power]) invalid(original);
            ++power;
            if (*s == L'B') { base = 1000; ++s; }
            else if (s[0] == L'i' && s[1] == L'B') s += 2;
        }
    } else if (!digits) invalid(original);
    if (*s || (!n && !allow_zero)) invalid(original);
    while (power--) { if (n > ~(ULONGLONG)0 / base) invalid(original); n *= base; }
    return n;
}
static void block_size(const WCHAR *s) {
    const WCHAR *start;
    human = suffix_output = grouping = 0; suffix[0] = 0;
    if (*s == L'\'') { grouping = 1; ++s; }
    if (equal(s, L"human-readable")) { human = 1024; return; }
    if (equal(s, L"si")) { human = 1000; return; }
    start = s;
    block = quantity(s, 0);
    if ((*start < L'0' || *start > L'9') && *start != L'+') {
        SIZE_T n = (SIZE_T)lstrlenW(start);
        if (n > 4) invalid(start);
        for (SIZE_T i = 0; i <= n; ++i) suffix[i] = start[i];
        if (suffix[0] == L'K' && suffix[1] == L'B') suffix[0] = L'k';
        suffix_output = 1;
    }
}

static void reserve_path(SIZE_T n) {
    if (n > path_capacity) {
        path_capacity = n + 256;
        path = resize(path, path_capacity * sizeof(WCHAR));
    }
}
static void set_path(const WCHAR *s) {
    SIZE_T n = (SIZE_T)lstrlenW(s);
    reserve_path(n + 1);
    for (SIZE_T i = 0; i <= n; ++i) path[i] = s[i];
}
static void append_path(SIZE_T length, const WCHAR *name) {
    SIZE_T n = (SIZE_T)lstrlenW(name);
    reserve_path(length + n + 2);
    if (length && path[length - 1] != L'/' && path[length - 1] != L'\\') path[length++] = L'/';
    for (SIZE_T i = 0; i <= n; ++i) path[length + i] = name[i];
}
/* Extended absolute paths permit deep trees without a MAX_PATH-sized stack. */
static const WCHAR *native(const WCHAR *s) {
    DWORD n;
    SIZE_T offset;
    if (s[0] == L'\\' && s[1] == L'\\' && s[2] == L'?' && s[3] == L'\\') {
        SIZE_T length = (SIZE_T)lstrlenW(s);
        if (length + 1 > native_capacity) { native_capacity = length + 257; native_buffer = resize(native_buffer, native_capacity * sizeof(WCHAR)); }
        for (SIZE_T i = 0; i <= length; ++i) native_buffer[i] = s[i] == L'/' ? L'\\' : s[i];
        return native_buffer;
    }
    n = GetFullPathNameW(s, 0, 0, 0);
    if (!n) return s;
    if ((SIZE_T)n + 8 > native_capacity) {
        native_capacity = (SIZE_T)n + 264;
        native_buffer = resize(native_buffer, native_capacity * sizeof(WCHAR));
    }
    if (!GetFullPathNameW(s, n, native_buffer + 8, 0)) return s;
    offset = native_buffer[8] == L'\\' && native_buffer[9] == L'\\' ? 2 : 4;
    if (offset == 2) {
        native_buffer[2] = L'\\'; native_buffer[3] = L'\\'; native_buffer[4] = L'?'; native_buffer[5] = L'\\';
        native_buffer[6] = L'U'; native_buffer[7] = L'N'; native_buffer[8] = L'C'; native_buffer[9] = L'\\';
    } else { native_buffer[4] = L'\\'; native_buffer[5] = L'\\'; native_buffer[6] = L'?'; native_buffer[7] = L'\\'; }
    return native_buffer + offset;
}

static int same(const Identity *a, const Identity *b) {
    return a->volume == b->volume && a->low == b->low && a->high == b->high;
}
static SIZE_T hash(const Identity *id) {
    ULONGLONG n = id->low ^ id->high ^ id->volume;
    n ^= n >> 30; n *= 0xbf58476d1ce4e5b9ULL; n ^= n >> 27;
    return (SIZE_T)n;
}
static int remember(const Identity *id) {
    SIZE_T index;
    if (!seen_capacity || seen_count * 4 >= seen_capacity * 3) {
        SIZE_T old_capacity = seen_capacity;
        Slot *old = seen;
        seen_capacity = old_capacity ? old_capacity * 2 : 64;
        seen = HeapAlloc(heap, HEAP_ZERO_MEMORY, seen_capacity * sizeof(Slot));
        if (!seen) { error(L"out of memory", ERROR_NOT_ENOUGH_MEMORY); ExitProcess(1); }
        for (SIZE_T i = 0; i < old_capacity; ++i) if (old[i].occupied) {
            index = hash(&old[i].id) & (seen_capacity - 1);
            while (seen[index].occupied) index = (index + 1) & (seen_capacity - 1);
            seen[index].id = old[i].id; seen[index].occupied = 1;
        }
        release(old);
    }
    index = hash(id) & (seen_capacity - 1);
    while (seen[index].occupied) {
        if (same(&seen[index].id, id)) return 0;
        index = (index + 1) & (seen_capacity - 1);
    }
    seen[index].id = *id; seen[index].occupied = 1; ++seen_count;
    return 1;
}

static int metadata(Metadata *m, DWORD depth) {
    FILE_STANDARD_INFO standard;
    FILE_BASIC_INFO basic;
    FILE_ATTRIBUTE_TAG_INFO tag;
    FILE_ID_INFO identity;
    HANDLE file;
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    DWORD link_tag = 0;
    int physical = follow == 0 || (follow == 1 && depth != 0);
    if (physical) flags |= FILE_FLAG_OPEN_REPARSE_POINT;
open_file:
    file = CreateFileW(native(path), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       0, OPEN_EXISTING, flags, 0);
    if (file == INVALID_HANDLE_VALUE) { error(path, GetLastError()); return 0; }
    if (!GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof standard) ||
        !GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof basic)) {
        error(path, GetLastError()); CloseHandle(file); return 0;
    }
    m->directory = standard.Directory != 0;
    if (physical && (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof tag)) {
            error(path, GetLastError()); CloseHandle(file); return 0;
        }
        if (tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
            const WCHAR *source = native(path);
            SIZE_T n = (SIZE_T)lstrlenW(source);
            WCHAR volume[50], *mount = allocate((n + 2) * sizeof(WCHAR));
            BOOL mounted;
            for (SIZE_T i = 0; i < n; ++i) mount[i] = source[i];
            if (n && mount[n - 1] != L'\\') mount[n++] = L'\\';
            mount[n] = 0;
            mounted = GetVolumeNameForVolumeMountPointW(mount, volume, 50); release(mount);
            if (mounted) {
                CloseHandle(file); physical = 0; flags &= ~FILE_FLAG_OPEN_REPARSE_POINT;
                goto open_file;
            }
        }
        if (IsReparseTagNameSurrogate(tag.ReparseTag)) { m->directory = 0; link_tag = tag.ReparseTag; }
    }
    m->links = standard.NumberOfLinks;
    if (GetFileInformationByHandleEx(file, FileIdInfo, &identity, sizeof identity)) {
        m->id.volume = identity.VolumeSerialNumber;
        m->id.low = m->id.high = 0;
        for (int i = 0; i < 8; ++i) {
            m->id.low |= (ULONGLONG)identity.FileId.Identifier[i] << (8 * i);
            m->id.high |= (ULONGLONG)identity.FileId.Identifier[i + 8] << (8 * i);
        }
    } else {
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(file, &info)) { error(path, GetLastError()); CloseHandle(file); return 0; }
        m->id.volume = info.dwVolumeSerialNumber;
        m->id.low = ((ULONGLONG)info.nFileIndexHigh << 32) | info.nFileIndexLow;
        m->id.high = 0;
    }
    m->total.time = (ULONGLONG)(time_kind == 2 ? basic.LastAccessTime.QuadPart :
                               time_kind == 3 ? basic.ChangeTime.QuadPart : basic.LastWriteTime.QuadPart);
    m->total.size = inodes ? 1 : (ULONGLONG)(apparent ? standard.EndOfFile.QuadPart : standard.AllocationSize.QuadPart);
    if (!inodes && apparent && (link_tag == IO_REPARSE_TAG_SYMLINK || link_tag == IO_REPARSE_TAG_MOUNT_POINT)) {
        BYTE *data = allocate(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
        DWORD returned;
        if (!DeviceIoControl(file, FSCTL_GET_REPARSE_POINT, 0, 0, data, MAXIMUM_REPARSE_DATA_BUFFER_SIZE, &returned, 0)) {
            error(path, GetLastError()); release(data); CloseHandle(file); return 0;
        }
        {
            DWORD base = link_tag == IO_REPARSE_TAG_SYMLINK ? 20 : 16;
            DWORD offset = returned >= 16 ? *(USHORT *)(data + 12) : 0;
            DWORD length = returned >= 16 ? *(USHORT *)(data + 14) : 0;
            int substitute = !length;
            if (substitute && returned >= 16) { offset = *(USHORT *)(data + 8); length = *(USHORT *)(data + 10); }
            if (returned < base || base + offset + length > returned || (length & 1)) {
                error(path, ERROR_INVALID_REPARSE_DATA); release(data); CloseHandle(file); return 0;
            }
            {
                WCHAR *target = (WCHAR *)(data + base + offset);
                if (substitute && length >= 8 && target[0] == L'\\' && target[1] == L'?' && target[2] == L'?' && target[3] == L'\\') {
                    target += 4; length -= 8;
                }
                m->total.size = (ULONGLONG)WideCharToMultiByte(CP_UTF8, 0, target, (int)(length / 2), 0, 0, 0, 0);
            }
        }
        release(data);
    }
    if (!inodes && !apparent && (basic.FileAttributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE))) {
        FILE_COMPRESSION_INFO compression;
        if (!GetFileInformationByHandleEx(file, FileCompressionInfo, &compression, sizeof compression)) {
            error(path, GetLastError()); CloseHandle(file); return 0;
        }
        m->total.size = (ULONGLONG)compression.CompressedFileSize.QuadPart;
    }
    CloseHandle(file);
    return 1;
}

#include "du_match.h"
#include "du_format.h"

static void print_total(const Total *total, const WCHAR *name, int force) {
    if (!force && (threshold_negative ? total->size > threshold : total->size < threshold)) return;
    print_size(total->size);
    if (time_kind) { put('\t'); print_time(total->time); }
    put('\t'); wide(name); put(nul ? '\0' : '\n');
}

/* Iterative postorder traversal: no C recursion and no stored directory lists. */
static void walk(const WCHAR *operand) {
    Frame *top = 0;
    DWORD depth = 0;
    ULONGLONG root_volume = 0;
    WIN32_FIND_DATAW found;
    Metadata m;
    set_path(operand);
visit:
    if (!excluded(path) && metadata(&m, depth)) {
        int accept = 1;
        if (!depth) root_volume = m.id.volume;
        if (onefs && depth && m.id.volume != root_volume) accept = 0;
        if (accept && m.directory) for (Frame *f = top; f; f = f->parent) if (same(&m.id, &f->id)) {
            /* GNU logical traversal also prunes ancestors without counting them. */
            accept = 0; break;
        }
        if (accept && !count_links && (hash_all || (!m.directory && m.links > 1))) accept = remember(&m.id);
        if (accept) {
            add(&grand_total, &m.total);
            if (m.directory) {
                Frame *f = allocate(sizeof(Frame));
                f->parent = top; f->search = 0; f->length = (SIZE_T)lstrlenW(path);
                f->depth = depth; f->id = m.id; f->total = m.total; top = f;
            } else {
                if (!depth || (opt_all && depth <= maxdepth)) print_total(&m.total, path, 0);
                if (top) add(&top->total, &m.total);
            }
        }
    }
    while (top) {
        BOOL ok;
        DWORD code;
        path[top->length] = 0;
        if (!top->search) {
            append_path(top->length, L"*");
            top->search = FindFirstFileW(native(path), &found);
            code = GetLastError(); path[top->length] = 0;
            ok = top->search != INVALID_HANDLE_VALUE;
            if (!ok && code == ERROR_FILE_NOT_FOUND) code = ERROR_NO_MORE_FILES;
        } else { ok = FindNextFileW(top->search, &found); code = GetLastError(); }
        if (ok) {
            if (found.cFileName[0] == L'.' && (!found.cFileName[1] || (found.cFileName[1] == L'.' && !found.cFileName[2]))) continue;
            append_path(top->length, found.cFileName); depth = top->depth + 1;
            goto visit;
        }
        if (code != ERROR_NO_MORE_FILES) error(path, code);
        if (top->search != INVALID_HANDLE_VALUE) FindClose(top->search);
        if (top->depth <= maxdepth) print_total(&top->total, path, 0);
        {
            Frame *parent = top->parent;
            if (parent && !separate) add(&parent->total, &top->total);
            release(top); top = parent;
        }
    }
}

/* Read names/patterns incrementally; --files0-from never retains its file list. */
static void consume_input(const char *s, SIZE_T length, int names);
static void input_file(const WCHAR *name, int names) {
    HANDLE file = equal(name, L"-") ? GetStdHandle(STD_INPUT_HANDLE) :
        CreateFileW(native(name), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    char *bytes = allocate(4096), *item = 0;
    SIZE_T length = 0, capacity = 0;
    DWORD count;
    if (file == INVALID_HANDLE_VALUE) { error(name, GetLastError()); release(bytes); return; }
    for (;;) {
        BOOL ok = ReadFile(file, bytes, 4096, &count, 0);
        if (!ok) {
            DWORD code = GetLastError();
            if (code != ERROR_BROKEN_PIPE) { error(name, code); break; }
            count = 0;
        }
        for (DWORD i = 0; i < count; ++i) {
            if (bytes[i] != (names ? '\0' : '\n')) {
                if (length + 1 >= capacity) { capacity = capacity ? capacity * 2 : 256; item = resize(item, capacity); }
                item[length++] = bytes[i];
            } else {
                if (!names && length && item[length - 1] == '\r') --length;
                consume_input(item, length, names);
                length = 0;
            }
        }
        if (!count) { if (length) consume_input(item, length, names); break; }
    }
    if (!equal(name, L"-")) CloseHandle(file);
    release(bytes); release(item);
}

static void consume_input(const char *s, SIZE_T length, int names) {
    int n;
    WCHAR *value;
    if (!length) { if (names) error(L"empty file name in --files0-from", ERROR_INVALID_NAME); return; }
    if (length > MAXINT) { error(L"input entry too long", ERROR_FILENAME_EXCED_RANGE); return; }
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)length, 0, 0);
    if (!n) { error(L"input is not UTF-8", GetLastError()); return; }
    value = allocate(((SIZE_T)n + 1) * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)length, value, n); value[n] = 0;
    if (names && files_from && equal(files_from, L"-") && equal(value, L"-")) error(L"'-' cannot be an operand when names come from stdin", ERROR_INVALID_NAME);
    else if (names) walk(value); else add_pattern(value);
    release(value);
}

enum { APPARENT = 256, SI, INODES, EXCLUDE, FILES_FROM, TIME, TIME_STYLE, HELP, VERSION };
typedef struct { const WCHAR *name; int code, argument; } Option;
static const Option options[] = {
    { L"all", 'a', 0 }, { L"apparent-size", APPARENT, 0 }, { L"block-size", 'B', 1 },
    { L"bytes", 'b', 0 }, { L"total", 'c', 0 }, { L"dereference-args", 'D', 0 },
    { L"max-depth", 'd', 1 }, { L"files0-from", FILES_FROM, 1 }, { L"human-readable", 'h', 0 },
    { L"inodes", INODES, 0 }, { L"dereference", 'L', 0 }, { L"count-links", 'l', 0 },
    { L"no-dereference", 'P', 0 }, { L"null", '0', 0 }, { L"separate-dirs", 'S', 0 },
    { L"summarize", 's', 0 }, { L"si", SI, 0 }, { L"one-file-system", 'x', 0 },
    { L"threshold", 't', 1 }, { L"exclude", EXCLUDE, 1 }, { L"exclude-from", 'X', 1 },
    { L"time", TIME, 2 }, { L"time-style", TIME_STYLE, 1 },
    { L"help", HELP, 0 }, { L"version", VERSION, 0 }
};
static void help(void) {
    text("Usage: du [OPTION]... [FILE]...\n       du [OPTION]... --files0-from=FILE\n"
         "  -0, --null                 NUL-terminate output records\n"
         "  -a, --all                  report files as well as directories\n"
         "  -A, --apparent-size        logical length instead of allocated bytes\n"
         "  -B, --block-size=SIZE      output units (K, KiB, MB, etc.)\n"
         "  -b, --bytes                apparent size in bytes\n"
         "  -c, --total                append grand total\n"
         "  -d, --max-depth=N          limit displayed depth, starting at zero\n"
         "  -D, -H, --dereference-args follow only operand links\n"
         "  -L, --dereference          follow links throughout traversal\n"
         "  -P, --no-dereference       measure links themselves (default)\n"
         "  -h, --human-readable       powers of 1024\n"
         "      --si                   powers of 1000\n"
         "      --inodes               count Windows file identities\n"
         "  -k / -m                    1024 / 1048576 byte output units\n"
         "  -l, --count-links          count each hard-link occurrence\n"
         "  -S, --separate-dirs        exclude subdirectories from parent totals\n"
         "  -s, --summarize            report only operands\n"
         "  -t, --threshold=SIZE       minimum size; negative means maximum\n"
         "  -x, --one-file-system      stay on each operand's volume\n"
         "      --exclude=PATTERN     skip matching paths\n"
         "  -X, --exclude-from=FILE    read UTF-8 exclusion patterns; - is stdin\n"
         "      --files0-from=FILE    read NUL-separated UTF-8 operands; - is stdin\n"
         "      --time[=WORD]         latest mtime, atime/access/use, ctime/status\n"
         "      --time-style=STYLE    full-iso, long-iso, iso, or +FORMAT\n"
         "      --help / --version    usage / implementation version\n"
         "Units: --block-size, DU_BLOCK_SIZE, BLOCK_SIZE, BLOCKSIZE, then 1024\n"
         "(512 with POSIXLY_CORRECT). Windows allocation and timestamp rules apply.\n");
}

void mainCRTStartup(void) {
    const WCHAR *command = GetCommandLineW();
    WCHAR **argv, *storage, *env;
    int argc, operands = 0, end = 0, summarize = 0, depth_set = 0, posix;
    heap = GetProcessHeap(); output = GetStdHandle(STD_OUTPUT_HANDLE);
    env = environment(L"POSIXLY_CORRECT"); posix = env != 0; release(env);
    if (posix) block = 512;
    env = environment(L"DU_BLOCK_SIZE");
    if (!env) env = environment(L"BLOCK_SIZE");
    if (!env) env = environment(L"BLOCKSIZE");
    if (env) { block_size(env); release(env); }
    time_style = environment(L"TIME_STYLE");
    argc = arguments(command, 0, 0);
    argv = allocate((SIZE_T)argc * sizeof(WCHAR *));
    storage = allocate(((SIZE_T)lstrlenW(command) + 1) * sizeof(WCHAR));
    arguments(command, storage, argv);
    for (int i = 1; i < argc; ++i) {
        WCHAR *arg = argv[i];
        int j = 1;
        if (!end && equal(arg, L"--")) { end = 1; argv[i] = 0; continue; }
        if (end || arg[0] != L'-' || !arg[1]) { ++operands; if (posix) end = 1; continue; }
        argv[i] = 0;
        do {
            int code, needs = 0;
            WCHAR *value = 0;
            if (arg[1] == L'-') {
                SIZE_T n = 0;
                int match = -1;
                while (arg[n + 2] && arg[n + 2] != L'=') ++n;
                if (!n) invalid(arg);
                for (int o = 0; o < (int)(sizeof options / sizeof options[0]); ++o) {
                    SIZE_T k = 0;
                    while (k < n && options[o].name[k] && options[o].name[k] == arg[k + 2]) ++k;
                    if (k == n) {
                        if (!options[o].name[k]) { match = o; break; }
                        if (match != -1) match = -2; else match = o;
                    }
                }
                if (match < 0) invalid(arg);
                code = options[match].code; needs = options[match].argument;
                if (arg[n + 2] == L'=') { if (!needs) invalid(arg); value = arg + n + 3; }
                j = (int)lstrlenW(arg);
            } else {
                code = arg[j++];
                needs = code == 'B' || code == 'd' || code == 't' || code == 'X';
                if (needs && arg[j]) { value = arg + j; j = (int)lstrlenW(arg); }
            }
            if (needs == 1 && !value) { if (++i == argc) invalid(arg); value = argv[i]; argv[i] = 0; }
            switch (code) {
                case 'a': opt_all = 1; break;
                case 'A': case APPARENT: apparent = 1; break;
                case 'B': block_size(value); break;
                case 'b': apparent = 1; block_size(L"1"); break;
                case 'c': grand = 1; break;
                case 'D': case 'H': follow = 1; break;
                case 'L': follow = 2; break;
                case 'P': follow = 0; break;
                case 'd': {
                    ULONGLONG n = 0;
                    if (!*value) invalid(value);
                    for (WCHAR *p = value; *p; ++p) { if (*p < L'0' || *p > L'9' || n > MAXDWORD / 10) invalid(value); n = n * 10 + (*p - L'0'); }
                    if (n > MAXDWORD) invalid(value); maxdepth = (DWORD)n; depth_set = 1; break;
                }
                case 'h': block_size(L"human-readable"); break;
                case SI: block_size(L"si"); break;
                case INODES: inodes = 1; break;
                case 'k': block_size(L"1024"); break;
                case 'm': block_size(L"1048576"); break;
                case 'l': count_links = 1; break;
                case '0': nul = 1; break;
                case 'S': separate = 1; break;
                case 's': summarize = 1; break;
                case 'x': onefs = 1; break;
                case 't':
                    threshold_negative = *value == L'-'; threshold = quantity(value + threshold_negative, 1);
                    if (threshold > 0x7fffffffffffffffULL + threshold_negative) invalid(value);
                    if (!threshold && threshold_negative) invalid(value);
                    break;
                case EXCLUDE: add_pattern(value); break;
                case 'X': input_file(value, 0); break;
                case FILES_FROM: files_from = value; break;
                case TIME:
                    if (!value) time_kind = 1;
                    else if (prefix(value, L"atime") || prefix(value, L"access") || prefix(value, L"use")) time_kind = 2;
                    else if (prefix(value, L"ctime") || prefix(value, L"status")) time_kind = 3;
                    else invalid(value);
                    break;
                case TIME_STYLE: release(time_style); time_style = copy(value); time_style_environment = 0; break;
                case HELP: help(); flush(); ExitProcess(failed ? 1 : 0);
                case VERSION: text("du (unix-utils for Windows) 1.0\n"); flush(); ExitProcess(failed ? 1 : 0);
                default: invalid(arg);
            }
        } while (arg[j]);
    }
    if (summarize && (opt_all || (depth_set && maxdepth))) invalid(L"conflicting summary/depth/all options");
    if (summarize) maxdepth = 0;
    if (files_from && operands) invalid(L"--files0-from cannot be combined with operands");
    if (inodes) { block = 1; suffix_output = 0; }
    if (human || grouping) prepare_numeric();
    if (time_kind) prepare_time();
    hash_all = follow == 2 || operands > 1 || files_from != 0;
    if (files_from) input_file(files_from, 1);
    else if (!operands) walk(L".");
    else for (int i = 1; i < argc; ++i) if (argv[i]) walk(argv[i]);
    if (grand) print_total(&grand_total, L"total", 1);
    flush(); ExitProcess(failed ? 1 : 0);
}
