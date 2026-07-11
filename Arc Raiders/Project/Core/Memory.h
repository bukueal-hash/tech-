#pragma once
#include "../Driver/Driver.hpp"

#include <cstdint>
#include <string>
#include <basetsd.h>
#include <windows.h>
#include <psapi.h>
#include <iostream>

class Memory
{
public:
    // Enums
    enum LoadError
    {
        noProcessID,
        noBaseAddress,
        success
    };

    enum MemoryStatus
    {
        bad,
        initialized,
        loaded
    };

protected:
    // Valores que podem ser compartilhados entre inst�ncias
    inline static uint64_t baseAddress = 0;
    inline static int processID = 0;
    inline static int client_base = 0;

private:
    // Status do driver e vari�veis b�sicas
    inline static MemoryStatus status = bad;

    // Contadores de leituras e escritas
    inline static int totalReads = 0;
    inline static int totalWrites = 0;

public:
    Memory() = default;

    Memory::LoadError load(std::string processName);

    static void checkStatus();

    static MemoryStatus getStatus();

    // Informa��es do processo

    static int getProcessID();

    static int getTotalReads();

    static int getTotalWrites();

    // Fun��es de leitura
    static uint64_t getBaseAddress()
    {
        return virtualaddy;
    }

    static uint64_t getClientBase()
    {
        return client_base;
    }

    static bool hookMouse(long x, long y, unsigned short button_flags)
    {
        Kernel::simmousemov1(x, y);
        return true;
    }

    // novo
    static bool read(const void* address, void* buffer, DWORD64 size)
    {
        Kernel::rmem((PVOID)address, buffer, size);
        return true;
    }

    static bool read(DWORD64 address, void* buffer, DWORD64 size)
    {
        return read(reinterpret_cast<void*>(address), buffer, size);
    }

    template <typename T>
    static T read(void* address)
    {
        T buffer{};
        read(address, &buffer, sizeof(T));
        return buffer;
    }

    template <typename T>
    static T read(uint64_t address)
    {
        return read<T>(reinterpret_cast<void*>(address));
    }
    //

    static std::string read_string(DWORD_PTR address, size_t max_length = 256) {
        std::string result;
        result.reserve(max_length);

        for (size_t i = 0; i < max_length; ++i) {
            char c = read<char>(address + i); // solution

            // Parar no null terminator ou em caso de erro
            if (c == '\0' || c == 0) {
                break;
            }

            result += c;
        }

        // Garantir que a string n�o exceda o tamanho m�ximo
        if (result.size() > max_length) {
            result.resize(max_length);
        }

        return result;
    }

    static bool IsValidPtrFast2(uintptr_t address) {
        if (!address)
            return false;
        if (address < 0x10000 || address > 0x7FFFFFFFFFFF)
            return false;
        return true;
    }


    static bool ReadRaw(uintptr_t address, void* buffer, size_t size) {
        if (!IsValidPtrFast2(address))
            return false;

        Kernel::rmem((PVOID)address, buffer, size);
        return true;
    }

    // Fun��es de escrita
    static void write(void* address, const void* buffer, DWORD64 size)
    {
        Kernel::wmem((PVOID)address, (PVOID)buffer, size);
    }

    static void write(DWORD64 address, DWORD64 buffer, DWORD64 size)
    {
        write(reinterpret_cast<void*>(address), reinterpret_cast<void*>(buffer), size);
    }

    template <typename T>
    static void write(void* address, const T& data)
    {
        write(address, &data, sizeof(T));
    }

    template <typename T>
    static void write(uint64_t address, const T& data)
    {
        write<T>(reinterpret_cast<void*>(address), data);
    }

    static bool IsValidPtrFast(void* ptr)
    {
        if (!ptr) {
            return false;
        }

        if (reinterpret_cast<uintptr_t>(ptr) <= 0x400000 || reinterpret_cast<uintptr_t>(ptr) >= 0x7FFFFFFFFFFFULL) {
            return false;
        }

        return true;
    }

    // Fun��o da memoria

    static int32_t GetProcId(LPCTSTR process_name) {
        PROCESSENTRY32 pt;
        HANDLE hsnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        pt.dwSize = sizeof(PROCESSENTRY32);

        if (Process32First(hsnap, &pt)) {
            do {
                if (!lstrcmpi(pt.szExeFile, process_name)) {
                    CloseHandle(hsnap);
                    processID = pt.th32ProcessID;
                    return processID;
                }
            } while (Process32Next(hsnap, &pt));
        }

        CloseHandle(hsnap);
        return 0;
    }

    static uintptr_t GetModule(HANDLE hProcess, const char* moduleName)
    {
        HMODULE hModules[1024];
        DWORD cbNeeded;

        if (EnumProcessModules(hProcess, hModules, sizeof(hModules), &cbNeeded)) {
            for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
                char szModName[MAX_PATH];

                if (GetModuleBaseNameA(hProcess, hModules[i], szModName, sizeof(szModName))) {
                    if (strcmp(szModName, moduleName) == 0) {
                        MODULEINFO modInfo;

                        if (GetModuleInformation(hProcess, hModules[i], &modInfo, sizeof(modInfo))) {
                            return reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll);
                        }
                    }
                }
            }
        }

        return 0;
    }

};
