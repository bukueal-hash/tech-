#pragma once

#include "includes.h"

#include "Dumper.h"
#include <iostream>
#include <iomanip>
#include <ctime>
#include <chrono>

namespace logging {
    template< typename... Args >
    inline void print( const char* format, Args... args ) {
        char buf[1024];
        snprintf(buf, sizeof(buf), format, args...);

        auto now = std::chrono::system_clock::now();
        std::time_t time = std::chrono::system_clock::to_time_t(now);
        tm local_tm;
        localtime_s(&local_tm, &time);

        std::stringstream ss;
        ss << "[" << std::setfill('0') << std::setw(2) << local_tm.tm_mon + 1 << "/"
           << std::setw(2) << local_tm.tm_mday << "/"
           << (local_tm.tm_year + 1900) << " "
           << std::setw(2) << local_tm.tm_hour << ":"
           << std::setw(2) << local_tm.tm_min << ":"
           << std::setw(2) << local_tm.tm_sec << "] "
           << "[flaky] " << buf;
        
        std::string msg = ss.str();
        std::cout << msg << std::endl;
        Dumper::Log(msg + "\n");
    }

    template< typename... Args >
    inline void print_line( const char* format, Args... args ) {
        print(format, args...);
    }
}