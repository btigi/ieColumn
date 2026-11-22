#pragma once

#include "Config.h"

std::wstring PropertyCanonicalName(const std::wstring& handlerId, const ColumnDef& column);
std::wstring BuildPropertySchemaXml(const HandlerConfig& config);
std::wstring BuildPropList(const TypeHandler& handler);
HRESULT WritePropertySchemaFile(const HandlerConfig& config, const std::wstring& path, std::wstring& error);
