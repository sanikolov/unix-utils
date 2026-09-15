/* Operand expansion only: option values and --files0-from stay literal.
   Keep just enumeration frames and the resulting names, never directory lists. */
typedef struct GlobPath {
    struct GlobPath *next;
    WCHAR value[1];
} GlobPath;
typedef struct GlobFrame {
    struct GlobFrame *parent;
    HANDLE search;
    SIZE_T begin, end, length;
} GlobFrame;

static int glob_separator(WCHAR c) { return c == L'/' || c == L'\\'; }
static const WCHAR *glob_character(const WCHAR *s) {
    return s + (s[0] >= 0xd800 && s[0] <= 0xdbff && s[1] >= 0xdc00 && s[1] <= 0xdfff ? 2 : 1);
}

/* Component matching deliberately avoids DOS wildcard/8.3-name semantics. */
static int glob_component(const WCHAR *p, const WCHAR *end, const WCHAR *s) {
    const WCHAR *star = 0, *retry = 0;
    if (*s == L'.' && (p == end || *p != L'.')) return 0;
    while (*s) {
        const WCHAR *next = p;
        int matched = 0, whole_character = 0;
        if (p < end && *p == L'*') { star = ++p; retry = s; continue; }
        if (p < end && *p == L'?') { matched = whole_character = 1; next = p + 1; }
        else if (p < end && *p == L'[') {
            const WCHAR *q = p + 1;
            int negate = q < end && (*q == L'!' || *q == L'^'), first = 1;
            if (negate) ++q;
            while (q < end && (*q != L']' || first)) {
                WCHAR lo = *q++, hi = lo;
                first = 0;
                if (q + 1 < end && *q == L'-' && q[1] != L']') { hi = q[1]; q += 2; }
                if (*s >= lo && *s <= hi) matched = 1;
            }
            if (q < end) { if (negate) matched = !matched; next = q + 1; }
            else { matched = *s == L'['; next = p + 1; }
        } else if (p < end) { matched = *p == *s; next = p + 1; }
        if (matched) { p = next; s = whole_character ? glob_character(s) : s + 1; }
        else if (star && *retry) { p = star; s = retry = glob_character(retry); }
        else return 0;
    }
    while (p < end && *p == L'*') ++p;
    return p == end;
}

static GlobPath *glob_name(const WCHAR *name) {
    SIZE_T n = (SIZE_T)lstrlenW(name);
    GlobPath *p = allocate((SIZE_T)FIELD_OFFSET(GlobPath, value) + (n + 1) * sizeof(WCHAR));
    p->next = 0;
    for (SIZE_T i = 0; i <= n; ++i) p->value[i] = name[i];
    return p;
}
static GlobPath *glob_sort(GlobPath *head) {
    GlobPath *slow, *fast, *right, *tail = 0, *result = 0;
    if (!head || !head->next) return head;
    slow = head; fast = head->next;
    while (fast && fast->next) { slow = slow->next; fast = fast->next->next; }
    right = slow->next; slow->next = 0;
    head = glob_sort(head); right = glob_sort(right);
    while (head || right) {
        GlobPath *p;
        if (!right || (head && CompareStringOrdinal(head->value, -1, right->value, -1, FALSE) != CSTR_GREATER_THAN)) {
            p = head; head = head->next;
        } else { p = right; right = right->next; }
        if (tail) tail->next = p; else result = p;
        tail = p;
    }
    tail->next = 0;
    return result;
}

/* Do not mistake the '?' in an extended path prefix for a wildcard, and do
   not enumerate UNC server/share or drive names. */
static SIZE_T glob_root(const WCHAR *pattern) {
    SIZE_T i = 0;
    int unc = 0;
    if (glob_separator(pattern[0]) && glob_separator(pattern[1])) {
        i = 2; unc = 1;
        if (pattern[2] == L'?' && glob_separator(pattern[3])) {
            i = 4; unc = 0;
            if ((pattern[4] == L'U' || pattern[4] == L'u') &&
                (pattern[5] == L'N' || pattern[5] == L'n') &&
                (pattern[6] == L'C' || pattern[6] == L'c') && glob_separator(pattern[7])) { i = 8; unc = 1; }
        }
        if (unc) {
            for (int part = 0; part < 2; ++part) {
                while (pattern[i] && !glob_separator(pattern[i])) ++i;
                if (glob_separator(pattern[i])) ++i;
            }
            return i;
        }
    }
    if (pattern[i] && pattern[i + 1] == L':') i += 2;
    while (glob_separator(pattern[i])) ++i;
    return i;
}

static void glob_reserve(WCHAR **path, SIZE_T *capacity, SIZE_T needed) {
    if (needed > *capacity) {
        WCHAR *p;
        *capacity = needed + 256;
        p = *path ? HeapReAlloc(heap, 0, *path, *capacity * sizeof(WCHAR)) : allocate(*capacity * sizeof(WCHAR));
        if (!p) { error(L"out of memory", ERROR_NOT_ENOUGH_MEMORY); ExitProcess(FAILURE_CODE); }
        *path = p;
    }
}

/* A temporary extended absolute path is used for Win32 queries only. */
static WCHAR *glob_native(const WCHAR *path) {
    WCHAR *result;
    DWORD n;
    if (path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' && path[3] == L'\\') {
        n = (DWORD)lstrlenW(path) + 1;
        result = allocate((SIZE_T)n * sizeof(WCHAR));
        for (DWORD i = 0; i < n; ++i) result[i] = path[i] == L'/' ? L'\\' : path[i];
    } else {
        n = GetFullPathNameW(path, 0, 0, 0);
        if (!n) return 0;
        result = allocate(((SIZE_T)n + 8) * sizeof(WCHAR));
        if (!GetFullPathNameW(path, n, result + 8, 0)) { HeapFree(heap, 0, result); return 0; }
        if (result[8] == L'\\' && result[9] == L'\\') {
            result[0] = L'\\'; result[1] = L'\\'; result[2] = L'?'; result[3] = L'\\';
            result[4] = L'U'; result[5] = L'N'; result[6] = L'C'; result[7] = L'\\';
            for (DWORD i = 0; i < n - 2; ++i) result[i + 8] = result[i + 10];
        } else {
            result[0] = L'\\'; result[1] = L'\\'; result[2] = L'?'; result[3] = L'\\';
            for (DWORD i = 0; i < n; ++i) result[i + 4] = result[i + 8];
        }
    }
    return result;
}

static GlobPath *glob_expand(const WCHAR *pattern) {
    SIZE_T root = glob_root(pattern), cursor = root, length = root, capacity = 0;
    WCHAR *work = 0, *query;
    GlobFrame *top = 0;
    GlobPath *matches = 0;
    WIN32_FIND_DATAW found;
    int magic = 0;
    for (SIZE_T i = root; pattern[i]; ++i) if (pattern[i] == L'*' || pattern[i] == L'?' || pattern[i] == L'[') magic = 1;
    if (!magic) return glob_name(pattern);
    glob_reserve(&work, &capacity, root + 1);
    for (SIZE_T i = 0; i < root; ++i) work[i] = pattern[i];
    work[length] = 0;
component:
    if (!pattern[cursor]) {
        DWORD attr;
        query = glob_native(work);
        attr = query ? GetFileAttributesW(query) : INVALID_FILE_ATTRIBUTES;
        if (query) HeapFree(heap, 0, query);
        if (attr != INVALID_FILE_ATTRIBUTES && (!length || !glob_separator(work[length - 1]) || (attr & FILE_ATTRIBUTE_DIRECTORY))) {
            GlobPath *p = glob_name(work); p->next = matches; matches = p;
        }
        goto advance;
    }
    {
        SIZE_T end = cursor;
        magic = 0;
        while (pattern[end] && !glob_separator(pattern[end])) {
            if (pattern[end] == L'*' || pattern[end] == L'?' || pattern[end] == L'[') magic = 1;
            ++end;
        }
        if (!magic) {
            while (glob_separator(pattern[end])) ++end;
            glob_reserve(&work, &capacity, length + end - cursor + 1);
            while (cursor < end) work[length++] = pattern[cursor++];
            work[length] = 0;
            goto component;
        } else {
            HANDLE search;
            glob_reserve(&work, &capacity, length + 2);
            work[length] = L'*'; work[length + 1] = 0;
            query = glob_native(work);
            search = query ? FindFirstFileW(query, &found) : INVALID_HANDLE_VALUE;
            if (query) HeapFree(heap, 0, query);
            work[length] = 0;
            if (search == INVALID_HANDLE_VALUE) goto advance;
            {
                GlobFrame *f = allocate(sizeof(GlobFrame));
                f->parent = top; f->search = search; f->begin = cursor; f->end = end; f->length = length; top = f;
            }
        }
    }
candidate:
    if (!(found.cFileName[0] == L'.' && (!found.cFileName[1] || (found.cFileName[1] == L'.' && !found.cFileName[2]))) &&
        (!pattern[top->end] || (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) &&
        glob_component(pattern + top->begin, pattern + top->end, found.cFileName)) {
        SIZE_T n = (SIZE_T)lstrlenW(found.cFileName), end = top->end;
        while (glob_separator(pattern[end])) ++end;
        length = top->length;
        glob_reserve(&work, &capacity, length + n + end - top->end + 1);
        for (SIZE_T i = 0; i < n; ++i) work[length++] = found.cFileName[i];
        for (SIZE_T i = top->end; i < end; ++i) work[length++] = pattern[i];
        work[length] = 0; cursor = end;
        goto component;
    }
advance:
    while (top) {
        GlobFrame *parent;
        if (FindNextFileW(top->search, &found)) goto candidate;
        FindClose(top->search); parent = top->parent; HeapFree(heap, 0, top); top = parent;
    }
    HeapFree(heap, 0, work);
    return matches ? glob_sort(matches) : glob_name(pattern);
}

static GlobPath *glob_operands(int argc, WCHAR **argv, SIZE_T *count) {
    GlobPath *result = 0, *tail = 0;
    *count = 0;
    for (int i = 1; i < argc; ++i) if (argv[i]) {
        GlobPath *matches = glob_expand(argv[i]);
        if (tail) tail->next = matches; else result = matches;
        while (matches) { ++*count; tail = matches; matches = matches->next; }
    }
    return result;
}
