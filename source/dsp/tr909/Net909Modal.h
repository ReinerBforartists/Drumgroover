// E-03: modal port model of a block that loads the NOISE line (BD, SD, HC noise filters and the VCA at their end, closed while the voice is not played).
//
// The block is linearised at its stored circuit state (Net::linearize: Newton Jacobian split into G and C by its step dependence). The response of the block to the NOISE voltage is the response of the linear
// descriptor system  C dx/dt + G x = e u  (u = deviation of the NOISE voltage from its value at the linearisation, e = unit vector of the branch row of the interface source), read at the branch current
// y = -x[br] = load current drawn from the line:  H(s) = -e^T (G + s C)^-1 e.
//
// Modal decomposition: with N = G^-1 C the transfer function is  H(s) = sum_k rho_k / (1 + s mu_k) + d  (mu_k: eigenvalues of N = time constants of the modes, rho_k: residues, d: part of the zero eigenvalues =
// the algebraic part of the block). Only the dynamic nodes (rows / columns of C) carry eigenvalues, the eigenproblem is the one of P = C_RC Ginv_CR on them. The modes that matter for the port are the few ones with a
// residue above a threshold (3 ... 10 per block); all other modes, and the modes that are unstable in the linearisation (the free-running oscillator of the block at an unstable point of its cycle: not part
// of the response of the loads, it would grow without bound), are residualised: their (small) contribution is put into the instantaneous term d, which keeps H(0) exact.
//
// Every mode is integrated with the SAME variable-step BDF2 formulae as the block itself (Net::chargeStampAt), so the model has the discretisation of the circuit and needs no matrix per step size:
//   mu (a0 m + a1 m1 + a2 m2) / h + m = u   =>   m = c0 u + c1,   c0 = 1 / (1 + a0 mu / h),   c1 = -c0 mu (a1 m1 + a2 m2) / h.
// Per step the model delivers its Norton equivalent at the NOISE node,  I = g V + Ih  (g = d + sum Re(rho c0), Ih = I0 - g V0 + sum Re(rho c1)), which the block that solves the line puts in series with the
// Thevenin source of the line: V = (Th - sum of the other loads - Ih) / (gTot + g).
#pragma once
#include "Net909.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace n909 {

class PortModel {
 public:
  using cd = std::complex<double>;
  bool valid = false;
  int n = 0, br = 0, pNode = 0;           // unknowns of the block, row of the source branch, node of the NOISE pin (1-based node index of the block)
  double V0 = 0.0, I0 = 0.0, d = 0.0;     // NOISE voltage and load current at the linearisation, instantaneous conductance of the residualised modes
  std::vector<cd> mu, rho;                // kept modes
  int nDyn = 0, nStable = 0;              // dynamic nodes of the block, stable modes among them (diagnostics)
  double relKeep = 1e-4;                  // keep modes with |rho| >= relKeep * max |rho|
  struct State { std::vector<cd> m1, m2; int stepIndex = 0; double hPrev = 0.0; std::vector<cd> m; double uPrev = 0.0; };   // m1 / m2 / stepIndex / hPrev: BDF2 history (test hook DGG_PART_MODALBDF2), m / uPrev: exact integration
  State st;

  // Builds the model from the block at its present state. tNow: time of the linearisation (dynamic resistors).
  bool build(Net& net, int ifSrc, double tNow) {
    valid = false; n = net.nUnk; const int nn = net.nNodes - 1; br = nn + net.vs[static_cast<size_t>(ifSrc)].br; pNode = net.vs[static_cast<size_t>(ifSrc)].p;
    V0 = net.V(pNode); I0 = net.staticNodeCurrent(pNode, br, tNow);   // the branch current of the stored state is not meaningful (the block was never stepped): the load is the static current of the elements at the NOISE node
    std::vector<double> G, C; net.linearize(G, C, tNow);
    std::vector<int> R, Cc;   // rows and columns of C with entries
    { std::vector<char> rr(static_cast<size_t>(n), 0), cc(static_cast<size_t>(n), 0);
      for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) if (C[static_cast<size_t>(i) * n + j] != 0.0) { rr[static_cast<size_t>(i)] = 1; cc[static_cast<size_t>(j)] = 1; }
      for (int i = 0; i < n; ++i) { if (rr[static_cast<size_t>(i)] && i != br) R.push_back(i); if (cc[static_cast<size_t>(i)] && i != br) Cc.push_back(i); } }
    const int mr = static_cast<int>(R.size()), mc = static_cast<int>(Cc.size());
    // columns of Ginv: e_R (for the right vectors / the left vectors through C) and e_br
    std::vector<double> A = G; std::vector<int> piv(static_cast<size_t>(n)); if (!luFactor(A, piv, n)) return false;
    auto colInv = [&](int e, std::vector<double>& out) {   // out = Ginv e_e with iterative refinement (the matrix spans 1e-12 ... 1e3 S)
      out.assign(static_cast<size_t>(n), 0.0); out[static_cast<size_t>(e)] = 1.0; luSolve(A, piv, n, out); std::vector<double> res(static_cast<size_t>(n));
      for (int ref = 0; ref < 3; ++ref) { for (int i = 0; i < n; ++i) { long double r = (i == e) ? 1.0L : 0.0L; for (int j = 0; j < n; ++j) r -= static_cast<long double>(G[static_cast<size_t>(i) * n + j]) * out[static_cast<size_t>(j)]; res[static_cast<size_t>(i)] = static_cast<double>(r); } luSolve(A, piv, n, res); for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] += res[static_cast<size_t>(i)]; } };
    std::vector<std::vector<double>> gcol(static_cast<size_t>(mr)); for (int j = 0; j < mr; ++j) colInv(R[static_cast<size_t>(j)], gcol[static_cast<size_t>(j)]);   // gcol[j][i] = Ginv[i][R[j]]
    std::vector<double> gbr; colInv(br, gbr);                                                                                                                          // gbr[i] = Ginv[i][br]
    const double H0 = -gbr[static_cast<size_t>(br)];   // H(0) = -Ginv[br][br]
    // P = C[R, Cc] * Ginv[Cc, R]   (mr x mr; the nonzero eigenvalues of N = Ginv C are those of P)
    std::vector<cd> P(static_cast<size_t>(mr) * mr);
    for (int a = 0; a < mr; ++a) for (int b = 0; b < mr; ++b) { double s = 0.0; for (int k = 0; k < mc; ++k) { const double c = C[static_cast<size_t>(R[static_cast<size_t>(a)]) * n + Cc[static_cast<size_t>(k)]]; if (c != 0.0) s += c * gcol[static_cast<size_t>(b)][static_cast<size_t>(Cc[static_cast<size_t>(k)])]; } P[static_cast<size_t>(a) * mr + b] = s; }
    std::vector<cd> T, Q; if (!schur(P, mr, T, Q)) return false;
    // modes: right vector of N v = Ginv[:, R] z, left w^T = l C[R, :]; rho = -(e_br^T v)(w^T g) / (mu l z), g = Ginv e_br
    std::vector<cd> cand, candMu; double maxRho = 0.0; nDyn = mr; nStable = 0;
    std::vector<double> CG(static_cast<size_t>(mr), 0.0);   // (C Ginv e_br)[R]: C[R, :] gbr
    for (int a = 0; a < mr; ++a) { double s = 0.0; for (int k = 0; k < mc; ++k) { const double c = C[static_cast<size_t>(R[static_cast<size_t>(a)]) * n + Cc[static_cast<size_t>(k)]]; if (c != 0.0) s += c * gbr[static_cast<size_t>(Cc[static_cast<size_t>(k)])]; } CG[static_cast<size_t>(a)] = s; }
    double normT = 0.0; for (int i = 0; i < mr; ++i) for (int j = i; j < mr; ++j) normT = std::max(normT, std::abs(T[static_cast<size_t>(i) * mr + j]));
    for (int k = 0; k < mr; ++k) {
      const cd lam = T[static_cast<size_t>(k) * mr + k]; if (std::abs(lam) < 1e-12 * std::max(normT, 1e-300) || std::abs(lam) < 1e-13) continue;   // zero (algebraic) eigenvalue: in d
      if (lam.real() <= 0.0) continue;   // unstable or marginal: residualised
      ++nStable;
      std::vector<cd> y(static_cast<size_t>(mr), cd(0, 0)), r(static_cast<size_t>(mr), cd(0, 0));
      y[static_cast<size_t>(k)] = 1.0;   // right eigenvector of T
      for (int i = k - 1; i >= 0; --i) { cd s = 0; for (int j = i + 1; j <= k; ++j) s += T[static_cast<size_t>(i) * mr + j] * y[static_cast<size_t>(j)]; cd den = T[static_cast<size_t>(i) * mr + i] - lam; if (std::abs(den) < 1e-300) den = cd(1e-300, 0); y[static_cast<size_t>(i)] = -s / den; }
      r[static_cast<size_t>(k)] = 1.0;   // left eigenvector of T (row vector)
      for (int j = k + 1; j < mr; ++j) { cd s = 0; for (int i = k; i < j; ++i) s += r[static_cast<size_t>(i)] * T[static_cast<size_t>(i) * mr + j]; cd den = T[static_cast<size_t>(j) * mr + j] - lam; if (std::abs(den) < 1e-300) den = cd(1e-300, 0); r[static_cast<size_t>(j)] = -s / den; }
      // z = Q y (right eigenvector of P), l = r Q^H (left eigenvector of P as a row)
      std::vector<cd> z(static_cast<size_t>(mr), cd(0, 0)), l(static_cast<size_t>(mr), cd(0, 0));
      for (int i = 0; i < mr; ++i) { cd s = 0; for (int j = 0; j <= k; ++j) s += Q[static_cast<size_t>(i) * mr + j] * y[static_cast<size_t>(j)]; z[static_cast<size_t>(i)] = s; }
      for (int i = 0; i < mr; ++i) { cd s = 0; for (int j = k; j < mr; ++j) s += r[static_cast<size_t>(j)] * std::conj(Q[static_cast<size_t>(i) * mr + j]); l[static_cast<size_t>(i)] = s; }
      cd lz = 0; for (int i = 0; i < mr; ++i) lz += l[static_cast<size_t>(i)] * z[static_cast<size_t>(i)];
      cd ebv = 0; for (int j = 0; j < mr; ++j) ebv += gcol[static_cast<size_t>(j)][static_cast<size_t>(br)] * z[static_cast<size_t>(j)];   // e_br^T v
      cd wg = 0; for (int i = 0; i < mr; ++i) wg += l[static_cast<size_t>(i)] * CG[static_cast<size_t>(i)];                                 // w^T g
      if (std::abs(lz) == 0.0) continue;
      const cd rh = -(ebv * wg) / (lam * lz); maxRho = std::max(maxRho, std::abs(rh));
      candMu.push_back(lam); cand.push_back(rh);
    }
    mu.clear(); rho.clear(); cd keptSum = 0;
    for (size_t i = 0; i < cand.size(); ++i) if (std::abs(cand[i]) >= relKeep * maxRho && std::abs(candMu[i]) >= 1e-11) { mu.push_back(candMu[i]); rho.push_back(cand[i]); keptSum += cand[i]; }
    d = H0 - keptSum.real();
    st.m1.assign(mu.size(), cd(0, 0)); st.m2.assign(mu.size(), cd(0, 0)); st.stepIndex = 0; st.hPrev = 0.0; st.m.assign(mu.size(), cd(0, 0)); st.uPrev = 0.0; cacheH = -1.0;
    valid = true; return true;
  }
  void reset() { std::fill(st.m1.begin(), st.m1.end(), cd(0, 0)); std::fill(st.m2.begin(), st.m2.end(), cd(0, 0)); st.stepIndex = 0; st.hPrev = 0.0; std::fill(st.m.begin(), st.m.end(), cd(0, 0)); st.uPrev = 0.0; }

  // Norton equivalent of the next step of size h: I = g V + ih  (absolute NOISE voltage V).
  // Exact integration of every mode for an input that is linear within the step (the NOISE source is piecewise linear and the steps end at its knots): mu m' + m = u(t), u(t) = u0 + (u1 - u0) t / h
  //   =>  m1 = E m0 + phi u1 + psi u0,   E = exp(-h / mu),  phi = 1 - mu (1 - E) / h,  psi = 1 - E - phi   (per mode one multiplication by cached coefficients and two additions).
  void norton(double h, double& g, double& ih) const {
    double gs = d, hs = 0.0;
    if (bdf2Hook()) {
      double a0, a1, a2; coeffs(h, a0, a1, a2);
      for (size_t k = 0; k < mu.size(); ++k) { const cd c0 = 1.0 / (1.0 + a0 * mu[k] / h); const cd c1 = -c0 * mu[k] * (a1 * st.m1[k] + a2 * st.m2[k]) / h; gs += (rho[k] * c0).real(); hs += (rho[k] * c1).real(); }
    } else {
      prepare(h);
      for (size_t k = 0; k < mu.size(); ++k) { const cd c1 = cE[k] * st.m[k] + cPsi[k] * st.uPrev; gs += (rho[k] * cPhi[k]).real(); hs += (rho[k] * c1).real(); }
    }
    g = gs; ih = I0 - gs * V0 + hs;
  }
  // Accepts the step: the NOISE voltage at its end is vNew; returns the load current at the end of the step.
  double advance(double h, double vNew) {
    const double u = vNew - V0; double y = I0 + d * u;
    if (bdf2Hook()) {
      double a0, a1, a2; coeffs(h, a0, a1, a2);
      for (size_t k = 0; k < mu.size(); ++k) { const cd c0 = 1.0 / (1.0 + a0 * mu[k] / h); const cd mnew = c0 * u - c0 * mu[k] * (a1 * st.m1[k] + a2 * st.m2[k]) / h; st.m2[k] = st.m1[k]; st.m1[k] = mnew; y += (rho[k] * mnew).real(); }
    } else {
      prepare(h);
      for (size_t k = 0; k < mu.size(); ++k) { const cd mnew = cPhi[k] * u + cE[k] * st.m[k] + cPsi[k] * st.uPrev; st.m[k] = mnew; y += (rho[k] * mnew).real(); }
    }
    st.uPrev = u; st.hPrev = h; ++st.stepIndex; return y;
  }

 private:
  mutable double cacheH = -1.0;                  // step size of the cached coefficients
  mutable std::vector<cd> cE, cPhi, cPsi;        // E, phi, psi of every kept mode for cacheH
  static bool bdf2Hook() { static const bool on = std::getenv("DGG_PART_MODALEXACT") == nullptr; return on; }   // BDF2 integration (production) unless the test hook DGG_PART_MODALEXACT=1 selects the exact integration (A/B)
  void prepare(double h) const {
    if (h == cacheH && cE.size() == mu.size()) return;
    cE.resize(mu.size()); cPhi.resize(mu.size()); cPsi.resize(mu.size());
    for (size_t k = 0; k < mu.size(); ++k) {
      const cd z = h / mu[k]; const cd E = std::exp(-z); cd omE, phi;
      if (std::abs(z) < 0.1) {   // series (the direct form cancels): 1 - E = sum (-1)^{j+1} z^j / j!,  phi = sum (-1)^{j+1} z^j / (j+1)!
        cd zp = z; double fj = 1.0; omE = 0.0; phi = 0.0;
        for (int j = 1; j <= 10; ++j) { const double sg = (j % 2 == 1) ? 1.0 : -1.0; omE += sg * zp / fj; phi += sg * zp / (fj * (j + 1)); zp *= z; fj *= (j + 1); }
      } else { omE = 1.0 - E; phi = 1.0 - omE / z; }
      cE[k] = E; cPhi[k] = phi; cPsi[k] = omE - phi;
    }
    cacheH = h;
  }
  void coeffs(double h, double& a0, double& a1, double& a2) const {
    if (st.stepIndex == 0) { a0 = 1; a1 = -1; a2 = 0; }
    else if (st.hPrev > 0) { const double r = h / st.hPrev; a0 = (1 + 2 * r) / (1 + r); a1 = -(1 + r); a2 = r * r / (1 + r); }
    else { a0 = 1.5; a1 = -2; a2 = 0.5; }
  }
  static bool luFactor(std::vector<double>& A, std::vector<int>& piv, int nn) {   // dense LU with partial pivoting (row-major, in place)
    for (int k = 0; k < nn; ++k) {
      int p = k; double mx = std::fabs(A[static_cast<size_t>(k) * nn + k]); for (int i = k + 1; i < nn; ++i) { const double v = std::fabs(A[static_cast<size_t>(i) * nn + k]); if (v > mx) { mx = v; p = i; } }
      piv[static_cast<size_t>(k)] = p; if (mx == 0.0) return false; if (p != k) for (int j = 0; j < nn; ++j) std::swap(A[static_cast<size_t>(k) * nn + j], A[static_cast<size_t>(p) * nn + j]);
      const double dd = A[static_cast<size_t>(k) * nn + k];
      for (int i = k + 1; i < nn; ++i) { const double f = A[static_cast<size_t>(i) * nn + k] / dd; A[static_cast<size_t>(i) * nn + k] = f; if (f != 0.0) for (int j = k + 1; j < nn; ++j) A[static_cast<size_t>(i) * nn + j] -= f * A[static_cast<size_t>(k) * nn + j]; }
    }
    return true;
  }
  static void luSolve(const std::vector<double>& A, const std::vector<int>& piv, int nn, std::vector<double>& b) {
    for (int k = 0; k < nn; ++k) { const int p = piv[static_cast<size_t>(k)]; if (p != k) std::swap(b[static_cast<size_t>(k)], b[static_cast<size_t>(p)]); for (int i = k + 1; i < nn; ++i) b[static_cast<size_t>(i)] -= A[static_cast<size_t>(i) * nn + k] * b[static_cast<size_t>(k)]; }
    for (int i = nn - 1; i >= 0; --i) { double s = b[static_cast<size_t>(i)]; for (int j = i + 1; j < nn; ++j) s -= A[static_cast<size_t>(i) * nn + j] * b[static_cast<size_t>(j)]; b[static_cast<size_t>(i)] = s / A[static_cast<size_t>(i) * nn + i]; }
  }
  // Complex Schur decomposition P = Q T Q^H (row-major m x m): Householder reduction to Hessenberg form, single-shift QR iteration with deflation.
  static bool schur(std::vector<cd>& H, int m, std::vector<cd>& T, std::vector<cd>& Q) {
    auto at = [m](std::vector<cd>& M, int i, int j) -> cd& { return M[static_cast<size_t>(i) * m + j]; };
    Q.assign(static_cast<size_t>(m) * m, cd(0, 0)); for (int i = 0; i < m; ++i) at(Q, i, i) = 1.0;
    for (int k = 0; k + 2 < m; ++k) {   // Householder: zero H[k+2.., k]
      double nrm = 0; for (int i = k + 1; i < m; ++i) nrm += std::norm(at(H, i, k)); nrm = std::sqrt(nrm); if (nrm == 0.0) continue;
      const cd x0 = at(H, k + 1, k); const cd ph = std::abs(x0) > 0 ? x0 / std::abs(x0) : cd(1, 0); const cd alpha = -ph * nrm;
      std::vector<cd> v(static_cast<size_t>(m), cd(0, 0)); for (int i = k + 1; i < m; ++i) v[static_cast<size_t>(i)] = at(H, i, k); v[static_cast<size_t>(k + 1)] -= alpha;
      double vn = 0; for (int i = k + 1; i < m; ++i) vn += std::norm(v[static_cast<size_t>(i)]); if (vn == 0.0) continue; vn = std::sqrt(vn); for (int i = k + 1; i < m; ++i) v[static_cast<size_t>(i)] /= vn;
      for (int j = 0; j < m; ++j) { cd s = 0; for (int i = k + 1; i < m; ++i) s += std::conj(v[static_cast<size_t>(i)]) * at(H, i, j); for (int i = k + 1; i < m; ++i) at(H, i, j) -= 2.0 * v[static_cast<size_t>(i)] * s; }   // H = (I - 2 v v^H) H
      for (int i = 0; i < m; ++i) { cd s = 0; for (int j = k + 1; j < m; ++j) s += at(H, i, j) * v[static_cast<size_t>(j)]; for (int j = k + 1; j < m; ++j) at(H, i, j) -= 2.0 * s * std::conj(v[static_cast<size_t>(j)]); }   // H = H (I - 2 v v^H)
      for (int i = 0; i < m; ++i) { cd s = 0; for (int j = k + 1; j < m; ++j) s += at(Q, i, j) * v[static_cast<size_t>(j)]; for (int j = k + 1; j < m; ++j) at(Q, i, j) -= 2.0 * s * std::conj(v[static_cast<size_t>(j)]); }   // Q = Q (I - 2 v v^H)
    }
    double normH = 0; for (const cd& e : H) normH = std::max(normH, std::abs(e)); const double eps = 1e-16 * std::max(normH, 1e-300);
    int hi = m - 1, iter = 0, total = 0;
    while (hi > 0) {
      if (++total > 60 * m + 200) return false;
      int l = hi; while (l > 0) { if (std::abs(at(H, l, l - 1)) <= eps + 1e-16 * (std::abs(at(H, l - 1, l - 1)) + std::abs(at(H, l, l)))) { at(H, l, l - 1) = 0.0; break; } --l; }
      if (l == hi) { --hi; iter = 0; continue; }
      ++iter;
      const cd a = at(H, hi - 1, hi - 1), b = at(H, hi - 1, hi), c = at(H, hi, hi - 1), dd = at(H, hi, hi);
      const cd tr = a + dd, det = a * dd - b * c, disc = std::sqrt(tr * tr / 4.0 - det); const cd m1 = tr / 2.0 + disc, m2 = tr / 2.0 - disc;
      cd shift = std::abs(m1 - dd) < std::abs(m2 - dd) ? m1 : m2;
      if (iter % 11 == 10) shift = dd + cd(std::abs(c), 0.0) * cd(0.75, 0.25);   // exceptional shift
      for (int i = l; i <= hi; ++i) at(H, i, i) -= shift;
      std::vector<cd> cs(static_cast<size_t>(hi - l)), sn(static_cast<size_t>(hi - l));
      for (int k = l; k < hi; ++k) {   // Givens: zero H[k+1, k]
        const cd f = at(H, k, k), g = at(H, k + 1, k); const double r = std::sqrt(std::norm(f) + std::norm(g)); cd cc, ss;
        if (r == 0.0) { cc = 1.0; ss = 0.0; } else if (std::abs(f) == 0.0) { cc = 0.0; ss = std::conj(g) / std::abs(g); } else { const cd ph = f / std::abs(f); cc = std::abs(f) / r; ss = ph * std::conj(g) / r; }
        cs[static_cast<size_t>(k - l)] = cc; sn[static_cast<size_t>(k - l)] = ss;
        for (int j = k; j < m; ++j) { const cd x = at(H, k, j), y = at(H, k + 1, j); at(H, k, j) = cc * x + ss * y; at(H, k + 1, j) = -std::conj(ss) * x + cc * y; }   // rows k, k+1 over all columns to the right
      }
      for (int k = l; k < hi; ++k) {   // H = R G^H, Q = Q G^H
        const cd cc = cs[static_cast<size_t>(k - l)], ss = sn[static_cast<size_t>(k - l)];
        for (int i = 0; i <= std::min(k + 2, hi); ++i) { const cd x = at(H, i, k), y = at(H, i, k + 1); at(H, i, k) = cc * x + std::conj(ss) * y; at(H, i, k + 1) = -ss * x + cc * y; }
        for (int i = 0; i < m; ++i) { const cd x = at(Q, i, k), y = at(Q, i, k + 1); at(Q, i, k) = cc * x + std::conj(ss) * y; at(Q, i, k + 1) = -ss * x + cc * y; }
      }
      for (int i = l; i <= hi; ++i) at(H, i, i) += shift;
    }
    T = H; return true;
  }
};

}  // namespace n909
