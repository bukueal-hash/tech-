#pragma once

// Practice / calibration reference mode (Phase 4 -- T4.4).
//
// The external release's "practice" feature clones the local player into its own
// ImGui scene; it is not a read of a practice world. This codebase has no
// practice-scope offset, actor class or map to read -- searched, nothing found --
// and inventing one would be a guess (C4). So this mode surfaces the reference
// objects the calibration work actually needs, and only while it is on:
//
//   centre_cross   the projection screen centre the aim error is measured from
//   fov_ring       the aim cone, drawn from the same conversion the aim path uses
//   axis_ruler     tick marks every 5 degrees along both axes, so a known angle can
//                  be checked against where the projection actually puts it (T4.5)
//   counts_probe   the last commanded mouse counts and the error that produced
//                  them, so counts-per-pixel can be read off instead of guessed
//
// Normal ESP output is untouched: nothing here draws unless the mode is on, and
// the aim path never consults this unit.

#include <cstddef>
#include <string>

namespace PracticeMode {

struct ReferenceObject {
    const char* id;      // stable identifier, used in the panel listing
    const char* label;   // human name
    const char* purpose; // what it is for
};

// The fixed, complete list of what this mode draws. A user turning the mode on is
// told what to expect rather than left to find it, and the panel lists exactly
// these ids -- if drawing and this list ever disagree, the list is the contract
// that says so.
inline const ReferenceObject* Objects(size_t& count)
{
    static const ReferenceObject kObjects[] = {
        { "centre_cross", "Screen centre cross",
          "the reference the aim error is measured from" },
        { "fov_ring", "Aim FOV ring",
          "the cone the candidate outside_fov test uses" },
        { "axis_ruler", "5-degree axis ruler",
          "where the projection puts a known angle" },
        { "counts_probe", "Mouse counts probe",
          "the counts a given pixel error produced" },
    };
    count = sizeof(kObjects) / sizeof(kObjects[0]);
    return kObjects;
}

inline std::string ObjectListLine()
{
    size_t count = 0;
    const ReferenceObject* objects = Objects(count);
    std::string line;
    for (size_t i = 0; i < count; ++i) {
        if (!line.empty())
            line += ", ";
        line += objects[i].id;
    }
    return line;
}

} // namespace PracticeMode
