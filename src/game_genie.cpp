#include "game_genie.h"
#include "ff.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace {
bool space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r';
}

int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

enum class Field { Code, Label, Enable, Ignore };

Field parse_field(char* line, uint16_t& id, char*& value) {
    if (!strncmp(line, "\xEF\xBB\xBF", 3)) line += 3;
    while (space(*line)) ++line;
    if (strncmp(line, "cheat", 5) || line[5] < '0' || line[5] > '9') return Field::Ignore;
    char* key;
    const unsigned long number = strtoul(line + 5, &key, 10);
    if (number >= UINT16_MAX) return Field::Ignore;
    char* equals = strchr(key, '=');
    if (!equals) return Field::Ignore;
    char* end = equals;
    while (end > key && space(end[-1])) --end;
    *end = '\0';
    Field field = Field::Ignore;
    if (!strcmp(key, "_code")) field = Field::Code;
    if (!strcmp(key, "_desc")) field = Field::Label;
    if (!strcmp(key, "_enable")) field = Field::Enable;
    if (field == Field::Ignore) return field;

    value = equals + 1;
    while (space(*value)) ++value;
    if (*value == '"') {
        ++value;
        end = value;
        char* out = value;
        while (*end && *end != '"') {
            if (*end == '\\' && (end[1] == '\\' || end[1] == '"')) ++end;
            *out++ = *end++;
        }
        if (*end != '"') return Field::Ignore;
        *out = '\0';
    } else {
        end = value + strcspn(value, "#");
        while (end > value && space(end[-1])) --end;
        *end = '\0';
    }
    id = static_cast<uint16_t>(number);
    return field;
}

template<typename ParseLine>
bool read_lines(FIL& file, ParseLine parse_line) {
    static char line[512];
    static char block[256];
    unsigned length = 0;
    bool overflow = false;
    for (;;) {
        UINT read = 0;
        if (f_read(&file, block, sizeof block, &read) != FR_OK) return false;
        if (!read) break;
        for (unsigned i = 0; i < read; ++i) {
            if (block[i] == '\n') {
                if (!overflow) {
                    line[length] = '\0';
                    parse_line(line);
                }
                length = 0;
                overflow = false;
            } else if (length < sizeof line - 1) {
                line[length++] = block[i];
            } else {
                overflow = true;
            }
        }
    }
    if (length && !overflow) {
        line[length] = '\0';
        parse_line(line);
    }
    return true;
}
} // namespace

void GameGenie::clear() {
    entry_count = code_count = active_count = 0;
}

bool GameGenie::decode(const char* first, const char* last, Code& code) {
    while (first < last && space(*first)) ++first;
    while (last > first && space(last[-1])) --last;
    const auto length = last - first;
    if (length != 7 && length != 11) return false;
    uint8_t digits[9];
    unsigned count = 0;
    for (int i = 0; i < length; ++i) {
        if (i == 3 || i == 7) {
            if (first[i] != '-') return false;
        } else {
            const int digit = hex_digit(first[i]);
            if (digit < 0) return false;
            digits[count++] = digit;
        }
    }
    const unsigned address = ((digits[5] ^ 15u) << 12) | (digits[2] << 8) |
                             (digits[3] << 4) | digits[4];
    if (address >= 0x8000) return false;
    code.address = address;
    code.value = (digits[0] << 4) | digits[1];
    code.compare = 0;
    if (count == 9) {
        const unsigned encoded = (digits[6] << 4) | digits[8];
        code.compare = ((encoded >> 2) | (encoded << 6)) ^ 0xBA;
        code.address |= 0x8000;
    }
    return true;
}

GameGenie::Entry* GameGenie::find(uint16_t id) {
    for (unsigned i = 0; i < entry_count; ++i)
        if (entries[i].id == id) return &entries[i];
    return nullptr;
}

bool GameGenie::append(uint16_t id, const char* text, const char* label, bool enabled, bool& limited) {
    if (entry_count == max_entries) {
        limited = true;
        return false;
    }
    unsigned next = code_count;
    for (const char* part = text; ; ) {
        const char* end = part + strcspn(part, "+");
        Code code;
        if (!decode(part, end, code)) return false;
        if (next == max_codes) {
            limited = true;
            return false;
        }
        codes[next++] = code;
        if (!*end) break;
        part = end + 1;
    }
    Entry& entry = entries[entry_count++];
    snprintf(entry.label, sizeof entry.label, "%s", label);
    entry.id = id;
    entry.first = code_count;
    entry.size = next - code_count;
    entry.enabled = enabled;
    code_count = next;
    return true;
}

GameGenie::LoadResult GameGenie::load(const char* rom_path) {
    clear();
    if (!*rom_path) return LoadResult::Missing;
    char path[256];
    const char* dot = strrchr(rom_path, '.');
    if (!dot || snprintf(path, sizeof path, "%.*s.cht", static_cast<int>(dot - rom_path),
                         rom_path) >= static_cast<int>(sizeof path))
        return LoadResult::Error;

    static FIL file;
    const FRESULT result = f_open(&file, path, FA_READ);
    if (result == FR_NO_FILE || result == FR_NO_PATH) return LoadResult::Missing;
    if (result != FR_OK) return LoadResult::Error;

    bool limited = false;
    // First collect valid code groups; labels and switches can precede codes.
    bool metadata = false;
    const auto parse_line = [&](char* line) {
        uint16_t id;
        char* value;
        const Field field = parse_field(line, id, value);
        if (!metadata) {
            if (field == Field::Code && !find(id))
                append(id, value, value, false, limited);
            return;
        }
        if (field != Field::Label && field != Field::Enable) return;
        Entry* entry = find(id);
        if (!entry) return;
        if (field == Field::Enable) entry->enabled = !strcasecmp(value, "true") || !strcmp(value, "1");
        else if (*value) snprintf(entry->label, sizeof entry->label, "%s", value);
    };
    const bool codes_loaded = read_lines(file, parse_line);
    metadata = true;
    const bool ok = codes_loaded && f_lseek(&file, 0) == FR_OK && read_lines(file, parse_line);
    const FRESULT closed = f_close(&file);
    if (!ok || closed != FR_OK) {
        clear();
        return LoadResult::Error;
    }
    rebuild();
    return limited ? LoadResult::Limited : LoadResult::Loaded;
}

void GameGenie::toggle(unsigned index) {
    entries[index].enabled = !entries[index].enabled;
    rebuild();
}

bool GameGenie::add_manual(const char* code) {
    bool limited = false;
    if (!append(manual_id, code, code, true, limited)) return false;
    rebuild();
    return true;
}

void GameGenie::rebuild() {
    active_count = 0;
    memset(active_pages, 0, sizeof active_pages);
    for (unsigned i = 0; i < entry_count; ++i) {
        const Entry& entry = entries[i];
        if (!entry.enabled) continue;
        for (unsigned n = entry.first; n < entry.first + entry.size; ++n) {
            const unsigned address = codes[n].address & 0x7fff;
            active_pages[address >> 13] |= 1u << ((address >> 8) & 31);
            unsigned pos = active_count++;
            while (pos && (codes[active[pos - 1]].address & 0x7fff) > address) {
                active[pos] = active[pos - 1];
                --pos;
            }
            active[pos] = n;
        }
    }
}

uint8_t GameGenie::patch_active(uint16_t address, uint8_t original) const {
    unsigned first = 0;
    unsigned end = active_count;
    while (first < end) {
        const unsigned middle = first + (end - first) / 2;
        if ((codes[active[middle]].address & 0x7fff) < address) first = middle + 1;
        else end = middle;
    }
    for (; first < active_count; ++first) {
        const Code& code = codes[active[first]];
        if ((code.address & 0x7fff) != address) break;
        if (!(code.address & 0x8000) || code.compare == original) return code.value;
    }
    return original;
}
