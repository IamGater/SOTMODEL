#pragma once
#include "ue/Package.h"

// UDataTable / UCurveTable: after the object's own properties come int32 NumRows and per row an FName plus the row
// struct's tagged properties (a RichCurve for curve tables).
struct TableRow {
    std::string name;
    PropertyList fields;
};

struct TableData {
    std::string rowStruct;   // DataTable row struct name ("" for curve tables)
    bool curves = false;
    std::vector<TableRow> rows;
    std::vector<std::string> columns;   // field names in first-seen order; struct fields flattened one level ("Acoustics.Thickness")
    std::vector<std::vector<std::string>> cells;   // per row: [0] row name, then one text per column
};

bool IsTableClass(std::string_view cls);
bool ReadTable(const Package& pkg, size_t exportIdx, TableData& out, std::string* error = nullptr);
// One cell as text: scalars as is, structs / arrays as compact JSON.
std::string TableCell(const PropValue& v);
std::string TableToCsv(const TableData& t);
void WriteTableRows(class JsonWriter& w, const TableData& t);   // "Rows": { "RowName": { fields } }
