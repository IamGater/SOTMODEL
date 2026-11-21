#include "ue/DataTable.h"
#include "core/Json.h"
#include <map>

bool IsTableClass(std::string_view cls) { return cls == "DataTable" || cls == "CurveTable"; }

bool ReadTable(const Package& pkg, size_t exportIdx, TableData& out, std::string* error) {
    try {
        std::string cls = pkg.ExportClass(exportIdx);
        out.curves = cls == "CurveTable";
        size_t end = 0;
        std::string perr;
        PropertyList props = pkg.ReadProperties(exportIdx, &end, &perr);
        if (const PropValue* rs = FindValue(props, "RowStruct")) {
            out.rowStruct = rs->str;
            size_t dot = out.rowStruct.rfind('.');
            if (dot != std::string::npos) out.rowStruct = out.rowStruct.substr(dot + 1);
        }
        Reader r = pkg.ExportReader(exportIdx);
        r.Seek(end);
        if (r.U32()) r.Skip(16);   // object guid
        i32 n = r.I32();
        if (n < 0 || n > (1 << 20)) throw ParseError(Fmt("bad row count %d", n));
        const auto& ex = pkg.Exports()[exportIdx];
        size_t limit = (size_t)ex.serialOffset + ex.serialSize;
        out.rows.reserve(n);
        for (i32 i = 0; i < n; i++) {
            TableRow row;
            row.name = pkg.ReadName(r);
            ReadTaggedProperties(pkg, r, limit, row.fields);
            out.rows.push_back(std::move(row));
        }
        // columns and cell texts: plain fields as they are, struct fields one level deep
        std::map<std::string, size_t> colIndex;
        std::vector<std::vector<std::pair<std::string, std::string>>> flat(out.rows.size());
        for (size_t ri = 0; ri < out.rows.size(); ri++)
            for (auto& f : out.rows[ri].fields) {
                std::string base = f.arrayIndex ? Fmt("%s[%d]", f.name.c_str(), f.arrayIndex) : f.name;
                if (f.value.type == PropType::Struct && f.value.Fields() && !f.value.Fields()->empty() && f.value.Fields()->size() <= 16) {
                    for (auto& sf : *f.value.Fields()) flat[ri].push_back({base + "." + sf.name, TableCell(sf.value)});
                } else {
                    flat[ri].push_back({base, TableCell(f.value)});
                }
            }
        for (auto& row : flat)
            for (auto& [col, v] : row)
                if (colIndex.emplace(col, out.columns.size()).second) out.columns.push_back(col);
        out.cells.resize(out.rows.size());
        for (size_t ri = 0; ri < out.rows.size(); ri++) {
            out.cells[ri].assign(out.columns.size() + 1, "");
            out.cells[ri][0] = out.rows[ri].name;
            for (auto& [col, v] : flat[ri]) out.cells[ri][1 + colIndex[col]] = v;
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

std::string TableCell(const PropValue& v) {
    switch (v.type) {
    case PropType::Bool: return v.b ? "true" : "false";
    case PropType::Int8: case PropType::Int16: case PropType::Int: case PropType::Int64: case PropType::Byte: return std::to_string(v.i);
    case PropType::UInt16: case PropType::UInt32: case PropType::UInt64: return std::to_string(v.u);
    case PropType::Float: case PropType::Double: return Fmt("%g", v.f);
    case PropType::Enum: case PropType::Name: case PropType::Str: case PropType::Text: case PropType::SoftObject: return v.str;
    case PropType::Object: return v.str.empty() ? "None" : v.str;
    default: break;
    }
    // structs / arrays / maps: compact JSON
    JsonWriter w;
    WriteValue(w, v);
    std::string s = w.Take(), out;
    bool inStr = false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '"' && (i == 0 || s[i - 1] != '\\')) inStr = !inStr;
        if (!inStr && (c == '\n' || c == '\r')) continue;
        if (!inStr && c == ' ' && (out.empty() || out.back() == ' ' || out.back() == '{' || out.back() == '[' || out.back() == ',')) continue;
        out += c;
    }
    return out;
}

static std::string CsvField(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += '"';
        o += c;
    }
    return o + "\"";
}

std::string TableToCsv(const TableData& t) {
    std::string out = "Name";
    for (auto& c : t.columns) out += "," + CsvField(c);
    out += "\n";
    for (auto& row : t.cells) {
        for (size_t c = 0; c < row.size(); c++) out += (c ? "," : "") + CsvField(row[c]);
        out += "\n";
    }
    return out;
}

void WriteTableRows(JsonWriter& w, const TableData& t) {
    w.Key("Rows");
    w.BeginObject();
    for (auto& row : t.rows) {
        w.Key(row.name);
        WriteProperties(w, row.fields);
    }
    w.EndObject();
}
