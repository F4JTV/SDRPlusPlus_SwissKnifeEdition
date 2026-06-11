// gps_pvt.cpp -- IS-GPS-200 user algorithm + LS position solver

#include "gps_pvt.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gps {

namespace {

constexpr double PI = 3.14159265358979323846;

// Compute (x mod 604800) folded to [-302400, +302400].
inline double weekFold(double dt) {
    if (dt >  302400.0) dt -= 604800.0;
    if (dt < -302400.0) dt += 604800.0;
    return dt;
}

// Solve a 4x4 linear system A * x = b in place by partial-pivoting Gauss
// elimination. Returns false if singular.
bool solve4x4(double A[4][4], double b[4], double x[4]) {
    double M[4][5];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) M[i][j] = A[i][j];
        M[i][4] = b[i];
    }
    for (int col = 0; col < 4; col++) {
        int pivot = col;
        double bestAbs = std::fabs(M[col][col]);
        for (int r = col + 1; r < 4; r++) {
            if (std::fabs(M[r][col]) > bestAbs) {
                bestAbs = std::fabs(M[r][col]);
                pivot = r;
            }
        }
        if (bestAbs < 1e-12) return false;
        if (pivot != col) {
            for (int j = 0; j < 5; j++) std::swap(M[col][j], M[pivot][j]);
        }
        double pv = M[col][col];
        for (int j = col; j < 5; j++) M[col][j] /= pv;
        for (int r = 0; r < 4; r++) {
            if (r == col) continue;
            double f = M[r][col];
            if (f == 0.0) continue;
            for (int j = col; j < 5; j++) M[r][j] -= f * M[col][j];
        }
    }
    for (int i = 0; i < 4; i++) x[i] = M[i][4];
    return true;
}

} // namespace

SatPos satPositionFromEphemeris(const Ephemeris& eph, double t) {
    const double A  = eph.sqrtA * eph.sqrtA;
    if (A <= 0.0) return {};                       // empty ephemeris
    const double n0 = std::sqrt(GM / (A * A * A)); // computed mean motion (rad/s)
    double tk = weekFold(t - eph.t_oe);

    const double n = n0 + eph.dn;                  // corrected mean motion
    double Mk = eph.M0 + n * tk;                   // mean anomaly

    // Solve Kepler's equation: Ek = Mk + e * sin(Ek)
    double Ek = Mk;
    for (int iter = 0; iter < 30; iter++) {
        double Ek_new = Mk + eph.e * std::sin(Ek);
        if (std::fabs(Ek_new - Ek) < 1e-13) { Ek = Ek_new; break; }
        Ek = Ek_new;
    }

    const double sE = std::sin(Ek), cE = std::cos(Ek);
    const double sqrt_1_e2 = std::sqrt(1.0 - eph.e * eph.e);

    // True anomaly
    double nu_k = std::atan2(sqrt_1_e2 * sE, cE - eph.e);

    // Argument of latitude
    double Phi_k = nu_k + eph.omega;

    // Second harmonic perturbations
    double s2P = std::sin(2.0 * Phi_k), c2P = std::cos(2.0 * Phi_k);
    double du = eph.C_us * s2P + eph.C_uc * c2P;
    double dr = eph.C_rs * s2P + eph.C_rc * c2P;
    double di = eph.C_is * s2P + eph.C_ic * c2P;

    double uk = Phi_k + du;
    double rk = A * (1.0 - eph.e * cE) + dr;
    double ik = eph.i0 + di + eph.i_dot * tk;

    // Position in orbital plane
    double xkp = rk * std::cos(uk);
    double ykp = rk * std::sin(uk);

    // Corrected longitude of ascending node (with Earth rotation during
    // signal-of-week reference -- subtract Omega_E * t_oe AND apply rate)
    double Omegak = eph.Omega0
                  + (eph.Omega_dot - OMEGA_E_DOT) * tk
                  - OMEGA_E_DOT * eph.t_oe;

    SatPos out;
    out.x = xkp * std::cos(Omegak) - ykp * std::cos(ik) * std::sin(Omegak);
    out.y = xkp * std::sin(Omegak) + ykp * std::cos(ik) * std::cos(Omegak);
    out.z = ykp * std::sin(ik);

    // Clock correction polynomial referenced to t_oc, with relativistic
    // eccentricity term and group delay differential.
    double dt = weekFold(t - eph.t_oc);
    double dt_rel = F_REL * eph.e * eph.sqrtA * sE;
    out.clock_correction = eph.a_f0
                         + eph.a_f1 * dt
                         + eph.a_f2 * dt * dt
                         + dt_rel
                         - eph.T_GD;
    return out;
}

LLA ecefToLla(double x, double y, double z) {
    LLA out;
    out.lon_deg = std::atan2(y, x) * 180.0 / PI;

    double p = std::hypot(x, y);
    // Iterative method for latitude
    double lat = std::atan2(z, p * (1.0 - WGS84_E2));
    for (int i = 0; i < 8; i++) {
        double sLat = std::sin(lat);
        double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sLat * sLat);
        double alt = (p > 1e-6) ? (p / std::cos(lat) - N) : (std::fabs(z) - WGS84_A);
        double lat_new = std::atan2(z, p * (1.0 - WGS84_E2 * N / (N + alt)));
        if (std::fabs(lat_new - lat) < 1e-12) { lat = lat_new; break; }
        lat = lat_new;
    }
    double sLat = std::sin(lat);
    double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sLat * sLat);
    out.lat_deg = lat * 180.0 / PI;
    out.alt_m   = (p > 1e-6) ? (p / std::cos(lat) - N) : (std::fabs(z) - WGS84_A);
    return out;
}

PvtSolution solvePvtLeastSquares(const std::vector<PseudorangeObs>& obs,
                                 double t_rx_common_s,
                                 const double* initial_guess_xyzct,
                                 int max_iter)
{
    PvtSolution out;
    const int N = (int)obs.size();
    if (N < 4) return out;
    (void)t_rx_common_s; // observation pseudoranges already encode receive time

    // State: [x, y, z, c*dt_rx]
    double s[4] = {0.0, 0.0, 0.0, 0.0};
    if (initial_guess_xyzct) {
        for (int i = 0; i < 4; i++) s[i] = initial_guess_xyzct[i];
    }

    // Pre-compute each satellite's "broadcast position" (ECEF at transmit
    // time, no Earth-rotation correction). The Sagnac correction is applied
    // per iteration once we know a transit-time estimate.
    std::vector<SatPos> sat0(N);
    for (int k = 0; k < N; k++) {
        sat0[k] = satPositionFromEphemeris(*obs[k].eph, obs[k].tx_time_gps_s);
    }

    int iter = 0;
    double last_step_norm = 0.0;
    for (; iter < max_iter; iter++) {
        // Build H (N x 4) and y (N) for normal equations
        // y_k = (rho_observed - sat_clock_correction*c)
        //       - || R_sagnac(sat0_k) - r_user || - c*dt_rx
        // partial derivatives: -unit(r_sat - r_user) for x,y,z; +1 for c*dt
        std::vector<double> y(N);
        std::vector<std::array<double,4>> H(N);

        double rms_sum = 0.0;

        for (int k = 0; k < N; k++) {
            // Estimated geometric range (preliminary) to compute transit
            // time for Sagnac correction.
            double dx = sat0[k].x - s[0];
            double dy = sat0[k].y - s[1];
            double dz = sat0[k].z - s[2];
            double range_pre = std::sqrt(dx*dx + dy*dy + dz*dz);
            double transit = range_pre / SPEED_OF_LIGHT;

            // Rotate satellite ECEF by Omega_E * transit (Sagnac)
            double theta = OMEGA_E_DOT * transit;
            double cT = std::cos(theta), sT = std::sin(theta);
            double xs =  cT * sat0[k].x + sT * sat0[k].y;
            double ys = -sT * sat0[k].x + cT * sat0[k].y;
            double zs =  sat0[k].z;

            dx = xs - s[0];
            dy = ys - s[1];
            dz = zs - s[2];
            double r = std::sqrt(dx*dx + dy*dy + dz*dz);

            // Predicted pseudorange: r + c*dt_rx - c*dt_sat
            double pred = r + s[3] - SPEED_OF_LIGHT * sat0[k].clock_correction;

            double residual = obs[k].pseudorange_m - pred;
            y[k] = residual;
            rms_sum += residual * residual;

            H[k][0] = -dx / r;
            H[k][1] = -dy / r;
            H[k][2] = -dz / r;
            H[k][3] = 1.0;
        }
        out.residual_rms_m = std::sqrt(rms_sum / (double)N);

        // Normal equations: A = H^T H, b = H^T y
        double A[4][4] = {};
        double b[4]    = {};
        for (int k = 0; k < N; k++) {
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) A[i][j] += H[k][i] * H[k][j];
                b[i] += H[k][i] * y[k];
            }
        }
        double dx_vec[4];
        if (!solve4x4(A, b, dx_vec)) return out;

        for (int i = 0; i < 4; i++) s[i] += dx_vec[i];
        last_step_norm = std::sqrt(dx_vec[0]*dx_vec[0] + dx_vec[1]*dx_vec[1]
                                 + dx_vec[2]*dx_vec[2]);
        if (last_step_norm < 0.1) { iter++; break; }
    }

    // Final DOP computation: invert (H^T H) with the converged H
    {
        std::vector<std::array<double,4>> H(N);
        for (int k = 0; k < N; k++) {
            double dx = sat0[k].x - s[0];
            double dy = sat0[k].y - s[1];
            double dz = sat0[k].z - s[2];
            double r = std::sqrt(dx*dx + dy*dy + dz*dz);
            H[k][0] = -dx / r;
            H[k][1] = -dy / r;
            H[k][2] = -dz / r;
            H[k][3] = 1.0;
        }
        double HTH[4][4] = {};
        for (int k = 0; k < N; k++) {
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) HTH[i][j] += H[k][i] * H[k][j];
            }
        }
        // Invert HTH via Gauss-Jordan on [HTH | I]
        double M[4][8] = {};
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) M[i][j] = HTH[i][j];
            M[i][4 + i] = 1.0;
        }
        bool ok = true;
        for (int col = 0; col < 4 && ok; col++) {
            int pivot = col;
            double bestAbs = std::fabs(M[col][col]);
            for (int r = col + 1; r < 4; r++) {
                if (std::fabs(M[r][col]) > bestAbs) {
                    bestAbs = std::fabs(M[r][col]); pivot = r;
                }
            }
            if (bestAbs < 1e-12) { ok = false; break; }
            if (pivot != col) {
                for (int j = 0; j < 8; j++) std::swap(M[col][j], M[pivot][j]);
            }
            double pv = M[col][col];
            for (int j = 0; j < 8; j++) M[col][j] /= pv;
            for (int r = 0; r < 4; r++) {
                if (r == col) continue;
                double f = M[r][col]; if (f == 0.0) continue;
                for (int j = 0; j < 8; j++) M[r][j] -= f * M[col][j];
            }
        }
        if (ok) {
            // Q matrix (covariance, in metres^2) is M[*][4..7]
            // For DOP we need it in a local east-north-up frame, but for a
            // first pass we report the ECEF diagonal directly: GDOP = sqrt(trace).
            double gdop2 = M[0][4] + M[1][5] + M[2][6] + M[3][7];
            double pdop2 = M[0][4] + M[1][5] + M[2][6];
            out.gdop = std::sqrt(std::max(0.0, gdop2));
            out.pdop = std::sqrt(std::max(0.0, pdop2));
            // Rotate to ENU for HDOP/VDOP using the user latitude/longitude
            double lat = std::atan2(s[2], std::hypot(s[0], s[1])
                                          * (1.0 - WGS84_E2));
            double lon = std::atan2(s[1], s[0]);
            double sl = std::sin(lat), cl = std::cos(lat);
            double so = std::sin(lon), co = std::cos(lon);
            // Rotation R from ECEF to ENU
            double R[3][3] = {
                { -so,        co,        0.0 },
                { -sl*co,    -sl*so,     cl  },
                {  cl*co,     cl*so,     sl  }
            };
            // covariance in ENU = R * Q3x3 * R^T
            double Q[3][3];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) Q[i][j] = M[i][4 + j];
            double RQ[3][3] = {};
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    for (int k = 0; k < 3; k++) RQ[i][j] += R[i][k] * Q[k][j];
            double Qenu[3][3] = {};
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    for (int k = 0; k < 3; k++) Qenu[i][j] += RQ[i][k] * R[j][k];
            out.hdop = std::sqrt(std::max(0.0, Qenu[0][0] + Qenu[1][1]));
            out.vdop = std::sqrt(std::max(0.0, Qenu[2][2]));
            out.tdop = std::sqrt(std::max(0.0, M[3][7]));
        }
    }

    out.x = s[0]; out.y = s[1]; out.z = s[2];
    out.clock_bias_s = s[3] / SPEED_OF_LIGHT;
    out.lla = ecefToLla(s[0], s[1], s[2]);
    out.used_sats = N;
    out.iterations = iter;
    // Final sanity: valid if iteration converged AND the result is plausible
    // (within ±2× Earth radius of surface).
    double r_user = std::sqrt(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);
    out.valid = (last_step_norm < 1.0)
             && (r_user > 0.5 * WGS84_A) && (r_user < 1.5 * WGS84_A);
    return out;
}

} // namespace gps
