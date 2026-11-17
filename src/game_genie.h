#pragma once

#include <stdint.h>

class GameGenie {
public:
    enum class LoadResult { Missing, Loaded, Error, Limited };

    void clear();
    LoadResult load(const char* rom_path);
    unsigned count() const { return entry_count; }
    const char* label(unsigned index) const { return entries[index].label; }
    bool enabled(unsigned index) const { return entries[index].enabled; }
    void toggle(unsigned index);
    bool add_manual(const char* code);

    // Called only for cartridge ROM reads, after the mapper selected the bank.
    uint8_t patch(uint16_t address, uint8_t original) const {
        if (!active_count || !(active_pages[address >> 13] & (1u << ((address >> 8) & 31))))
            return original;
        return patch_active(address, original);
    }

private:
    struct Code {
        uint16_t address; // bit 15 means the code compares the original byte
        uint8_t value;
        uint8_t compare;
    };
    struct Entry {
        char label[40];
        uint16_t id;
        uint8_t first;
        uint8_t size;
        bool enabled;
    };
    static constexpr unsigned max_entries = 64;
    static constexpr unsigned max_codes = 160;
    static constexpr uint16_t manual_id = UINT16_MAX;

    Entry entries[max_entries];
    Code codes[max_codes];
    uint8_t active[max_codes]; // indexes into codes, sorted by address
    uint32_t active_pages[4];
    unsigned entry_count = 0;
    unsigned code_count = 0;
    unsigned active_count = 0;

    static bool decode(const char* first, const char* last, Code& code);
    Entry* find(uint16_t id);
    bool append(uint16_t id, const char* text, const char* label, bool enabled, bool& limited);
    void rebuild();
    uint8_t patch_active(uint16_t address, uint8_t original) const;
};
