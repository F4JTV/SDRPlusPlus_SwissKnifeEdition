// gps_pvt.h -- Satellite position from broadcast ephemeris,
//              pseudorange / clock corrections, and iterative least-squares
//              position-velocity-time (PVT) solution.
//
// Follows IS-GPS-200 §20.3.3.4.3 (Table 20-IV) for the user algorithm.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

#include "gps_ephemeris.h"

namespace gps {

// WGS-84 constants
constexpr double WGS84_A         = 6378137.0;          // semi-major axis (m)
constexpr double WGS84_F         = 1.0 / 298.257223563;
constexpr double WGS84_E2        = WGS84_F * (2.0 - WGS84_F);
constexpr double GM              = 3.986005e14;        // (m^3/s^2) per IS-GPS-200
constexpr double OMEGA_E_DOT     = 7.2921151467e-5;    // rad/s
constexpr double F_REL           = -4.442807633e-10;   // relativistic correction
constexpr double SPEED_OF_LIGHT  = 299792458.0;        // m/s

// Position of a satellite at a given GPS time (ECEF, m), plus the
// instantaneous clock correction in seconds (already accounting for the
// af0/af1/af2 polynomial, the relativistic eccentricity term, and TGD).
struct SatPos {
    double x = 0.0, y = 0.0, z = 0.0;          // ECEF (m)
    double clock_correction = 0.0;             // satellite clock bias (s)
};

// Compute satellite ECEF position and clock correction at GPS time `t`
// (seconds into week). Implements the user algorithm verbatim.
SatPos satPositionFromEphemeris(const Ephemeris& eph, double t);

// Convenience: ECEF to (latitude_deg, longitude_deg, altitude_m).
struct LLA {
    double lat_deg = 0.0;
    double lon_deg = 0.0;
    double alt_m   = 0.0;
};
LLA ecefToLla(double x, double y, double z);

// A single pseudorange observation paired with the satellite's ephemeris.
struct PseudorangeObs {
    int    prn = 0;
    double tx_time_gps_s = 0.0;   // signal transmit time, GPS seconds of week
    double pseudorange_m = 0.0;   // (T_rx_common - tx_time) * c, BEFORE any
                                  // satellite-clock-correction subtraction
    const Ephemeris* eph = nullptr;
    float  cn0_dBHz = 0.0f;
};

struct PvtSolution {
    bool   valid = false;
    double x = 0.0, y = 0.0, z = 0.0;        // ECEF (m)
    double clock_bias_s = 0.0;                // receiver clock bias (s)
    LLA    lla;
    double gdop = 0.0, pdop = 0.0, hdop = 0.0, vdop = 0.0, tdop = 0.0;
    int    used_sats = 0;
    int    iterations = 0;
    double residual_rms_m = 0.0;
    std::chrono::system_clock::time_point time;
};

// Run iterative least-squares to solve for receiver (x, y, z, c·Δt) given
// N >= 4 pseudorange observations with attached ephemerides. The starting
// guess is the centre of the Earth (or `initial_guess` if non-null), and we
// iterate up to `max_iter` times until the step size drops below 0.1 m.
//
// `t_rx_common_s` is the common receive time (GPS s of week) at which the
// observations were synchronised. It is used to compute the satellite
// transmit time and apply the Sagnac (Earth-rotation) correction.
PvtSolution solvePvtLeastSquares(const std::vector<PseudorangeObs>& obs,
                                 double t_rx_common_s,
                                 const double* initial_guess_xyzct = nullptr,
                                 int max_iter = 12);

} // namespace gps
