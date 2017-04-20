// Copyright © 2017 Martin Ueding <dev@martin-ueding.de>
// Licensed under the GNU Public License version 3 (or later).

/**
  \file Additions to QPhiX that are only needed for tmLQCD.

  In the original QPhiX, there are only Wilson fermions and Wilson clover
  fermions. The Dslash operators have a different call signature (the latter
  requiring a clover term), so there is no common base class. With the addition
  of Wilson twisted mass (Mario) and Wilson twisted clover (Peter), there are
  now two instances of the Dslash that have the same signature. In order to
  write a more general even-odd source preparation and solution reconstruction
  code, a common base class for non-clover and clover is desired. In order to
  leave the QPhiX code untouched (for now), this code lives here in tmLQCD.
  */

#pragma once

#include <qphix/blas_new_c.h>

namespace tmlqcd {

size_t constexpr re = 0;
size_t constexpr im = 1;

template <typename FT, int veclen, int soalen, bool compress12>
class Dslash {
 public:
  typedef typename ::QPhiX::Geometry::FourSpinorBlock Spinor;

  /**
    Computes \f$ \psi_\mathrm o = A_\mathrm{oo} \chi_\mathrm o \f$.

    The actual definition of the matrix \f$ A_\mathrm{oo} \f$ is
    implementation dependent and can be the mass factor \f$ \alpha = 4 + m
    \f$ for plain Wilson or something more complicated for twisted mass.

    \param[out] out Output spinor \f$ \psi \f$.
    \param[in] in Input spinor \f$ \chi \f$.
    */
  virtual void A_chi(Spinor *const out, Spinor const *const in, int const isign) const = 0;

  /**
    Computes \f$ \psi_\mathrm e = A_\mathrm{ee}^{-1} \chi_\mathrm e \f$.

    \param[out] out Output spinor \f$ \psi \f$.
    \param[in] in Input spinor \f$ \chi \f$.
    */
  virtual void A_inv_chi(Spinor *const out, Spinor const *const in, int const isign) const = 0;
};

template <typename FT, int veclen, int soalen, bool compress12>
class WilsonDslash : public ::QPhiX::Dslash<FT, veclen, soalen, compress12>, public Dslash {
 public:
  WilsonDslash(template ::QPhiX::Geometry<FT, veclen, soalen, compress12> *geom_,
               double const t_boundary_,
               double const aniso_coeff_S_,
               double const aniso_coeff_T_,
               double const mass_)
      : ::QPhiX::Dslash<FT, veclen, soalen, compress12>(
            geom_, t_boundary_, aniso_coeff_S_, aniso_coeff_T_),
        mass_factor_alpha(4.0 + mass_),
        mass_factor_beta(1.0 / (4.0 * alpha)),
        mass_factor_a(mu_ / mass_factor_alpha),
        mass_factor_b(mass_factor_alpha / (mass_factor_alpha * mass_factor_alpha + mu_ * mu_)) {}

  void A_chi(Spinor *const out, Spinor const *const in, int const isign) const override {
    int const n_blas_simt = 1;
    ::QPhiX::axy(mass_factor_beta, in, out, getGeometry(), n_blas_simt);
  }

  void A_inv_chi(Spinor *const out, Spinor const *const in, int const isign) const override {
    int const n_blas_simt = 1;
    ::QPhiX::axy(1.0 / mass_factor_beta, in, out, getGeometry(), n_blas_simt);
  }

 private:
  double const mass_factor_alpha;
  double const mass_factor_beta;
  double const mass_factor_a;
  double const mass_factor_b;
};

template <typename FT, int veclen, int soalen, bool compress12>
class WilsonTMDslash : public ::QPhiX::Dslash<FT, veclen, soalen, compress12>, public Dslash {
 public:
  WilsonTMDslash(template ::QPhiX::Geometry<FT, veclen, soalen, compress12> *geom_,
                 double const t_boundary_,
                 double const aniso_coeff_S_,
                 double const aniso_coeff_T_,
                 double const mass_,
                 double const mu_)
      : ::QPhiX::Dslash<FT, veclen, soalen, compress12>(
            geom_, t_boundary_, aniso_coeff_S_, aniso_coeff_T_),
        mass_factor_alpha(4.0 + mass_),
        mass_factor_beta(1.0 / (4.0 * alpha)),
        mass_factor_a(mu_ / mass_factor_alpha),
        mass_factor_b(mass_factor_alpha / (mass_factor_alpha * mass_factor_alpha + mu_ * mu_)) {}

  void A_chi(Spinor *const out, Spinor const *const in, int const isign) const override {
    size_t const num_blocks = getGeometry().get_num_blocks();
    for (size_t block = 0u; block < num_blocks; ++block) {
      for (int color = 0; color < 3; ++color) {
        for (int spin_block = 0; spin_block < 2; ++spin_block) {
          // Implement the $\gamma_5$ structure.
          auto const signed_mass_factor_a = mass_factor_a * (spin_block == 0 ? 1.0 : -1.0) * isign;

          for (int half_spin = 0; half_spin < 2; ++half_spin) {
            auto const four_spin = 2 * spin_block + half_spin;
            for (int v = 0; v < soalen; ++v) {
              auto &out_bcs = out[block][color][four_spin];
              auto const &in_bcs = in[block][color][four_spin];

              out_bcs[re][v] =
                  mass_factor_b * (in_bcs[re][v] + signed_mass_factor_a * in_bcs[im][v]);
              out_bcs[im][v] =
                  mass_factor_b * (in_bcs[im][v] - signed_mass_factor_a * in_bcs[re][v]);
            }
          }
        }
      }
    }
  }

  void A_inv_chi(Spinor *const out, Spinor const *const in, int const isign) const override {
    size_t const num_blocks = getGeometry().get_num_blocks();
    for (size_t block = 0u; block < num_blocks; ++block) {
      for (int color = 0; color < 3; ++color) {
        for (int spin_block = 0; spin_block < 2; ++spin_block) {
          // Implement the $\gamma_5$ structure.
          auto const signed_mass_factor_alpha =
              mass_factor_alpha * (spin_block == 0 ? 1.0 : -1.0) * isign;

          for (int half_spin = 0; half_spin < 2; ++half_spin) {
            auto const four_spin = 2 * spin_block + half_spin;
            for (int v = 0; v < soalen; ++v) {
              auto &out_bcs = out[block][color][four_spin];
              auto const &in_bcs = in[block][color][four_spin];

              out_bcs[re][v] =
                  mass_factor_alpha * (in_bcs[re][v] - signed_mass_factor_a * in_bcs[im][v]);
              out_bcs[im][v] =
                  mass_factor_alpha * (in_bcs[im][v] + signed_mass_factor_a * in_bcs[re][v]);
            }
          }
        }
      }
    }
  }

 private:
  double const mass_factor_alpha;
  double const mass_factor_beta;
  double const mass_factor_a;
  double const mass_factor_b;
};
}