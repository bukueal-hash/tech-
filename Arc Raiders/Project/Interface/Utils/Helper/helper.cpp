#include "helper.hpp"
#include <TlHelp32.h>
#include <tchar.h>
#include <string>
#include <Psapi.h>
#include <unordered_map>

float Helper::MaxFloat(float a, float b) {
    return (a > b) ? a : b;
}

float Helper::MinFloat(float a, float b) {
    return (a < b) ? a : b;
}

void Helper::Log(std::string name, uintptr_t offset)
{
    std::cout << " " << name << "()-> " << offset << "\n";
}