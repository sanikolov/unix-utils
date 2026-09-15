/* Formatting without a CRT or floating point. Human sizes round upward. */
static WCHAR numeric_locale[LOCALE_NAME_MAX_LENGTH], numeric_decimal[8] = L".", numeric_thousands[8];
static int numeric_group;
static UINT numeric_group_size = 3;
static void prepare_numeric(void) {
    WCHAR *locale = environment(L"LC_ALL");
    int i = 0;
    if (!locale) locale = environment(L"LC_NUMERIC");
    if (!locale) locale = environment(L"LANG");
    if (!locale) return;
    if (!equal(locale, L"C") && !equal(locale, L"POSIX") && !equal(locale, L"C.UTF-8")) {
        WCHAR groups[16];
        while (locale[i] && locale[i] != L'.' && locale[i] != L'@' && i < LOCALE_NAME_MAX_LENGTH - 1) {
            numeric_locale[i] = locale[i] == L'_' ? L'-' : locale[i]; ++i;
        }
        numeric_locale[i] = 0;
        if (IsValidLocaleName(numeric_locale)) {
            GetLocaleInfoEx(numeric_locale, LOCALE_SDECIMAL, numeric_decimal, 8);
            GetLocaleInfoEx(numeric_locale, LOCALE_STHOUSAND, numeric_thousands, 8);
            if (GetLocaleInfoEx(numeric_locale, LOCALE_SGROUPING, groups, 16)) {
                numeric_group_size = 0;
                for (int j = 0; groups[j]; ++j) if (groups[j] >= L'1' && groups[j] <= L'9') numeric_group_size = numeric_group_size * 10 + groups[j] - L'0';
            }
            numeric_group = 1;
        }
    }
    release(locale);
}

static void print_size(ULONGLONG n) {
    if (n == ~(ULONGLONG)0) { text("Infinity"); return; }
    if (human) {
        const char *units = human == 1000 ? " kMGTPEZYRQ" : " KMGTPEZYRQ";
        ULONGLONG factor = 1, q, remainder, tenths;
        int unit = 0;
        while (n / factor >= (unsigned)human && factor <= ~(ULONGLONG)0 / (unsigned)human) { factor *= (unsigned)human; ++unit; }
        q = n / factor; remainder = n % factor;
        if (unit && q < 10) {
            tenths = remainder * 10 / factor + (remainder * 10 % factor != 0);
            if (tenths == 10) { ++q; tenths = 0; }
            number(q, 0);
            if (q < 10) { wide(numeric_decimal); number(tenths, 0); }
        } else {
            q += remainder != 0;
            if (unit && q == (unsigned)human) { ++unit; put('1'); wide(numeric_decimal); put('0'); }
            else number(q, 0);
        }
        if (unit) put(units[unit]);
    } else {
        ULONGLONG value = n / block + (n % block != 0);
        if (grouping && numeric_group) {
            WCHAR raw[21], grouped[96];
            int length = 0;
            ULONGLONG temp = value;
            do { raw[length++] = (WCHAR)(L'0' + temp % 10); temp /= 10; } while (temp);
            for (int i = 0; i < length / 2; ++i) { WCHAR c = raw[i]; raw[i] = raw[length - i - 1]; raw[length - i - 1] = c; }
            raw[length] = 0;
            {
                NUMBERFMTW format;
                format.NumDigits = 0; format.LeadingZero = 1; format.Grouping = numeric_group_size;
                format.lpDecimalSep = numeric_decimal; format.lpThousandSep = numeric_thousands; format.NegativeOrder = 1;
                if (GetNumberFormatEx(numeric_locale, 0, raw, &format, grouped, 96)) wide(grouped);
                else number(value, 0);
            }
        } else number(value, 0);
        if (suffix_output) wide(suffix);
    }
}

typedef struct {
    SYSTEMTIME local;
    ULONGLONG ticks;
    int offset, yearday, isoyear, isoweek;
} Date;
static DYNAMIC_TIME_ZONE_INFORMATION timezone;
static WCHAR locale_name[LOCALE_NAME_MAX_LENGTH];
static int c_locale = 1;
static int utc_zone;
static const WCHAR *date_format_string;

static void prepare_time(void) {
    WCHAR *locale = environment(L"LC_ALL"), *zone;
    const WCHAR *style = time_style;
    if (!locale) locale = environment(L"LC_TIME");
    if (!locale) locale = environment(L"LANG");
    if (locale && !equal(locale, L"C") && !equal(locale, L"POSIX") && !equal(locale, L"C.UTF-8")) {
        int i = 0;
        while (locale[i] && locale[i] != L'.' && locale[i] != L'@' && i < LOCALE_NAME_MAX_LENGTH - 1) {
            locale_name[i] = locale[i] == L'_' ? L'-' : locale[i]; ++i;
        }
        locale_name[i] = 0;
        c_locale = !IsValidLocaleName(locale_name);
    }
    release(locale);
    GetDynamicTimeZoneInformation(&timezone);
    zone = environment(L"TZ");
    if (zone) {
        if (equal(zone, L"UTC") || equal(zone, L"UTC0") || equal(zone, L"GMT") || equal(zone, L"GMT0")) {
            utc_zone = 1;
            timezone.Bias = timezone.StandardBias = timezone.DaylightBias = 0;
            timezone.StandardDate.wMonth = timezone.DaylightDate.wMonth = 0;
            timezone.DynamicDaylightTimeDisabled = TRUE;
            timezone.TimeZoneKeyName[0] = 0;
            timezone.StandardName[0] = L'U'; timezone.StandardName[1] = L'T'; timezone.StandardName[2] = L'C'; timezone.StandardName[3] = 0;
        } else {
            DYNAMIC_TIME_ZONE_INFORMATION candidate;
            HMODULE library = LoadLibraryExW(L"advapi32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
            union { FARPROC raw; DWORD (WINAPI *enumerate)(DWORD, PDYNAMIC_TIME_ZONE_INFORMATION); } function;
            DWORD i = 0;
            int matched = 0;
            function.raw = library ? GetProcAddress(library, "EnumDynamicTimeZoneInformation") : 0;
            while (function.raw && function.enumerate(i++, &candidate) == ERROR_SUCCESS) {
                if (equal(zone, candidate.TimeZoneKeyName)) { timezone = candidate; matched = 1; break; }
            }
            if (library) FreeLibrary(library);
            if (!matched) invalid(zone);
        }
        release(zone);
    }
    if (!style || (time_style_environment && equal(style, L"locale"))) style = L"long-iso";
    if (time_style_environment) while (style[0] == L'p' && style[1] == L'o' && style[2] == L's' && style[3] == L'i' && style[4] == L'x' && style[5] == L'-') style += 6;
    if (prefix(style, L"full-iso")) date_format_string = L"%Y-%m-%d %H:%M:%S.%N %z";
    else if (prefix(style, L"long-iso")) date_format_string = L"%Y-%m-%d %H:%M";
    else if (prefix(style, L"iso")) date_format_string = L"%Y-%m-%d";
    else if (*style == L'+') {
        WCHAR *s = (WCHAR *)style + 1;
        if (time_style_environment) for (SIZE_T i = 0; s[i]; ++i) if (s[i] == L'\n') { s[i] = 0; break; }
        date_format_string = s;
    } else invalid(style);
}

static int leap(int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
static int iso_weeks(int year, int jan1) { return jan1 == 4 || (jan1 == 3 && leap(year)) ? 53 : 52; }

static int decimal(WCHAR *out, LONGLONG value) {
    WCHAR reversed[21];
    ULONGLONG n = value < 0 ? (ULONGLONG)(-(value + 1)) + 1 : (ULONGLONG)value;
    int length = 0, written = 0;
    do { reversed[length++] = (WCHAR)(L'0' + n % 10); n /= 10; } while (n);
    if (value < 0) out[written++] = L'-';
    while (length) out[written++] = reversed[--length];
    out[written] = 0; return written;
}

static void literal(WCHAR *out, const WCHAR *s) {
    int i = 0;
    while (s[i] && i < 255) { out[i] = s[i]; ++i; }
    out[i] = 0;
}

static void format_date(WCHAR *out, SIZE_T capacity, const WCHAR *format, const Date *date) {
    static const WCHAR *days[] = { L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday" };
    static const WCHAR *months[] = { L"January", L"February", L"March", L"April", L"May", L"June", L"July", L"August", L"September", L"October", L"November", L"December" };
    const SYSTEMTIME *s = &date->local;
    SIZE_T used_chars = 0;
    while (*format && used_chars + 1 < capacity) {
        WCHAR piece[256], padding = 0, code;
        const WCHAR *start, *composite = 0;
        int width = 0, default_width = 0, no_padding = 0, upper = 0, swap = 0, colons = 0, plus = 0;
        LONGLONG value = 0;
        int numeric = 1, length;
        if (*format != L'%') { out[used_chars++] = *format++; continue; }
        start = format++;
        for (;;) {
            if (*format == L'-') no_padding = 1;
            else if (*format == L'_') padding = L' ';
            else if (*format == L'0') padding = L'0';
            else if (*format == L'^') upper = 1;
            else if (*format == L'#') swap = 1;
            else if (*format == L'+') plus = 1;
            else break;
            ++format;
        }
        while (*format >= L'0' && *format <= L'9') { if (width < 1024) width = width * 10 + *format - L'0'; ++format; }
        if (width > 1024) width = 1024;
        if (*format == L'E' || *format == L'O') ++format;
        while (*format == L':') { ++colons; ++format; }
        code = *format;
        if (code) ++format;
        piece[0] = 0;
        switch (code) {
            case L'Y': value = s->wYear; default_width = 4; break;
            case L'y': value = s->wYear % 100; default_width = 2; break;
            case L'C': value = s->wYear / 100; default_width = 2; break;
            case L'm': value = s->wMonth; default_width = 2; break;
            case L'd': case L'e': value = s->wDay; default_width = 2; if (code == L'e' && !padding) padding = L' '; break;
            case L'H': case L'k': value = s->wHour; default_width = 2; if (code == L'k' && !padding) padding = L' '; break;
            case L'I': case L'l': value = s->wHour % 12; if (!value) value = 12; default_width = 2; if (code == L'l' && !padding) padding = L' '; break;
            case L'M': value = s->wMinute; default_width = 2; break;
            case L'S': value = s->wSecond; default_width = 2; break;
            case L'j': value = date->yearday + 1; default_width = 3; break;
            case L'w': value = s->wDayOfWeek; break;
            case L'u': value = s->wDayOfWeek ? s->wDayOfWeek : 7; break;
            case L'U': value = (date->yearday + 7 - s->wDayOfWeek) / 7; default_width = 2; break;
            case L'W': value = (date->yearday + 7 - (s->wDayOfWeek + 6) % 7) / 7; default_width = 2; break;
            case L'V': value = date->isoweek; default_width = 2; break;
            case L'G': value = date->isoyear; default_width = 4; break;
            case L'g': value = date->isoyear % 100; default_width = 2; break;
            case L'q': value = (s->wMonth + 2) / 3; break;
            case L'N': value = (LONGLONG)(date->ticks % 10000000) * 100; default_width = 9; break;
            case L's': value = ((LONGLONG)date->ticks - 116444736000000000LL) / 10000000; break;
            case L'a': case L'A':
                numeric = 0;
                if (c_locale || !GetDateFormatEx(locale_name, 0, s, code == L'a' ? L"ddd" : L"dddd", piece, 256, 0)) {
                    literal(piece, days[s->wDayOfWeek]); if (code == L'a') piece[3] = 0;
                }
                break;
            case L'b': case L'B': case L'h':
                numeric = 0;
                if (c_locale || !GetDateFormatEx(locale_name, 0, s, code == L'B' ? L"MMMM" : L"MMM", piece, 256, 0)) {
                    literal(piece, months[s->wMonth - 1]); if (code != L'B') piece[3] = 0;
                }
                break;
            case L'p': case L'P':
                numeric = 0;
                if (c_locale || !GetTimeFormatEx(locale_name, 0, s, L"tt", piece, 256)) literal(piece, s->wHour < 12 ? L"AM" : L"PM");
                if (code == L'P') LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, piece, lstrlenW(piece), piece, 256, 0, 0, 0);
                break;
            case L'D': composite = L"%m/%d/%y"; break;
            case L'F': composite = plus ? L"%+Y-%m-%d" : L"%Y-%m-%d"; break;
            case L'R': composite = L"%H:%M"; break;
            case L'T': composite = L"%H:%M:%S"; break;
            case L'r': composite = L"%I:%M:%S %p"; break;
            case L'x':
                if (c_locale) composite = L"%m/%d/%y";
                else { numeric = 0; GetDateFormatEx(locale_name, DATE_SHORTDATE, s, 0, piece, 256, 0); }
                break;
            case L'X':
                if (c_locale) composite = L"%H:%M:%S";
                else { numeric = 0; GetTimeFormatEx(locale_name, 0, s, 0, piece, 256); }
                break;
            case L'c': composite = c_locale ? L"%a %b %e %H:%M:%S %Y" : L"%x %X"; break;
            case L'z': {
                int offset = date->offset, i = 0;
                numeric = 0;
                piece[i++] = offset < 0 ? L'-' : L'+'; if (offset < 0) offset = -offset;
                piece[i++] = (WCHAR)(L'0' + offset / 60 / 10); piece[i++] = (WCHAR)(L'0' + offset / 60 % 10);
                if (colons != 3 || offset % 60) {
                    if (colons) piece[i++] = L':';
                    piece[i++] = (WCHAR)(L'0' + offset % 60 / 10); piece[i++] = (WCHAR)(L'0' + offset % 10);
                    if (colons == 2) { piece[i++] = L':'; piece[i++] = L'0'; piece[i++] = L'0'; }
                }
                piece[i] = 0; break;
            }
            case L'Z': {
                TIME_ZONE_INFORMATION yearly;
                const WCHAR *name = timezone.StandardName;
                numeric = 0;
                if (GetTimeZoneInformationForYear(s->wYear, &timezone, &yearly) &&
                    yearly.DaylightDate.wMonth && date->offset == -(yearly.Bias + yearly.DaylightBias)) name = yearly.DaylightName;
                literal(piece, name); break;
            }
            case L'n': numeric = 0; literal(piece, L"\n"); break;
            case L't': numeric = 0; literal(piece, L"\t"); break;
            case L'%': numeric = 0; literal(piece, L"%"); break;
            default:
                numeric = 0;
                { int i = 0; while (start < format && i < 255) piece[i++] = *start++; piece[i] = 0; }
                break;
        }
        if (composite) { numeric = 0; format_date(piece, 256, composite, date); }
        if (numeric && code == L'N') {
            int precision = width ? width : 9;
            ULONGLONG fraction = date->ticks % 10000000 * 100;
            if (precision > 255) precision = 255;
            for (int i = 8; i >= 0; --i) { piece[i] = (WCHAR)(L'0' + fraction % 10); fraction /= 10; }
            for (int i = 9; i < precision; ++i) piece[i] = L'0';
            piece[precision] = 0; numeric = 0; width = 0;
        } else if (numeric) decimal(piece, value);
        length = lstrlenW(piece);
        if (numeric && plus && (code == L'Y' || code == L'G') && (value > 9999 || width > default_width)) {
            for (int i = length; i >= 0; --i) piece[i + 1] = piece[i];
            piece[0] = L'+'; ++length;
        }
        if (upper || swap) {
            LCMapStringEx(LOCALE_NAME_INVARIANT, swap && (code == L'p' || code == L'Z') ? LCMAP_LOWERCASE : LCMAP_UPPERCASE,
                          piece, length, piece, 256, 0, 0, 0);
        }
        if (!width) width = numeric ? default_width : 0;
        if (!padding) padding = L'0';
        if (no_padding) width = 0;
        {
            int offset = 0;
            if (numeric && padding == L'0' && (piece[0] == L'-' || piece[0] == L'+') && used_chars + 1 < capacity) {
                out[used_chars++] = piece[0]; offset = 1;
            }
            while (width-- > length && used_chars + 1 < capacity) out[used_chars++] = numeric ? padding : L' ';
            for (int i = offset; i < length && used_chars + 1 < capacity; ++i) out[used_chars++] = piece[i];
        }
    }
    out[used_chars] = 0;
}

static void print_time(ULONGLONG ticks) {
    static const int before_month[] = { 0,31,59,90,120,151,181,212,243,273,304,334 };
    FILETIME ft, localft;
    SYSTEMTIME utc;
    Date date;
    WCHAR *formatted;
    int jan1, weekday;
    ft.dwLowDateTime = (DWORD)ticks; ft.dwHighDateTime = (DWORD)(ticks >> 32);
    if (!FileTimeToSystemTime(&ft, &utc)) { put('?'); return; }
    if (utc_zone) date.local = utc;
    else if (!SystemTimeToTzSpecificLocalTimeEx(&timezone, &utc, &date.local)) { put('?'); return; }
    SystemTimeToFileTime(&date.local, &localft);
    date.ticks = ticks;
    date.offset = (int)(((LONGLONG)(((ULONGLONG)localft.dwHighDateTime << 32) | localft.dwLowDateTime) - (LONGLONG)(ticks - ticks % 10000)) / 600000000);
    date.yearday = before_month[date.local.wMonth - 1] + date.local.wDay - 1 + (date.local.wMonth > 2 && leap(date.local.wYear));
    weekday = date.local.wDayOfWeek ? date.local.wDayOfWeek : 7;
    jan1 = ((int)date.local.wDayOfWeek - date.yearday % 7 + 7) % 7;
    date.isoweek = (date.yearday + 1 - weekday + 10) / 7; date.isoyear = date.local.wYear;
    if (!date.isoweek) { --date.isoyear; date.isoweek = iso_weeks(date.isoyear, (jan1 - 1 - leap(date.isoyear) + 7) % 7); }
    else if (date.isoweek > iso_weeks(date.isoyear, jan1)) { ++date.isoyear; date.isoweek = 1; }
    formatted = allocate(2048 * sizeof(WCHAR));
    format_date(formatted, 2048, date_format_string, &date); wide(formatted); release(formatted);
}
