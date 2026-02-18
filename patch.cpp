// Binary Patcher - by Mockba the Borg
// A pattern-based binary patcher driven by a simple scripting language.
//
// Usage: patch <script file>|-i <binary file> [<output file>] [-v]
//        -v : verbose mode (can appear anywhere after the binary file argument)
//        -i : read the script from stdin instead of a file
// If no output file is specified, the binary file will be patched in-place.
//
// The script is a text file with the following format:
// The first line MUST be '# patch vX.XX' where X.XX is the script version.
// Subsequent lines are blank lines, comment lines (starting with '#'), or
// patch command lines.
//
// ---- Available commands ----
//
// print "text"        Print text to stdout (no trailing newline).
//                     Escape sequences: \r \n \t \\ \" \c \s \p
//                       \c = search result count (-1 if no search has happened yet)
//                       \s = selected search result offset (dec + hex)
//                       \p = current data pointer (dec + hex)
//                     Special argument tokens:
//                       $string  - print NUL-terminated string at pointer
//                       $int     - print 32-bit signed int at pointer
//                       $float   - print float at pointer
//                       $hex8    - print 8-bit value as 2-digit uppercase 0x hex at pointer
//                       $hex16   - print 16-bit value as 4-digit uppercase 0x hex at pointer
//                       $hex32   - print 32-bit value as 8-digit uppercase 0x hex at pointer
//
// println "text"      Same as print but appends a newline.
//
// search "XX XX ..." ["message"]
//                     Search for hex byte pattern. XX = hex byte, ?? = wildcard.
//                     Populates the result list. Aborts if nothing found.
//                     Auto-selects when exactly one result is found.
//                     An optional second quoted string overrides the default
//                     "not found" error message.
//
// searchnext "XX XX ..." ["message"]
//                     Like search, but starts after the current pointer.
//
// searchall "XX XX ..." Like search, but does NOT abort when zero results found.
//
// verify unique|<n> ["message"]
//                     Abort if the last search did not return exactly the
//                     specified number of results. The first argument may be
//                     the word `unique` (equivalent to 1) or a positive
//                     integer. An optional quoted string overrides the
//                     default error message.
//
// count               Print the number of results from the last search.
//
// match <n>           Print the n-th search result offset (1-based).
//
// pointer             Print the current data pointer position (dec + hex).
//
// position <n>        Set the data pointer. Accepts decimal or hex (0x prefix).
//
// skip <n>            Move the data pointer by n bytes (signed decimal).
//
// align <n>           Align pointer forward to the next n-byte boundary.
//
// select <n>          Select the n-th search result (1-based) and set the pointer.
//
// patch "XX XX ..."   Overwrite bytes at the current pointer with the hex sequence.
//
// assert "XX XX ..." ["message"]
//                     Verify that bytes at the pointer match the hex sequence.
//                     Aborts the script on mismatch.  An optional second
//                     quoted string overrides the default error message.
//
// dump <n>            Dump n bytes starting at the current pointer in hex.
//
// fill <n> <XX>       Fill n bytes starting at the pointer with byte XX.
//
// read8               Print the unsigned byte at the pointer.
// read16              Print the unsigned 16-bit LE value at the pointer.
// read32              Print the unsigned 32-bit LE value at the pointer.
//
// debug <on|off>      Toggle verbose/debug output.
//
// quit [<n>]          Stop script execution.
//                     If <n> is given, only quit when the last search returned
//                     more than <n> results.
//

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cstdint>

namespace mockba {
namespace patcher {

#define VERSION "2.01"
#define MAXSTR 1024
#define MAXPATTERN 512
#define MAXSEARCH 256

    // ---- Script parsing buffers ----
    char scriptLine[MAXSTR];
    char command[MAXSTR];
    char argument[MAXSTR];

    // ---- Search pattern (with separate mask for true wildcard support) ----
    unsigned char searchPattern[MAXPATTERN];
    bool          searchMask[MAXPATTERN];   // true = must match, false = wildcard
    int           searchPatternLen = 0;

    // ---- Patch buffer ----
    unsigned char patchString[MAXPATTERN];
    int           patchStringLen = 0;

    // ---- Binary data ----
    unsigned char* binaryData  = nullptr;
    long binaryFileSize        = 0;
    long binaryDataPtr         = 0;

    // ---- Search results ----
    long searchResults[MAXSEARCH];
    int  searchSelected = 0;
    int  searchCount    = 0;
    bool searchPerformed = false; // true after any search() / searchnext() / searchall()

    // ---- Flags ----
    int  lineNo       = 0;
    int  isVerbose    = 0;
    int  hasOutputFile = 0;
    int  noWrite      = 0; // -n: do not write output (dry-run)

    // ----------------------------------------------------------------
    // Convert "XX XX ?? XX ..." hex-string to binary pattern + mask.
    // Returns the pattern length, or 0 on error.
    // ----------------------------------------------------------------
    int str2pattern(const char* input, unsigned char* pattern, bool* mask)
    {
        int inputLen = (int)strlen(input);
        if (inputLen < 2) return 0;

        int out = 0;
        char hex[3] = {0, 0, 0};

        for (int i = 0; i < inputLen; ) {
            if (input[i] == ' ' || input[i] == '\t') { i++; continue; }
            if (i + 1 >= inputLen) return 0;             // incomplete byte
            if (out >= MAXPATTERN) return 0;             // too long

            hex[0] = input[i];
            hex[1] = input[i + 1];

            if (hex[0] == '?' && hex[1] == '?') {
                pattern[out] = 0x00;
                mask[out]    = false;                    // wildcard
            } else {
                if (!isxdigit(hex[0]) || !isxdigit(hex[1])) return 0;
                pattern[out] = (unsigned char)strtol(hex, nullptr, 16);
                mask[out]    = true;                     // must match
            }
            out++;
            i += 2;
        }
        return out;
    }

    // ----------------------------------------------------------------
    // Convert "XX XX XX ..." hex-string to raw bytes (no wildcards).
    // Returns byte count, or 0 on error.
    // ----------------------------------------------------------------
    int str2bin(const char* input, unsigned char* output)
    {
        int inputLen = (int)strlen(input);
        if (inputLen < 2) return 0;

        int out = 0;
        char hex[3] = {0, 0, 0};

        for (int i = 0; i < inputLen; ) {
            if (input[i] == ' ' || input[i] == '\t') { i++; continue; }
            if (i + 1 >= inputLen) return 0;
            if (out >= MAXPATTERN) return 0;

            hex[0] = input[i];
            hex[1] = input[i + 1];
            if (!isxdigit(hex[0]) || !isxdigit(hex[1])) return 0;

            output[out++] = (unsigned char)strtol(hex, nullptr, 16);
            i += 2;
        }
        return out;
    }

    // Print n bytes in hex
    void printbin(const unsigned char* data, int len)
    {
        for (int i = 0; i < len; i++)
            printf("%02X ", data[i]);
        printf("\n");
    }

    // Trim leading + trailing whitespace (in-place safe)
    int trim(char* input, char* output)
    {
        int len = (int)strlen(input);
        int start = 0, end = len - 1;
        while (start < len && isspace((unsigned char)input[start])) start++;
        while (end >= start && isspace((unsigned char)input[end])) end--;

        int n = 0;
        for (int i = start; i <= end; i++) output[n++] = input[i];
        output[n] = '\0';
        return n;
    }

    // Remove all double-quote characters from a string
    int removeQuotes(const char* input, char* output)
    {
        int n = 0;
        for (int i = 0; input[i]; i++) {
            if (input[i] != '"') output[n++] = input[i];
        }
        output[n] = '\0';
        return n;
    }

    // Convert string to lowercase (in-place safe)
    int strToLower(const char* input, char* output)
    {
        int n = 0;
        for (int i = 0; input[i]; i++) output[n++] = (char)tolower((unsigned char)input[i]);
        output[n] = '\0';
        return n;
    }

    // ----------------------------------------------------------------
    // Print a quoted string with escape-sequence processing.
    // Strips the outer quotes and interprets \n \r \t \\ \" \c \s \p.
    // ----------------------------------------------------------------
    void printStr(const char* str)
    {
        int len = (int)strlen(str);
        if (len < 2) return;
        for (int i = 1; i < len - 1; i++) {
            if (str[i] == '\\' && i + 1 < len - 1) {
                switch (str[i + 1]) {
                case 'n':  printf("\n"); break;
                case 'r':  printf("\r"); break;
                case 't':  printf("\t"); break;
                case '\\': printf("\\"); break;
                case '"':  printf("\""); break;
                case 'c':  if (!searchPerformed) printf("-1"); else printf("%d", searchCount); break;
                case 's':
                    if (searchSelected > 0)
                        printf("%ld (0x%lX)", searchResults[searchSelected - 1],
                               searchResults[searchSelected - 1]);
                    else
                        printf("(none)");
                    break;
                case 'p':  printf("%ld (0x%lX)", binaryDataPtr, binaryDataPtr); break;
                default:   printf("%c", str[i + 1]); break;
                }
                i++;  // skip the char after backslash
            } else {
                putchar(str[i]);
            }
        }
    }

    // Extract the first whitespace-delimited token (lowercased) from line.
    int getCommand(const char* line, char* cmd)
    {
        int i = 0;
        while (i < MAXSTR - 1 && line[i] &&
               line[i] != ' ' && line[i] != '\t' &&
               line[i] != '\r' && line[i] != '\n') {
            cmd[i] = (char)tolower((unsigned char)line[i]);
            i++;
        }
        cmd[i] = '\0';
        return i;
    }

    // Extract everything after the first whitespace-delimited token.
    int getArgument(const char* line, char* arg)
    {
        int i = 0;
        // skip command
        while (i < MAXSTR && line[i] && line[i] != ' ' && line[i] != '\t' &&
               line[i] != '\r' && line[i] != '\n') i++;
        // skip whitespace
        while (i < MAXSTR && (line[i] == ' ' || line[i] == '\t')) i++;
        // copy rest
        int n = 0;
        while (i < MAXSTR && line[i] && line[i] != '\r' && line[i] != '\n') {
            arg[n++] = line[i++];
        }
        arg[n] = '\0';
        return n;
    }

    // Extract a second argument (after first arg) e.g. "fill 16 FF"
    int getSecondArgument(const char* arg, char* second)
    {
        int i = 0;
        // skip first token
        while (arg[i] && arg[i] != ' ' && arg[i] != '\t') i++;
        // skip whitespace
        while (arg[i] == ' ' || arg[i] == '\t') i++;
        int n = 0;
        while (arg[i] && arg[i] != ' ' && arg[i] != '\t' && arg[i] != '\r' && arg[i] != '\n') {
            second[n++] = arg[i++];
        }
        second[n] = '\0';
        return n;
    }

    // ----------------------------------------------------------------
    // Extract the content between the first pair of double quotes.
    // Returns the content length, or 0 if no quoted string found.
    // If endPos is not null, *endPos is set to the index right after
    // the closing quote.
    // ----------------------------------------------------------------
    int extractFirstQuotedString(const char* arg, char* output, int* endPos = nullptr)
    {
        int i = 0;
        while (arg[i] && arg[i] != '"') i++;
        if (!arg[i]) { output[0] = '\0'; if (endPos) *endPos = i; return 0; }
        i++; // skip opening quote
        int n = 0;
        while (arg[i] && arg[i] != '"') output[n++] = arg[i++];
        output[n] = '\0';
        if (endPos) *endPos = arg[i] ? i + 1 : i;
        return n;
    }

    // ----------------------------------------------------------------
    // Starting at startPos, find the next quoted string and extract it.
    // Returns true if a quoted "..." pair was found.
    // ----------------------------------------------------------------
    bool extractQuotedAfter(const char* arg, int startPos, char* output)
    {
        int i = startPos;
        while (arg[i] && arg[i] != '"') i++;
        if (!arg[i]) { output[0] = '\0'; return false; }
        i++;
        int n = 0;
        while (arg[i] && arg[i] != '"') output[n++] = arg[i++];
        output[n] = '\0';
        return true;
    }

    void printUsage(const char* programName)
    {
        printf("Binary Patcher v" VERSION " by Mockba the Borg\n");
        printf("Usage: %s <script file>|-i <binary file> [<output file>] [-v] [-n]\n", programName);
        printf("  -n    dry-run: do not write an output file (for testing)\n");
    }

    void printError(const char* message)
    {
        fprintf(stderr, "Error on line %d: %s\n", lineNo, message);
    }

    // ----------------------------------------------------------------
    // Bounds-check the data pointer for a read/write of `len` bytes.
    // Returns true when access is safe.
    // ----------------------------------------------------------------
    bool boundsCheck(long offset, int len)
    {
        if (offset < 0 || offset + len > binaryFileSize) {
            fprintf(stderr, "Error: access at offset %ld (0x%lX), length %d is out of bounds "
                    "(file size %ld / 0x%lX)\n", offset, offset, len, binaryFileSize, binaryFileSize);
            return false;
        }
        return true;
    }

    // Parse a numeric argument that may be decimal, hex (0x), or octal (0).
    long parseNumber(const char* s)
    {
        return strtol(s, nullptr, 0);
    }

    // ----------------------------------------------------------------
    // Search for the current pattern in binaryData.
    // `startOffset` lets searchnext begin after the pointer.
    // Returns 0 on success, 1 on hard failure (e.g. too many results).
    // ----------------------------------------------------------------
    int doSearch(long startOffset = 0)
    {
        if (searchPatternLen <= 0) {
            if (isVerbose) printf("No search pattern provided\n");
            return 1;
        }
        // mark that a search has been attempted
        searchPerformed = true;
        if (isVerbose) printf("Searching for pattern of %d bytes starting at offset %ld\n",
                              searchPatternLen, startOffset);

        searchCount = 0;
        searchSelected = 0;

        for (long off = startOffset; off <= binaryFileSize - searchPatternLen; off++) {
            bool hit = true;
            for (int j = 0; j < searchPatternLen; j++) {
                if (searchMask[j] && searchPattern[j] != binaryData[off + j]) {
                    hit = false;
                    break;
                }
            }
            if (hit) {
                if (searchCount >= MAXSEARCH) {
                    fprintf(stderr, "Too many search results (>%d), aborting\n", MAXSEARCH);
                    return 1;
                }
                searchResults[searchCount] = off;
                searchCount++;
                if (isVerbose)
                    printf("  Match %d at offset %ld (0x%lX)\n", searchCount, off, off);
            }
        }
        return 0;
    }

    // ----------------------------------------------------------------
    // Process the script file line by line.
    // Returns 0 on success, 1 on failure.
    // ----------------------------------------------------------------
    int processScript(FILE* sf)
    {
        lineNo = 0;
        while (fgets(scriptLine, MAXSTR, sf) != nullptr) {
            lineNo++;
            if (isVerbose) printf("Line %d: %s", lineNo, scriptLine);

            trim(scriptLine, scriptLine);
            if (scriptLine[0] == '\0' || scriptLine[0] == '#') continue;

            getCommand(scriptLine, command);
            getArgument(scriptLine, argument);

            // ---- print / println ----
            if (strcmp(command, "print") == 0 || strcmp(command, "println") == 0) {
                if (strcmp(argument, "$string") == 0) {
                    if (!boundsCheck(binaryDataPtr, 1)) return 1;
                    printf("%s", (char*)binaryData + binaryDataPtr);
                } else if (strcmp(argument, "$int") == 0) {
                    if (!boundsCheck(binaryDataPtr, 4)) return 1;
                    int val; memcpy(&val, binaryData + binaryDataPtr, 4);
                    printf("%d", val);
                } else if (strcmp(argument, "$float") == 0) {
                    if (!boundsCheck(binaryDataPtr, 4)) return 1;
                    float val; memcpy(&val, binaryData + binaryDataPtr, 4);
                    printf("%f", val);
                } else if (strcmp(argument, "$hex8") == 0) {
                    if (!boundsCheck(binaryDataPtr, 1)) return 1;
                    unsigned int val = (unsigned int)binaryData[binaryDataPtr];
                    printf("0x%02X", val);
                } else if (strcmp(argument, "$hex16") == 0) {
                    if (!boundsCheck(binaryDataPtr, 2)) return 1;
                    unsigned short val; memcpy(&val, binaryData + binaryDataPtr, 2);
                    printf("0x%04X", val);
                } else if (strcmp(argument, "$hex32") == 0) {
                    if (!boundsCheck(binaryDataPtr, 4)) return 1;
                    unsigned int val; memcpy(&val, binaryData + binaryDataPtr, 4);
                    printf("%08X", val);
                } else {
                    printStr(argument);
                }
                if (strcmp(command, "println") == 0) printf("\n");
            }
            // ---- verify ----
            else if (strcmp(command, "verify") == 0) {
                // Extract optional custom message (second quoted string), but keep
                // the original argument text so we can parse the first token.
                char customMsg[MAXSTR] = {0};
                bool hasMsg = extractQuotedAfter(argument, 0, customMsg);

                // Obtain first token (before whitespace or a quote)
                char firstTok[MAXSTR] = {0};
                int i = 0;
                while (argument[i] && argument[i] != ' ' && argument[i] != '\t' && argument[i] != '"') {
                    firstTok[i] = argument[i];
                    i++;
                }
                firstTok[i] = '\0';

                if (i == 0) {
                    printError("invalid argument to verify command - aborting");
                    return 1;
                }

                char lowerTok[MAXSTR];
                strToLower(firstTok, lowerTok);

                long expected = -1;
                if (strcmp(lowerTok, "unique") == 0) {
                    expected = 1;
                } else {
                    // try parsing a positive integer
                    expected = parseNumber(firstTok);
                    if (expected <= 0) {
                        printError("invalid argument to verify command - aborting");
                        return 1;
                    }
                }

                if (searchCount != expected) {
                    if (hasMsg) printError(customMsg);
                    else if (expected == 1)
                        printError("previous search was not unique - aborting");
                    else {
                        char msg[MAXSTR];
                        snprintf(msg, MAXSTR, "previous search result count %d does not match expected %ld - aborting", searchCount, expected);
                        printError(msg);
                    }
                    return 1;
                }
            }
            // ---- search / searchall / searchnext ----
            else if (strcmp(command, "search") == 0 ||
                     strcmp(command, "searchall") == 0 ||
                     strcmp(command, "searchnext") == 0) {
                // Extract hex pattern from first quoted string and optional message from second
                char hexPart[MAXSTR];
                char customMsg[MAXSTR] = {0};
                int afterFirst = 0;
                extractFirstQuotedString(argument, hexPart, &afterFirst);
                bool hasMsg = extractQuotedAfter(argument, afterFirst, customMsg);

                searchPatternLen = str2pattern(hexPart, searchPattern, searchMask);
                if (searchPatternLen == 0) {
                    printError("invalid search string - aborting");
                    return 1;
                }

                long startOff = 0;
                if (strcmp(command, "searchnext") == 0)
                    startOff = binaryDataPtr + 1;

                if (doSearch(startOff)) {
                    printError("search failed - aborting");
                    return 1;
                }

                bool isSearchAll = (strcmp(command, "searchall") == 0);

                if (searchCount == 0 && !isSearchAll) {
                    printError(hasMsg ? customMsg : "pattern not found - aborting");
                    return 1;
                }
                if (searchCount == 1) {
                    searchSelected = 1;
                    binaryDataPtr = searchResults[0];
                }
                if (isVerbose) printf("Search count: %d\n", searchCount);
            }
            // ---- select ----
            else if (strcmp(command, "select") == 0) {
                int idx = (int)parseNumber(argument);
                if (idx < 1 || idx > searchCount) {
                    printError("invalid select index - aborting");
                    return 1;
                }
                searchSelected = idx;
                binaryDataPtr = searchResults[idx - 1];
                if (isVerbose)
                    printf("Selected match %d at offset %ld (0x%lX)\n", idx, binaryDataPtr, binaryDataPtr);
            }
            // ---- skip ----
            else if (strcmp(command, "skip") == 0) {
                long off = parseNumber(argument);
                binaryDataPtr += off;
                if (isVerbose)
                    printf("Skipped %ld bytes to offset %ld (0x%lX)\n", off, binaryDataPtr, binaryDataPtr);
            }
            // ---- position ----
            else if (strcmp(command, "position") == 0) {
                binaryDataPtr = parseNumber(argument);
                if (isVerbose)
                    printf("Set position to offset %ld (0x%lX)\n", binaryDataPtr, binaryDataPtr);
            }
            // ---- align ----
            else if (strcmp(command, "align") == 0) {
                long n = parseNumber(argument);
                if (n <= 0) { printError("align value must be positive"); return 1; }
                long rem = binaryDataPtr % n;
                if (rem != 0) binaryDataPtr += (n - rem);
                if (isVerbose)
                    printf("Aligned to %ld-byte boundary at offset %ld (0x%lX)\n",
                           n, binaryDataPtr, binaryDataPtr);
            }
            // ---- patch ----
            else if (strcmp(command, "patch") == 0) {
                removeQuotes(argument, argument);
                patchStringLen = str2bin(argument, patchString);
                if (patchStringLen == 0) {
                    printError("invalid patch string - aborting");
                    return 1;
                }
                if (!boundsCheck(binaryDataPtr, patchStringLen)) return 1;
                if (isVerbose) {
                    printf("Patching %d bytes at offset %ld (0x%lX): ", patchStringLen,
                           binaryDataPtr, binaryDataPtr);
                    printbin(patchString, patchStringLen);
                }
                memcpy(binaryData + binaryDataPtr, patchString, patchStringLen);
            }
            // ---- assert ----
            else if (strcmp(command, "assert") == 0) {
                // Extract hex pattern from first quoted string and optional message from second
                char hexPart[MAXSTR];
                char customMsg[MAXSTR] = {0};
                int afterFirst = 0;
                extractFirstQuotedString(argument, hexPart, &afterFirst);
                bool hasMsg = extractQuotedAfter(argument, afterFirst, customMsg);

                unsigned char assertBuf[MAXPATTERN];
                bool          assertMask[MAXPATTERN];
                int assertLen = str2pattern(hexPart, assertBuf, assertMask);
                if (assertLen == 0) { printError("invalid assert string"); return 1; }
                if (!boundsCheck(binaryDataPtr, assertLen)) return 1;
                bool ok = true;
                for (int j = 0; j < assertLen; j++) {
                    if (assertMask[j] && assertBuf[j] != binaryData[binaryDataPtr + j]) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) {
                    if (hasMsg) {
                        printError(customMsg);
                    } else {
                        fprintf(stderr, "Assertion failed at offset %ld (0x%lX)\n",
                                binaryDataPtr, binaryDataPtr);
                    }
                    fprintf(stderr, "  Expected: "); printbin(assertBuf, assertLen);
                    fprintf(stderr, "  Got:      "); printbin(binaryData + binaryDataPtr, assertLen);
                    return 1;
                }
                if (isVerbose) printf("Assertion passed at offset %ld\n", binaryDataPtr);
            }
            // ---- dump ----
            else if (strcmp(command, "dump") == 0) {
                int n = (int)parseNumber(argument);
                if (n <= 0) { printError("dump count must be positive"); return 1; }
                if (!boundsCheck(binaryDataPtr, n)) return 1;
                printf("Dump %d bytes at offset %ld (0x%lX):\n", n, binaryDataPtr, binaryDataPtr);
                // hex dump with 16 bytes per line + ASCII sidebar
                for (int i = 0; i < n; i += 16) {
                    printf("  %08lX: ", binaryDataPtr + i);
                    int rowEnd = (i + 16 < n) ? i + 16 : n;
                    for (int j = i; j < i + 16; j++) {
                        if (j < rowEnd)
                            printf("%02X ", binaryData[binaryDataPtr + j]);
                        else
                            printf("   ");
                    }
                    printf(" |");
                    for (int j = i; j < rowEnd; j++) {
                        unsigned char c = binaryData[binaryDataPtr + j];
                        putchar((c >= 0x20 && c < 0x7F) ? c : '.');
                    }
                    printf("|\n");
                }
            }
            // ---- fill ----
            else if (strcmp(command, "fill") == 0) {
                char secondArg[MAXSTR];
                int n = (int)parseNumber(argument);
                if (n <= 0) { printError("fill count must be positive"); return 1; }
                if (getSecondArgument(argument, secondArg) == 0) {
                    printError("fill requires a byte value"); return 1;
                }
                unsigned char val = (unsigned char)strtol(secondArg, nullptr, 16);
                if (!boundsCheck(binaryDataPtr, n)) return 1;
                memset(binaryData + binaryDataPtr, val, n);
                if (isVerbose)
                    printf("Filled %d bytes with 0x%02X at offset %ld\n", n, val, binaryDataPtr);
            }
            // ---- read8 / read16 / read32 ----
            else if (strcmp(command, "read8") == 0) {
                if (!boundsCheck(binaryDataPtr, 1)) return 1;
                printf("%u (0x%02X)\n", binaryData[binaryDataPtr], binaryData[binaryDataPtr]);
            }
            else if (strcmp(command, "read16") == 0) {
                if (!boundsCheck(binaryDataPtr, 2)) return 1;
                unsigned short val;
                memcpy(&val, binaryData + binaryDataPtr, 2);
                printf("%u (0x%04X)\n", val, val);
            }
            else if (strcmp(command, "read32") == 0) {
                if (!boundsCheck(binaryDataPtr, 4)) return 1;
                unsigned int val;
                memcpy(&val, binaryData + binaryDataPtr, 4);
                printf("%u (0x%08X)\n", val, val);
            }
            // ---- count ----
            else if (strcmp(command, "count") == 0) {
                printf("%d\n", searchCount);
            }
            // ---- match ----
            else if (strcmp(command, "match") == 0) {
                int m = (int)parseNumber(argument);
                if (m < 1 || m > searchCount) { printError("invalid match index"); return 1; }
                printf("%ld (0x%lX)\n", searchResults[m - 1], searchResults[m - 1]);
            }
            // ---- pointer ----
            else if (strcmp(command, "pointer") == 0) {
                printf("%ld (0x%lX)\n", binaryDataPtr, binaryDataPtr);
            }
            // ---- debug ----
            else if (strcmp(command, "debug") == 0) {
                strToLower(argument, argument);
                if (strcmp(argument, "on") == 0) isVerbose = 1;
                else if (strcmp(argument, "off") == 0) isVerbose = 0;
                else { printError("invalid argument to debug command"); return 1; }
            }
            // ---- quit ----
            else if (strcmp(command, "quit") == 0) {
                if (strlen(argument) > 0) {
                    int limit = (int)parseNumber(argument);
                    // Only quit when the last search returned more than <n> results
                    if (searchCount > limit) {
                        if (isVerbose)
                            printf("Quit: search count %d > %d\n", searchCount, limit);
                        return 0;
                    }
                    // Otherwise continue executing
                    if (isVerbose)
                        printf("Quit condition not met (count %d <= %d), continuing\n",
                               searchCount, limit);
                } else {
                    return 0;
                }
            }
            // ---- unknown ----
            else {
                fprintf(stderr, "Warning on line %d: unknown command '%s'\n", lineNo, command);
            }
        }
        return 0;
    }

} // namespace patcher
} // namespace mockba

// ====================================================================
// main
// ====================================================================
int main(int argc, char** argv)
{
    using namespace mockba::patcher;

    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    // --- Scan argv for -v anywhere after argv[2] ---
    const char* scriptPath  = argv[1];
    const char* binaryPath  = argv[2];
    const char* outputPath  = nullptr;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            isVerbose = 1;
        } else if (strcmp(argv[i], "-n") == 0) {
            noWrite = 1;
        } else if (!outputPath) {
            outputPath = argv[i];
            hasOutputFile = 1;
        } else {
            fprintf(stderr, "Error: unexpected argument '%s'\n", argv[i]);
            printUsage(argv[0]);
            return 1;
        }
    }

    // --- Open script ---
    FILE* sf;
    if (strcmp(scriptPath, "-i") == 0) {
        sf = stdin;
    } else {
        sf = fopen(scriptPath, "r");
        if (!sf) {
            fprintf(stderr, "Error: could not open script file '%s'\n", scriptPath);
            return 1;
        }
    }

    // --- Validate script header ---
    if (fgets(scriptLine, MAXSTR, sf) == nullptr) {
        fprintf(stderr, "Error: script file is empty\n");
        if (sf != stdin) fclose(sf);
        return 1;
    }
    strToLower(scriptLine, scriptLine);
    if (strncmp(scriptLine, "# patch", 7) != 0) {
        fprintf(stderr, "Error: invalid script file (first line must start with '# patch')\n");
        if (sf != stdin) fclose(sf);
        return 1;
    }
    rewind(sf);

    // --- Open and read binary file ---
    FILE* bf = fopen(binaryPath, "rb");
    if (!bf) {
        fprintf(stderr, "Error: could not open binary file '%s'\n", binaryPath);
        if (sf != stdin) fclose(sf);
        return 1;
    }
    fseek(bf, 0, SEEK_END);
    binaryFileSize = ftell(bf);
    rewind(bf);
    if (binaryFileSize <= 0) {
        fprintf(stderr, "Error: binary file is empty or unreadable\n");
        fclose(bf);
        if (sf != stdin) fclose(sf);
        return 1;
    }
    if (isVerbose) printf("Binary file size: %ld bytes\n", binaryFileSize);

    binaryData = (unsigned char*)malloc((size_t)binaryFileSize);
    if (!binaryData) {
        fprintf(stderr, "Error: could not allocate %ld bytes\n", binaryFileSize);
        fclose(bf);
        if (sf != stdin) fclose(sf);
        return 1;
    }
    if ((long)fread(binaryData, 1, (size_t)binaryFileSize, bf) != binaryFileSize) {
        fprintf(stderr, "Error: could not read entire binary file\n");
        free(binaryData);
        fclose(bf);
        if (sf != stdin) fclose(sf);
        return 1;
    }
    fclose(bf);

    // --- Execute script ---
    int result = processScript(sf);
    if (sf != stdin) fclose(sf);

    // --- Write output (unless dry-run) ---
    if (result == 0) {
        if (noWrite) {
            if (isVerbose) printf("Dry-run: not writing output (-n specified)\n");
        } else {
            const char* writePath = hasOutputFile ? outputPath : binaryPath;
            if (isVerbose) printf("Writing %s...\n", writePath);
            FILE* of = fopen(writePath, "wb");
            if (!of) {
                fprintf(stderr, "Error: could not open '%s' for writing\n", writePath);
                free(binaryData);
                return 1;
            }
            if ((long)fwrite(binaryData, 1, (size_t)binaryFileSize, of) != binaryFileSize) {
                fprintf(stderr, "Error: could not write entire file\n");
                fclose(of);
                free(binaryData);
                return 1;
            }
            fclose(of);
        }
    } else {
        fprintf(stderr, "Error: script processing failed\n");
        free(binaryData);
        return 1;
    }

    free(binaryData);
    return 0;
}
