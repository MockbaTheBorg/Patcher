# Binary Patcher v2.01

**by Mockba the Borg**

A powerful, pattern-based binary patcher driven by a simple scripting language.
Search for byte patterns (with wildcard support), verify matches, and apply
precise binary modifications — all from a human-readable script file.

---

## Building

```bash
make            # build the 'patch' executable
make clean      # remove the executable
```

Requirements: a C++11-capable compiler (GCC, Clang, MSVC).  No external
libraries are needed.

---

## Usage

```
patch <script file>|-i <binary file> [<output file>] [-v] [-n]
```

| Argument | Description |
|---|---|
| `<script file>` | Path to the patch script (see format below). |
| `-i` | Read the script from **stdin** instead of a file. |
| `<binary file>` | The binary file to patch. |
| `<output file>` | *(optional)* Write the patched result to a new file instead of modifying in-place. |
| `-v` | *(optional)* Enable verbose/debug output. Can appear anywhere after the binary file argument. |
| `-n` | *(optional)* Dry-run: perform all checks and script actions but do not write an output file. Overrides `<output file>` when present. |

### Examples

```bash
# Patch a binary in-place using a script
./patch mypatch.txt firmware.bin

# Patch to a new file with verbose output
./patch mypatch.txt firmware.bin firmware_patched.bin -v

# Pipe the script from stdin
cat mypatch.txt | ./patch -i firmware.bin
```

---

## Script Format

A patch script is a plain-text file.  Rules:

1. The **first line** must begin with `# patch` followed by a version string
   (e.g. `# patch v1.00`).  This acts as a magic header.
2. Blank lines and lines starting with `#` are ignored (comments).
3. All other lines are **commands** (case-insensitive).

### Hex Byte Strings

Several commands accept a quoted hex-byte string:

```
"48 65 6C 6C 6F"
```

- Each byte is written as two hex digits, separated by spaces.
- `??` is a **wildcard** that matches any single byte (supported in `search`
  and `assert`).
- `??` is **not** valid in `patch` or `fill` — only concrete byte values are
  allowed where data is written.

---

## Command Reference

### Output Commands

#### `print "text"`

Print text to stdout **without** a trailing newline.

The text is enclosed in double quotes and supports these escape sequences:

| Escape | Meaning |
|--------|---------|
| `\n` | Newline |
| `\r` | Carriage return |
| `\t` | Tab |
| `\\` | Literal backslash |
| `\"` | Literal double quote |
| `\c` | Number of results from the last search (prints -1 if no search has been performed) |
| `\s` | Offset of the currently selected search result (decimal + hex) |
| `\p` | Current data pointer position (decimal + hex) |

**Special argument tokens** (without quotes):

| Token | Meaning |
|-------|---------|
| `$string` | Print the NUL-terminated string starting at the current pointer. |
| `$int` | Print the 32-bit signed integer at the pointer. |
| `$float` | Print the IEEE 754 float at the pointer. |
| `$hex32` | Print the 32-bit value as 8-digit uppercase hex. |
| `$hex16` | Print the 16-bit value as 4-digit uppercase hex (0xFFFF). |
| `$hex8` | Print the 8-bit value as 2-digit uppercase hex (0xFF). |

#### `println "text"`

Same as `print` but appends a newline at the end.

---

### Search Commands

#### `search "XX XX ?? XX ..." ["message"]`

Search the **entire** binary for occurrences of the hex byte pattern.
`??` bytes are treated as single-byte wildcards.

- Populates the internal result list (up to 256 matches).
- **Aborts** the script if zero results are found.
- If exactly one result is found, it is automatically selected and the data
  pointer is moved there.

The optional second quoted string overrides the default "pattern not found"
error message on failure:

```
search "4D 5A" "MZ header not found — is this a valid PE file?"
```

#### `searchnext "XX XX ?? XX ..." ["message"]`

Like `search`, but begins scanning **after the current data pointer** position.
Useful for iterating through multiple occurrences.  Accepts the same optional
custom error message.

#### `searchall "XX XX ?? XX ..."`

Like `search`, but does **not** abort when zero results are found.  The result
count will simply be 0.

#### `verify unique|<n> ["message"]`

Abort the script if the last search did **not** return exactly the specified
number of results. The first argument may be the word `unique` (equivalent to
`1`) or a positive integer `n`. The optional quoted string overrides the
default error message.

Examples:

```
verify unique "Expected exactly one match for the boot signature"
verify 3 "Expected exactly three matches for the table"
```

#### `count`

Print the number of results from the last search.

#### `match <n>`

Print the offset of the *n*-th search result (1-based) in decimal and hex.

---

### Pointer / Navigation Commands

#### `pointer`

Print the current data pointer position in decimal and hex.

#### `position <n>`

Set the data pointer to an absolute offset.  Accepts decimal, hex (`0x` prefix),
or octal (`0` prefix).

```
position 0x1A3F
position 1024
```

#### `skip <n>`

Move the data pointer forward (or backward) by *n* bytes.  The value is a
signed decimal integer.

```
skip 16      # move forward 16 bytes
skip -4      # move backward 4 bytes
```

#### `align <n>`

Align the pointer forward to the next *n*-byte boundary.  If already aligned,
the pointer does not move.

```
align 16     # advance to next 16-byte boundary
```

#### `select <n>`

Select the *n*-th search result (1-based) and move the data pointer to that
offset.  Required when a search returns more than one match.

---

### Read Commands

#### `read8`

Print the unsigned byte at the current pointer (decimal + hex).

#### `read16`

Print the unsigned 16-bit little-endian value at the current pointer.

#### `read32`

Print the unsigned 32-bit little-endian value at the current pointer.

---

### Write / Modify Commands

#### `patch "XX XX XX ..."`

Overwrite bytes at the current data pointer with the given hex byte sequence.
The number of bytes to write is determined by the length of the sequence.

```
patch "90 90 90"      # write three NOP bytes
```

#### `fill <count> <XX>`

Fill *count* bytes starting at the current pointer with the byte value `XX`
(hex).

```
fill 64 00            # zero out 64 bytes
fill 8 CC             # fill 8 bytes with 0xCC
```

---

### Verification Commands

#### `assert "XX XX ?? XX ..." ["message"]`

Verify that the bytes at the current pointer match the given hex pattern.
Wildcards (`??`) are supported.  If the assertion fails, the script aborts and
the expected vs actual bytes are printed to stderr.

The optional second quoted string overrides the default error message on
failure:

```
assert "4D 5A"                         # verify MZ header (default error)
assert "4D 5A" "Not a valid PE file"   # verify with custom error
```

#### `dump <n>`

Dump *n* bytes at the current pointer in a classic hex+ASCII format:

```
  00001000: 48 65 6C 6C 6F 57 6F 72 6C 64 00 01 02 03 04 05  |HelloWorld......|
```

---

### Control Commands

#### `debug <on|off>`

Toggle verbose/debug output at runtime.  Equivalent to the `-v` flag but can
be turned on and off within the script.

#### `quit [<n>]`

Stop script execution immediately.

If the optional argument `<n>` is given, the script only quits when the last
search returned **more than** *n* results.  Otherwise execution continues past
the `quit` line.

```
quit            # unconditional stop
quit 1          # stop only if last search found more than 1 match
```

---

## Example Script

```
# patch v1.00
# Example: find a signature and patch it

println "Binary Patcher Example Script"

# Search for the target signature (with wildcard byte)
search "4D 5A ?? ?? 00 00"
verify unique
println "Found MZ header at \s"

# Verify the bytes we expect before patching
skip 2
assert "90 00"
println "Pre-patch assertion OK"

# Apply the patch
patch "FF FF"
println "Patch applied!"

# Verify the patch took effect
skip -2
assert "FF FF"
println "Post-patch assertion OK"

# Show the result
position 0
dump 16

println "Done."
quit
```

---

## Changelog

### v2.01
- **Optional custom error messages** — `search`, `searchnext`, `verify unique`,
  and `assert` now accept an optional second quoted string that replaces the
  default error message when the command fails.

### v2.00
- **Removed PCRE dependency** — search uses a built-in byte-pattern matcher.
- **Fixed wildcard vs 0x00 ambiguity** — search now uses a separate mask
  array, so `0x00` bytes can be matched literally alongside `??` wildcards.
- **Fixed `position` command** — now accepts hex (`0x`-prefixed) and decimal
  values via `strtol` (previously only decimal via `atoi`).
- **Fixed `quit <n>` logic** — now correctly quits only when the search count
  exceeds *n* (previously compared for equality).
- **Bounds checking** — all read/write/dump operations validate the data
  pointer and length against the file size before accessing memory.
- **Proper error output** — errors and warnings go to stderr.
- **Memory cleanup** — `free()` is called on all exit paths.
- **Return value checking** — `fgets`/`fread`/`fwrite` return values are
  validated.
- **Flexible `-v` flag** — can appear anywhere after the binary file argument.
- **Removed stray debug print** (`std::cout << "ok"`).
- **Increased limits** — `MAXSTR` 256→1024, `MAXPATTERN` 512,
  `MAXSEARCH` 32→256.
- New commands: `searchnext`, `searchall`, `assert`, `dump`, `fill`, `align`,
  `read8`, `read16`, `read32`.

### v1.02
- Initial release with PCRE-based search.

---

## License

This software is provided as-is.  Use at your own risk.
