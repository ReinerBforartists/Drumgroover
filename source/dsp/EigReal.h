// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
// Real nonsymmetric eigenvalue decomposition (used by the fast Tom/Conga model): balancing + Householder Hessenberg reduction + shifted QR (EISPACK orthes/hqr2, the algorithm of the
// public-domain JAMA library), no external dependency. Used to get the modal form of the small (m x m) Galerkin system at run time.
//   EigReal(n, A, wr, wi, V): A is n x n row-major (destroyed copy is made internally). Eigenvalues wr[k] + i wi[k]; eigenvectors in V (row-major n x n)
//   in the JAMA convention: a real eigenvalue k -> column k is the eigenvector; a complex pair (k, k+1) with wi[k] > 0 -> eigenvector for wr[k] + i wi[k]
//   is V[:,k] + i V[:,k+1] (the conjugate belongs to wi[k+1] < 0).
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace eig {

inline void Cdiv(double xr, double xi, double yr, double yi, double& cr, double& ci)
{
  double r, d;
  if (std::fabs(yr) > std::fabs(yi)) { r = yi / yr; d = yr + r * yi; cr = (xr + r * xi) / d; ci = (xi - r * xr) / d; }
  else { r = yr / yi; d = yi + r * yr; cr = (r * xr + xi) / d; ci = (r * xi - xr) / d; }
}

// scale rows and columns by powers of two until their norms are comparable (eigenvalues unchanged, eigenvectors are back-scaled by the caller through D)
inline void Balance(int n, std::vector<double>& A, std::vector<double>& D)
{
  D.assign(n, 1.0);
  bool done = false;
  for (int pass = 0; pass < 40 && !done; ++pass)
  {
    done = true;
    for (int i = 0; i < n; ++i)
    {
      double c = 0.0, r = 0.0;
      for (int j = 0; j < n; ++j) if (j != i) { c += std::fabs(A[(size_t)j * n + i]); r += std::fabs(A[(size_t)i * n + j]); }
      if (c == 0.0 || r == 0.0) continue;
      double g = r / 2.0, f = 1.0, s = c + r;
      while (c < g) { f *= 2.0; c *= 4.0; }
      g = r * 2.0;
      while (c > g) { f /= 2.0; c /= 4.0; }
      if ((c + r) / f < 0.95 * s)
      {
        done = false; const double fi = 1.0 / f; D[i] *= f;
        for (int j = 0; j < n; ++j) A[(size_t)i * n + j] *= fi;
        for (int j = 0; j < n; ++j) A[(size_t)j * n + i] *= f;
      }
    }
  }
}

inline bool EigReal(int nn, const double* Ain, double* wr, double* wi, double* Vout)
{
  std::vector<double> Hm(Ain, Ain + (size_t)nn * nn), D, Vm((size_t)nn * nn, 0.0), ort(nn, 0.0);
  Balance(nn, Hm, D);
  auto H = [&](int i, int j) -> double& { return Hm[(size_t)i * nn + j]; };
  auto V = [&](int i, int j) -> double& { return Vm[(size_t)i * nn + j]; };
  double* d = wr; double* e = wi;
  const int low = 0, high = nn - 1;
  // ---- orthes ----
  for (int m = low + 1; m <= high - 1; ++m)
  {
    double scale = 0.0;
    for (int i = m; i <= high; ++i) scale += std::fabs(H(i, m - 1));
    if (scale != 0.0)
    {
      double h = 0.0;
      for (int i = high; i >= m; --i) { ort[i] = H(i, m - 1) / scale; h += ort[i] * ort[i]; }
      double g = std::sqrt(h); if (ort[m] > 0) g = -g;
      h -= ort[m] * g; ort[m] -= g;
      for (int j = m; j < nn; ++j) { double f = 0.0; for (int i = high; i >= m; --i) f += ort[i] * H(i, j); f /= h; for (int i = m; i <= high; ++i) H(i, j) -= f * ort[i]; }
      for (int i = 0; i <= high; ++i) { double f = 0.0; for (int j = high; j >= m; --j) f += ort[j] * H(i, j); f /= h; for (int j = m; j <= high; ++j) H(i, j) -= f * ort[j]; }
      ort[m] = scale * ort[m]; H(m, m - 1) = scale * g;
    }
  }
  for (int i = 0; i < nn; ++i) for (int j = 0; j < nn; ++j) V(i, j) = (i == j ? 1.0 : 0.0);
  for (int m = high - 1; m >= low + 1; --m)
    if (H(m, m - 1) != 0.0)
    {
      for (int i = m + 1; i <= high; ++i) ort[i] = H(i, m - 1);
      for (int j = m; j <= high; ++j)
      {
        double g = 0.0; for (int i = m; i <= high; ++i) g += ort[i] * V(i, j);
        g = (g / ort[m]) / H(m, m - 1);
        for (int i = m; i <= high; ++i) V(i, j) += g * ort[i];
      }
    }
  // ---- hqr2 ----
  int n = nn - 1; const double eps = std::pow(2.0, -52.0); double exshift = 0.0, p = 0, q = 0, r = 0, s = 0, z = 0, t, w, x, y;
  double norm = 0.0;
  for (int i = 0; i < nn; ++i) for (int j = std::max(i - 1, 0); j < nn; ++j) norm += std::fabs(H(i, j));
  int iter = 0;
  while (n >= low)
  {
    int l = n;
    while (l > low)
    {
      s = std::fabs(H(l - 1, l - 1)) + std::fabs(H(l, l)); if (s == 0.0) s = norm;
      if (std::fabs(H(l, l - 1)) < eps * s) break;
      --l;
    }
    if (l == n) { H(n, n) += exshift; d[n] = H(n, n); e[n] = 0.0; --n; iter = 0; }
    else if (l == n - 1)
    {
      w = H(n, n - 1) * H(n - 1, n); p = (H(n - 1, n - 1) - H(n, n)) / 2.0; q = p * p + w; z = std::sqrt(std::fabs(q));
      H(n, n) += exshift; H(n - 1, n - 1) += exshift; x = H(n, n);
      if (q >= 0)
      {
        z = p >= 0 ? p + z : p - z; d[n - 1] = x + z; d[n] = d[n - 1]; if (z != 0.0) d[n] = x - w / z; e[n - 1] = 0.0; e[n] = 0.0;
        x = H(n, n - 1); s = std::fabs(x) + std::fabs(z); p = x / s; q = z / s; r = std::sqrt(p * p + q * q); p /= r; q /= r;
        for (int j = n - 1; j < nn; ++j) { z = H(n - 1, j); H(n - 1, j) = q * z + p * H(n, j); H(n, j) = q * H(n, j) - p * z; }
        for (int i = 0; i <= n; ++i) { z = H(i, n - 1); H(i, n - 1) = q * z + p * H(i, n); H(i, n) = q * H(i, n) - p * z; }
        for (int i = low; i <= high; ++i) { z = V(i, n - 1); V(i, n - 1) = q * z + p * V(i, n); V(i, n) = q * V(i, n) - p * z; }
      }
      else { d[n - 1] = x + p; d[n] = x + p; e[n - 1] = z; e[n] = -z; }
      n -= 2; iter = 0;
    }
    else
    {
      x = H(n, n); y = 0.0; w = 0.0;
      if (l < n) { y = H(n - 1, n - 1); w = H(n, n - 1) * H(n - 1, n); }
      if (iter == 10)
      {
        exshift += x; for (int i = low; i <= n; ++i) H(i, i) -= x;
        s = std::fabs(H(n, n - 1)) + std::fabs(H(n - 1, n - 2)); x = y = 0.75 * s; w = -0.4375 * s * s;
      }
      if (iter == 30)
      {
        s = (y - x) / 2.0; s = s * s + w;
        if (s > 0) { s = std::sqrt(s); if (y < x) s = -s; s = x - w / ((y - x) / 2.0 + s); for (int i = low; i <= n; ++i) H(i, i) -= s; exshift += s; x = y = w = 0.964; }
      }
      if (++iter > 300) return false;
      int m = n - 2;
      while (m >= l)
      {
        z = H(m, m); r = x - z; s = y - z;
        p = (r * s - w) / H(m + 1, m) + H(m, m + 1); q = H(m + 1, m + 1) - z - r - s; r = H(m + 2, m + 1);
        s = std::fabs(p) + std::fabs(q) + std::fabs(r); p /= s; q /= s; r /= s;
        if (m == l) break;
        if (std::fabs(H(m, m - 1)) * (std::fabs(q) + std::fabs(r)) < eps * (std::fabs(p) * (std::fabs(H(m - 1, m - 1)) + std::fabs(z) + std::fabs(H(m + 1, m + 1))))) break;
        --m;
      }
      for (int i = m + 2; i <= n; ++i) { H(i, i - 2) = 0.0; if (i > m + 2) H(i, i - 3) = 0.0; }
      for (int k = m; k <= n - 1; ++k)
      {
        const bool notlast = (k != n - 1);
        if (k != m)
        {
          p = H(k, k - 1); q = H(k + 1, k - 1); r = notlast ? H(k + 2, k - 1) : 0.0; x = std::fabs(p) + std::fabs(q) + std::fabs(r);
          if (x == 0.0) continue;
          p /= x; q /= x; r /= x;
        }
        s = std::sqrt(p * p + q * q + r * r); if (p < 0) s = -s;
        if (s != 0)
        {
          if (k != m) H(k, k - 1) = -s * x; else if (l != m) H(k, k - 1) = -H(k, k - 1);
          p += s; x = p / s; y = q / s; z = r / s; q /= p; r /= p;
          for (int j = k; j < nn; ++j)
          {
            p = H(k, j) + q * H(k + 1, j);
            if (notlast) { p += r * H(k + 2, j); H(k + 2, j) -= p * z; }
            H(k, j) -= p * x; H(k + 1, j) -= p * y;
          }
          for (int i = 0; i <= std::min(n, k + 3); ++i)
          {
            p = x * H(i, k) + y * H(i, k + 1);
            if (notlast) { p += z * H(i, k + 2); H(i, k + 2) -= p * r; }
            H(i, k) -= p; H(i, k + 1) -= p * q;
          }
          for (int i = low; i <= high; ++i)
          {
            p = x * V(i, k) + y * V(i, k + 1);
            if (notlast) { p += z * V(i, k + 2); V(i, k + 2) -= p * r; }
            V(i, k) -= p; V(i, k + 1) -= p * q;
          }
        }
      }
    }
  }
  if (norm == 0.0) return false;
  for (n = nn - 1; n >= 0; --n)
  {
    p = d[n]; q = e[n];
    if (q == 0)
    {
      int l = n; H(n, n) = 1.0;
      for (int i = n - 1; i >= 0; --i)
      {
        w = H(i, i) - p; r = 0.0; for (int j = l; j <= n; ++j) r += H(i, j) * H(j, n);
        if (e[i] < 0.0) { z = w; s = r; }
        else
        {
          l = i;
          if (e[i] == 0.0) { H(i, n) = (w != 0.0) ? -r / w : -r / (eps * norm); }
          else
          {
            x = H(i, i + 1); y = H(i + 1, i); q = (d[i] - p) * (d[i] - p) + e[i] * e[i]; t = (x * s - z * r) / q; H(i, n) = t;
            if (std::fabs(x) > std::fabs(z)) H(i + 1, n) = (-r - w * t) / x; else H(i + 1, n) = (-s - y * t) / z;
          }
          t = std::fabs(H(i, n)); if ((eps * t) * t > 1) for (int j = i; j <= n; ++j) H(j, n) /= t;
        }
      }
    }
    else if (q < 0)
    {
      int l = n - 1; double cr, ci;
      if (std::fabs(H(n, n - 1)) > std::fabs(H(n - 1, n))) { H(n - 1, n - 1) = q / H(n, n - 1); H(n - 1, n) = -(H(n, n) - p) / H(n, n - 1); }
      else { Cdiv(0.0, -H(n - 1, n), H(n - 1, n - 1) - p, q, cr, ci); H(n - 1, n - 1) = cr; H(n - 1, n) = ci; }
      H(n, n - 1) = 0.0; H(n, n) = 1.0;
      for (int i = n - 2; i >= 0; --i)
      {
        double ra = 0.0, sa = 0.0, vr, vi;
        for (int j = l; j <= n; ++j) { ra += H(i, j) * H(j, n - 1); sa += H(i, j) * H(j, n); }
        w = H(i, i) - p;
        if (e[i] < 0.0) { z = w; r = ra; s = sa; }
        else
        {
          l = i;
          if (e[i] == 0) { Cdiv(-ra, -sa, w, q, cr, ci); H(i, n - 1) = cr; H(i, n) = ci; }
          else
          {
            x = H(i, i + 1); y = H(i + 1, i);
            vr = (d[i] - p) * (d[i] - p) + e[i] * e[i] - q * q; vi = (d[i] - p) * 2.0 * q;
            if (vr == 0.0 && vi == 0.0) vr = eps * norm * (std::fabs(w) + std::fabs(q) + std::fabs(x) + std::fabs(y) + std::fabs(z));
            Cdiv(x * r - z * ra + q * sa, x * s - z * sa - q * ra, vr, vi, cr, ci); H(i, n - 1) = cr; H(i, n) = ci;
            if (std::fabs(x) > (std::fabs(z) + std::fabs(q))) { H(i + 1, n - 1) = (-ra - w * H(i, n - 1) + q * H(i, n)) / x; H(i + 1, n) = (-sa - w * H(i, n) - q * H(i, n - 1)) / x; }
            else { Cdiv(-r - y * H(i, n - 1), -s - y * H(i, n), z, q, cr, ci); H(i + 1, n - 1) = cr; H(i + 1, n) = ci; }
          }
          t = std::max(std::fabs(H(i, n - 1)), std::fabs(H(i, n)));
          if ((eps * t) * t > 1) for (int j = i; j <= n; ++j) { H(j, n - 1) /= t; H(j, n) /= t; }
        }
      }
    }
  }
  for (int j = nn - 1; j >= low; --j)
    for (int i = low; i <= high; ++i) { z = 0.0; for (int k = low; k <= std::min(j, high); ++k) z += V(i, k) * H(k, j); V(i, j) = z; }
  // undo the balancing on the eigenvectors (A = D^-1 Abal D ... Abal = D^-1 A D with D = diag(D) accumulated as row scale f: Abal(i,:) *= 1/f, Abal(:,i) *= f)
  for (int i = 0; i < nn; ++i) for (int j = 0; j < nn; ++j) Vout[(size_t)i * nn + j] = V(i, j) * D[i];
  return true;
}

}   // namespace eig
