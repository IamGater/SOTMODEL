#pragma once
#include "core/Vfs.h"
#include <unordered_map>

// Localised strings from the .locres files under <Project>/Localisation/<Target>/<culture>/ (FText namespace + key ->
// translated text). Legacy (Athena) and the later versioned formats are read.
struct LocEntry {
    std::string ns, key, text;
};

class Localization {
public:
    static std::vector<std::string> Cultures(const Vfs& vfs);   // "en", "de-DE", ...
    bool Load(const Vfs& vfs, const std::string& culture, std::string* error = nullptr);
    void Clear();
    const std::string& Culture() const { return m_culture; }
    size_t Size() const { return m_entries.size(); }
    const std::vector<LocEntry>& Entries() const { return m_entries; }
    const std::string* Find(std::string_view ns, std::string_view key) const;
    // case-insensitive substring search over text (and key); all space separated terms must match
    std::vector<const LocEntry*> Search(std::string_view query, size_t limit, bool inKeys = false) const;

private:
    bool ParseLocres(const Bytes& data, std::string* error);
    std::string m_culture;
    std::vector<LocEntry> m_entries;
    std::unordered_map<std::string, size_t> m_index;   // ns + '\x1f' + key
};

// The table used when FText values are written as JSON ("LocalizedString"); null = none.
void SetJsonLocalization(const Localization* loc);
