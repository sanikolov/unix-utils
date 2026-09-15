/* GNU-style unanchored shell patterns, matched case-sensitively. Windows path
   separators are normalized to '/'; backslashes in patterns quote a character. */
static WCHAR pathchar(WCHAR c) { return c == L'\\' ? L'/' : c; }

static int character_class(const WCHAR *name, SIZE_T n, WCHAR c) {
    static const WCHAR *names[] = { L"alnum", L"alpha", L"blank", L"cntrl", L"digit", L"graph", L"lower", L"print", L"punct", L"space", L"upper", L"xdigit" };
    WORD type = 0;
    int index = -1;
    GetStringTypeW(CT_CTYPE1, &c, 1, &type);
    for (int i = 0; i < 12; ++i) {
        SIZE_T j = 0;
        while (j < n && names[i][j] == name[j]) ++j;
        if (j == n && !names[i][j]) { index = i; break; }
    }
    switch (index) {
        case 0: return (type & (C1_ALPHA | C1_DIGIT)) != 0;
        case 1: return (type & C1_ALPHA) != 0;
        case 2: return c == L' ' || c == L'\t';
        case 3: return (type & C1_CNTRL) != 0;
        case 4: return (type & C1_DIGIT) != 0;
        case 5: return !(type & (C1_SPACE | C1_CNTRL)) && c != 0;
        case 6: return (type & C1_LOWER) != 0;
        case 7: return !(type & C1_CNTRL) && c != 0;
        case 8: return (type & C1_PUNCT) != 0;
        case 9: return (type & C1_SPACE) != 0;
        case 10: return (type & C1_UPPER) != 0;
        case 11: return (type & C1_XDIGIT) != 0;
        default: return 0;
    }
}

static int bracket(const WCHAR **pattern, WCHAR c) {
    const WCHAR *p = *pattern + 1;
    int negate = *p == L'!' || *p == L'^', matched = 0, first = 1;
    if (negate) ++p;
    while (*p && (*p != L']' || first)) {
        WCHAR lo, hi;
        first = 0;
        if (p[0] == L'[' && p[1] == L':') {
            const WCHAR *end = p + 2;
            while (*end && !(end[0] == L':' && end[1] == L']')) ++end;
            if (*end) { matched |= character_class(p + 2, (SIZE_T)(end - p - 2), c); p = end + 2; continue; }
        }
        if (*p == L'\\' && p[1]) ++p;
        lo = *p++; hi = lo;
        if (*p == L'-' && p[1] && p[1] != L']') {
            ++p; if (*p == L'\\' && p[1]) ++p; hi = *p++;
        }
        if (c >= lo && c <= hi) matched = 1;
    }
    if (*p != L']') return -1;
    *pattern = p + 1;
    return negate ? !matched : matched;
}

static int glob(const WCHAR *p, const WCHAR *s) {
    const WCHAR *star = 0, *retry = 0;
    while (*s) {
        const WCHAR *next = p;
        int matched = 0;
        if (*p == L'*') { do { ++p; } while (*p == L'*'); star = p; retry = s; if (!*p) return 1; continue; }
        if (*p == L'?') { matched = 1; next = p + 1; }
        else if (*p == L'[') { matched = bracket(&next, pathchar(*s)); if (matched < 0) { matched = *s == L'['; next = p + 1; } }
        else {
            if (*next == L'\\' && next[1]) ++next;
            if (*next) { matched = *next == pathchar(*s); ++next; }
        }
        if (matched) { p = next; ++s; }
        else if (star) { p = star; s = ++retry; }
        else return 0;
    }
    while (*p == L'*') ++p;
    return !*p;
}

static int excluded(const WCHAR *s) {
    for (Pattern *p = patterns; p; p = p->next) {
        const WCHAR *part = s;
        if (glob(p->value, part)) return 1;
        while (*part) { if (pathchar(*part++) == L'/' && glob(p->value, part)) return 1; }
    }
    return 0;
}

static void add_pattern(const WCHAR *value) {
    SIZE_T n = (SIZE_T)lstrlenW(value);
    Pattern *p = allocate((SIZE_T)FIELD_OFFSET(Pattern, value) + (n + 1) * sizeof(WCHAR));
    p->next = patterns; patterns = p;
    for (SIZE_T i = 0; i <= n; ++i) p->value[i] = value[i];
}
