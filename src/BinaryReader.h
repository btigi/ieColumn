#pragma once

#include "Config.h"

#include <cstddef>
#include <cstdint>

enum class EvalKind
{
    Empty,
    Int32,
    UInt32,
    String
};

struct EvaluatedValue
{
    EvalKind kind = EvalKind::Empty;
    int64_t i = 0;
    uint64_t u = 0;
    std::wstring text;
};

struct StrRefSource
{
    const uint8_t* data = nullptr;
    size_t size = 0;
};

bool EvaluateColumn(const uint8_t* data, size_t size, const ColumnDef& column, EvaluatedValue& out, const StrRefSource* strrefs = nullptr);
