#pragma once

#include <string>

namespace Dumper
{
    void RunDump();
    const std::string& GetDumpOutput();
    bool IsDumping();
    void Log(const std::string& message);
    void ClearLog();
}
