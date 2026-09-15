# unix-utils

Small native Windows implementations of essential Unix utilities.

## Build and test

Written in C and built with Microsoft's `cl.exe`. Requires 64-bit Windows 8 or newer and
Visual Studio or Build Tools with **Desktop development with C++** to build.

```powershell
.\build.ps1
.\build\ls.exe -lrt
.\build\ls.exe -la
.\build\ls.exe -1 C:\Windows
.\build\ls.exe -R
.\build\du.exe -sh .
.\build\du.exe -h --max-depth=1 .
.\test.ps1
.\test-du.ps1
.\bench.ps1
```

PowerShell aliases `ls` to `Get-ChildItem`; use `ls.exe` or its explicit path.
The build script discovers the x64 compiler using `vswhere`, or uses an existing
developer shell, including preview installations. Outputs are `build\ls.exe`
and `build\du.exe`; generated files are ignored by Git. No redistributable C
runtime needs to be installed to run either executable.

`test.ps1` checks ls. `test-du.ps1` checks du and, when available, compares
results against Git for Windows' GNU du 8.32. Use its `-Reference` parameter
for another GNU executable. Tests create and clean up isolated temporary
fixtures, including hard links, junctions, sparse files, and long paths.

## ls.exe

### Options

Flags can be combined; multiple file/directory operands and `--` are supported.

| Flag | Behavior |
| --- | --- |
| `-a` | Include dot-prefixed and Windows hidden entries, including `.` and `..` when Windows supplies them |
| `-A` | Include hidden entries, excluding `.` and `..` |
| `-l` | Long listing: approximate mode, placeholder link count/owner/group, byte size, local modification time, name |
| `-1` | One entry per line (also the default); cancels `-l` |
| `-r` | Reverse the sort |
| `-t` | Sort by modification time, newest first |
| `-S` | Sort by size, largest first |
| `-d` | Show directory operands themselves instead of their contents |
| `-U` | Stream entries in Windows enumeration order without sorting |
| `-R` | Recursively list subdirectories; `-d` takes precedence |
| `--help` | Show usage |

The last of `-t`, `-S`, and `-U` wins. Sorting defaults to case-sensitive ordinal
name order, with names breaking time/size ties. Each directory is sorted
independently; operands retain command-line order.

## Size and memory

The build optimizes for size with link-time optimization and dead-code folding.
Both executable import tables contain only `kernel32.dll`;
there is no statically linked library runtime or C runtime dependency. `/MD`
selects the dynamic runtime model, while `/NODEFAULTLIB` ensures no CRT is linked.
The custom entry point uses Win32 APIs directly. `/GS-` omits compiler stack
cookies to avoid a CRT dependency; ASLR and DEP remain enabled.
`du --time` with a Windows time-zone name in `TZ` loads `advapi32.dll` on demand
to look up that zone. The ordinary paths do not load it.

Output uses a 4 KiB buffer. Sorted listings allocate one variable-sized record
per visible entry in the current directory and use an in-place linked-list
merge sort (O(n log n) time, O(log n) stack). Records are freed before the next
directory. `-U` holds just one entry at a time. Recursive traversal re-enumerates
after freeing each listing, using one search handle, path storage, and stack
frame per ancestor directory. Windows loader/DLL overhead still
contributes to the process working set.

Du uses iterative depth-first traversal, one enumeration handle and a small
frame per ancestor, and reusable path buffers. It does not retain directory
entry lists. Hard-link identities are kept only where deduplication requires
them. Multiple operands and `-L` require tracking all encountered identities;
ordinary single-root traversal tracks only multiply linked files. `-l` avoids
that set while ancestor identities still prevent cycles. Exclusion patterns
are retained, but `--files0-from` operands are read incrementally.

Example release sizes with MSVC 14.51, x64:

| Executable | Bytes |
| --- | ---: |
| `ls.exe` | 7,680 |
| `du.exe` | 27,136 |

`bench.ps1` creates 10,000 empty files by default and samples each process's
working set and private memory while draining its output. On the development
machine, streaming ls and ordinary du used about **0.45 MiB private memory**
and **3.3 MiB working set**, including Windows loader/shared DLL pages. Sorted
ls used about 1.3 MiB private memory for those 10,000 names. These are machine-
and workload-dependent measurements, not fixed limits. Use `-Entries` to vary
the workload. The benchmark deliberately fills a pipe briefly so short-lived
processes can be observed; it is not a speed benchmark.

## Intentional differences from GNU ls

This is a compact subset, not full parity. Redirected output is UTF-8; console
names use native Unicode output. Listings always use one entry per
line, with no columns, color, glob expansion, or locale collation.
Quote paths containing spaces. Wildcards are not operands; supply directory
paths instead. Windows hidden attributes and dot-prefixed names are both hidden
by default. Long listings use `YYYY-MM-DD HH:MM`; permissions are approximated
from directory/read-only attributes, not ACLs. Link count is `1`, owner/group
are `-`, directory sizes are Windows-reported, and there is no block `total`.
Reparse points are not displayed as Unix symbolic links. Names are printed
in the requested sort order within each directory; recursive directory visits
follow Windows enumeration order. `-R` skips directory reparse points during
descent to avoid junction/symlink cycles (explicit directory operands are still
listed). Hidden directory descent follows `-a`/`-A`. Names are printed
literally without quoting or escaping. Long path availability follows Windows
path rules; extended `\\?\` paths can be supplied explicitly.

Exit status: `0` success, `1` filesystem/output error, `2` invalid option or
allocation failure. Errors go to stderr with a Windows error number.

## du.exe

Implements the GNU du option set using native Windows filesystem information.
The behavioral reference is the [GNU du manual](https://www.gnu.org/software/coreutils/manual/html_node/du-invocation.html).
Short options can be combined; long options accept unambiguous abbreviations,
and `--` ends option parsing. No operands means the current directory. Hidden
files are included. Output is postorder, with tab-separated sizes and paths.

| Options | Behavior |
| --- | --- |
| `-a`, `--all` | Report files as well as directories |
| `-A`, `--apparent-size` | Report logical bytes instead of allocation |
| `-b`, `--bytes` | Apparent size with one-byte output units |
| `-B SIZE`, `--block-size=SIZE` | Choose output units, rounded upward |
| `-k`, `-m` | 1,024-byte / 1,048,576-byte output units |
| `-h`, `--human-readable` | Human sizes using powers of 1,024 |
| `--si` | Human sizes using powers of 1,000 |
| `-c`, `--total` | Append a grand total |
| `-d N`, `--max-depth=N` | Limit displayed depth; still scan descendants |
| `-s`, `--summarize` | Report only each operand's total |
| `-S`, `--separate-dirs` | Exclude subdirectories from each directory's total |
| `-l`, `--count-links` | Count repeated hard-link occurrences |
| `-P`, `--no-dereference` | Measure symbolic links/junctions themselves (default) |
| `-D`, `-H`, `--dereference-args` | Follow only links supplied as operands |
| `-L`, `--dereference` | Follow links throughout traversal; prune cycles |
| `-x`, `--one-file-system` | Stay on the volume of each operand |
| `--inodes` | Count file/directory identities rather than bytes |
| `-t SIZE`, `--threshold=SIZE` | Print sizes at least SIZE; negative SIZE selects at most its magnitude |
| `--exclude=PATTERN` | Exclude matching paths and subtrees |
| `-X FILE`, `--exclude-from=FILE` | Read exclusion patterns, one per line; `-` reads stdin |
| `--files0-from=FILE` | Read NUL-separated operands incrementally; `-` reads stdin |
| `-0`, `--null` | NUL-terminate output records |
| `--time[=WORD]` | Latest modification, access (`atime`, `access`, `use`), or metadata-change (`ctime`, `status`) timestamp |
| `--time-style=STYLE` | `full-iso`, `long-iso`, `iso`, or `+FORMAT` |
| `--help`, `--version` | Usage / this implementation's version |

Size suffixes include `K`, `M`, `G`, etc. (powers of 1,024), `KB`, `MB`, etc.
(powers of 1,000), and `KiB`, `MiB`, etc. `-B M` includes the unit suffix;
`-B 1M` prints just the count. `-B human-readable` and `-B si` are also accepted.
The default unit comes from `DU_BLOCK_SIZE`, `BLOCK_SIZE`, then `BLOCKSIZE`,
otherwise 1,024 bytes (512 when `POSIXLY_CORRECT` is set). Command-line units
override the environment. A leading apostrophe in SIZE requests locale grouping.
`--inodes` ignores byte scaling/apparent-size options but supports human counts.

Exclusions support `*`, `?`, bracket ranges, negated ranges, and character
classes such as `[[:digit:]]`. Matching is case-sensitive and unanchored; use
forward slashes for pattern separators and backslashes to escape metacharacters.
Patterns and NUL-separated file lists are UTF-8. Pattern files accept LF or CRLF.
`--files0-from` cannot be combined with command-line operands.

### Windows semantics and limits

- Allocation comes from `FileStandardInfo`, with `FileCompressionInfo` for
  compressed/sparse files. Apparent sizes use logical lengths, including directory
  lengths where Windows reports them. Physical symbolic links/junctions use the
  UTF-8 target-name length in apparent-size mode. Filesystem metadata, resident
  data, allocation units, delayed allocation, and directory sizes differ from
  Linux and from MSYS's emulated `stat` values. Those byte totals are not expected
  to match across filesystems. This does not estimate storage-device compression,
  snapshots, or deduplicated physical media.
- Windows volume serial numbers and 128-bit file IDs identify repeated objects;
  filesystems without that API use Windows' 64-bit file-index information.
  `--inodes` counts objects; Windows has no Unix inode-capacity accounting.
- Volume mount points are traversed as directories. Junctions/symbolic links
  obey `-P`/`-D`/`-L`. `-x` compares volume identities, including when links are
  followed. Filesystems and remote servers must expose the required metadata;
  query failures are reported instead of fabricated values.
- Native Windows paths, UNC paths, and extended paths are accepted. Du converts
  ordinary paths to extended absolute paths internally for traversal beyond 260
  characters. Output preserves operand spelling and uses `/` for child separators.
  There is no shell glob expansion. File lists/output use UTF-8; console paths
  use Unicode. Paths are emitted literally, so use `-0` for machine parsing.
- Timestamps use Windows modification/access/change times, not creation time
  as a substitute for `ctime`. `full-iso` prints nine fractional digits, with
  Windows' 100 ns resolution. `+FORMAT` supports calendar/ISO-week fields,
  names, composite date/time formats, padding/case flags, `%s`, `%N`, and `%z`
  colon variants. Locale names/formats come from Windows, via `LC_ALL`,
  `LC_TIME`/`LC_NUMERIC`, or `LANG`; GNU alternative-era/digit locale tables are
  unavailable and `E`/`O` modifiers use the Windows standard representation.
  `TIME_STYLE` supplies the default format. The system time zone is the default;
  `TZ` accepts UTC/GMT (`UTC0`/`GMT0`) or a Windows time-zone key. GNU zoneinfo
  paths and POSIX TZ rule strings have no native Windows counterpart and are
  rejected rather than interpreted incorrectly. `%Z` uses Windows zone names.
- Linux-only filesystem objects, `/proc` behavior, and Unix permission semantics
  are not emulated. Alternate NTFS data streams are not enumerated separately.

Du returns `0` on success and `1` for invalid options, filesystem/input/output
errors, or allocation failure. Diagnostics go to stderr; later operands are
still processed after filesystem errors.
