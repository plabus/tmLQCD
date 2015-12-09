/***********************************************************************
 * Copyright (C) 2015 Bartosz Kostrzewa, Florian Burger
 *
 * This file is part of tmLQCD.
 *
 * tmLQCD is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * tmLQCD is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with tmLQCD.  If not, see <http://www.gnu.org/licenses/>.
 *
 * File: rg_mixed_cg_her.c
 *
 * CG solver for hermitian f only!
 *
 * The externally accessible functions are
 *
 *   Q: source
 * inout:
 *   P: initial guess and result
 * 
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
# include<config.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "global.h"
#include "su3.h"
#include "linalg_eo.h"
#include "start.h"
#include "operator/tm_operators_32.h"
#include "operator/clovertm_operators_32.h"
#include "solver/matrix_mult_typedef.h"
#include "read_input.h"

#include "solver_field.h"
#include "solver/rg_mixed_cg_her.h"
#include "gettime.h"

#define DELTA 1.0e-2f
#define PR 0

void output_flops(const double seconds, const unsigned int N, const unsigned int iter_out, const unsigned int iter_in_sp, const unsigned int iter_in_dp, const double eps_sq);

static inline unsigned int inner_loop_high(spinor * const x, spinor * const p, spinor * const q, spinor * const r, double * const rho1, double delta,
                              matrix_mult f, const double eps_sq, const unsigned int N, const unsigned int iter ){

  static double alpha, beta, rho, rhomax;
  unsigned int j = 0;

  rho = *rho1;
  rhomax = *rho1;

  /* break out of inner loop if iterated residual goes below some fraction of the maximum observed
  * iterated residual since the last update or if the target precision has been reached 
  * enforce convergence more strictly by a factor of 1.3 to avoid unnecessary restarts 
  * if the real residual is still a bit too large */
  while( rho > delta*rhomax ){
    ++j;
    f(q,p);
    alpha = rho/scalar_prod_r(p,q,N,1);
    assign_add_mul_r(x, p, alpha, N);
    assign_add_mul_r(r, q, -alpha, N);
    rho = square_norm(r,N,1);
    beta = rho / *rho1;
    *rho1 = rho;
    assign_mul_add_r(p, beta, r, N);
    
    if( 1.3*rho < eps_sq ) break;
    if( rho > rhomax ) rhomax = rho;
    
    if(g_debug_level > 2 && g_proc_id == 0) {
      printf("DP_inner CG: %d res^2 %g\t\n", j+iter, rho);
    }
  }

  return j;
}

static inline unsigned int inner_loop(spinor32 * const x, spinor32 * const p, spinor32 * const q, spinor32 * const r, float * const rho1, float delta,
                              matrix_mult32 f32, const float eps_sq, const unsigned int N, const unsigned int iter,
                              float alpha, float beta, int pipelined, int pr ){

  static float rho, rhomax, pro;
  unsigned int j = 0;

  rho = *rho1;
  rhomax = *rho1;

  if(pipelined==0){
    /* break out of inner loop if iterated residual goes below some fraction of the maximum observed
    * iterated residual since the last update or if the target precision has been reached 
    * enforce convergence more strictly by a factor of 1.3 to avoid unnecessary restarts 
    * if the real residual is still a bit too large */
    while( rho > delta*rhomax ){
      ++j;
      f32(q,p);
      pro = scalar_prod_r_32(p,q,N,1);
      alpha = rho/pro;
      assign_add_mul_r_32(x, p, alpha, N);
      assign_add_mul_r_32(r, q, -alpha, N);
      rho = square_norm_32(r,N,1);
      // Polak-Ribiere computation of beta, claimed to be self-stabilising, positive effect so far not observed or required
      if(pr==1){
        beta = alpha*(alpha*square_norm_32(q,N,1)-pro) / *rho1;
      }else{
        beta = rho / *rho1;
      }
      *rho1 = rho;
      assign_mul_add_r_32(p, beta, r, N);
      if(g_debug_level > 2 && g_proc_id == 0) {
        printf("SP_inner CG: %d res^2 %g\t\n", j+iter, rho);
      }
      /* break out of inner loop if iterated residual goes below some fraction of the maximum observed
       * iterated residual since the last update or if the target precision has been reached 
       * enforce convergence more strictly by a factor of 1.3 to avoid unnecessary restarts 
       * if the real residual is still a bit too large */
      if( 1.3*rho < eps_sq ) break;
      if( rho > rhomax ) rhomax = rho;
    }
  }else{
    // pipelined cg requires one more scalar product but may allow optimisations to be made
    // e.g.: one could do the collective communication for sqrnrm(r) while other stuff is being computed
    // it is also self-initialising (alpha=0, beta=0 will work)
    while( rho > delta*rhomax ){
      ++j;
      assign_add_mul_r_32(x, p, alpha, N);
      assign_add_mul_r_32(r, q, -alpha, N);
      assign_mul_add_r_32(p, beta, r, N);
      f32(q,p);
  
      rho = square_norm_32(r,N,1);
      pro = scalar_prod_r_32(p,q,N,1);
      alpha = rho/pro;
      if(pr==1){
        beta = alpha*(alpha*square_norm_32(q,N,1)-pro)/rho;
      }else{
        beta = rho/ *rho1;
      }
      *rho1=rho;

      if(g_debug_level > 2 && g_proc_id == 0) {
        printf("SP_inner CG: %d res^2 %g\t\n", j+iter, rho);
      }
      if( 1.3*rho < eps_sq ) break;
      if( rho > rhomax ) rhomax = rho;
    }
  }

  return j;
}


/* P output = solution , Q input = source */
int rg_mixed_cg_her(spinor * const P, spinor * const Q, const int max_iter, 
		 double eps_sq, const int rel_prec, const int N, matrix_mult f, matrix_mult32 f32) {

  int iter_in_sp = 0, iter_in_dp = 0, iter_out = 0, i;
  float rho_sp;
  double beta_dp, rho_dp;
  double sourcesquarenorm, target_eps_sq;

  spinor *xhigh, *rhigh, *qhigh, *phigh;
  spinor32 *x, *p, *q, *r;

  spinor ** solver_field = NULL;
  spinor32 ** solver_field32 = NULL;  
  const int nr_sf = 4;
  const int nr_sf32 = 4;
  
  // for now use constant residual reduction factor, 0.01 seems to work very well in general
  float delta = DELTA;

  int high_control = 0;

  double atime, etime, flops;
  
  if(N == VOLUME) {
    init_solver_field(&solver_field, VOLUMEPLUSRAND, nr_sf);    
    init_solver_field_32(&solver_field32, VOLUMEPLUSRAND, nr_sf32);
  }
  else {
    init_solver_field(&solver_field, VOLUMEPLUSRAND/2, nr_sf);
    init_solver_field_32(&solver_field32, VOLUMEPLUSRAND/2, nr_sf32);    
  }

  atime = gettime();

  // we could get away with using fewer fields, of course
  phigh = solver_field[3];
  xhigh = solver_field[2];
  rhigh = solver_field[1];
  qhigh = solver_field[0];

  x = solver_field32[3];
  r = solver_field32[2];
  p = solver_field32[1];
  q = solver_field32[0];

  // we always want to apply the full precision operator
  int save_sloppy = g_sloppy_precision_flag;
  g_sloppy_precision_flag = 0;

  sourcesquarenorm = square_norm(Q,N,1);
  if( rel_prec == 1 ) {
    target_eps_sq = eps_sq*sourcesquarenorm;
    if(g_debug_level > 0 && g_proc_id==0) 
      printf("#RG_Mixed CG: Using relative precision! eps_sq: %.6g target_eps_sq: %.6g \n",eps_sq,target_eps_sq);
  }else{
    target_eps_sq = eps_sq;
  }
  
  // compute the maximum number of maximum iterations based on the expected reduction
  // of the residual at each run of the inner solver
  int N_outer = (int)ceil(log10( delta/eps_sq ))+4; // +4 is an arbitrary choice which seems to avoid switching to DP
  if(g_debug_level > 0 && g_proc_id==0) 
    printf("#RG_Mixed CG: N_outer: %d \n", N_outer);
  
  // should compute real residual here, for now we always use a zero guess
  zero_spinor_field_32(x,N);
  zero_spinor_field(P,N);
  assign(phigh,Q,N);
  assign(rhigh,Q,N);
  
  rho_dp = square_norm(rhigh,N,1);
  assign_to_32(r,rhigh,N);
  rho_sp = rho_dp;
  assign_32(p,r,N);
  
  iter_in_sp += inner_loop(x, p, q, r, &rho_sp, delta, f32, (float)target_eps_sq, 
                           N, iter_out+iter_in_sp+iter_in_dp, 0.0, 0.0, 0, PR);

  for(i = 0; i < N_outer; i++) {
     
    ++iter_out;

    // prepare for defect correction
    // update high precision solution 
    if(high_control==0) {
      // accumulate solution (sp -> dp) 
      addto_32(P,x,N);
      // compute real residual
      f(qhigh,P);
      diff(rhigh,Q,qhigh,N);
      beta_dp = 1/rho_dp;
      rho_dp = square_norm(rhigh,N,1);
      beta_dp *= rho_dp;
    }
    
    // the iteration limit was reached in the previous iteration, let's try to save the day using double precision
    if( high_control==2 ) {
      assign(phigh,rhigh,N);
      zero_spinor_field(xhigh,N);
      beta_dp = 1/rho_dp;
      iter_in_dp += inner_loop_high(xhigh, phigh, qhigh, rhigh, &rho_dp, delta, f, 
                                    target_eps_sq, N, iter_out+iter_in_sp+iter_in_dp);
      rho_sp = rho_dp;
      // accumulate solution
      add(P,P,xhigh,N);
      // compute real residual
      f(qhigh,P);
      diff(rhigh,Q,qhigh,N);
      rho_dp = square_norm(rhigh,N,1);
      beta_dp *= rho_dp;
      high_control = 1;
    }

    if(g_debug_level > 2 && g_proc_id == 0) {
      printf("RG_mixed CG last inner residue:       %17g\n", rho_sp);
      printf("RG_mixed CG true residue:             %6d %10g\n", iter_in_sp+iter_in_dp+iter_out, rho_dp);
      printf("RG_mixed CG residue reduction factor: %6d %10g\n", iter_in_sp+iter_in_dp+iter_out, beta_dp); fflush(stdout);
    }
    if( rho_dp <= target_eps_sq ) {
      etime = gettime();
      output_flops(etime-atime, N, iter_out, iter_in_sp, iter_in_dp, eps_sq);
      
      g_sloppy_precision_flag = save_sloppy;
      finalize_solver(solver_field, nr_sf);
      finalize_solver_32(solver_field32, nr_sf32); 
      return(iter_in_sp+iter_in_dp+iter_out);
    }

    // if it seems like we're stuck or reaching the iteration limit, we skip this correction and proceed in full precision above
    if( i >= (N_outer-3) ){
      if(g_proc_id==0) printf("mixed CG: Reaching iteration limit, switching to DP!\n");
      high_control = 2;
      continue;
    }else{
      // correct defect
      assign_to_32(r,rhigh,N);
      rho_sp = rho_dp; // not sure if it's fine to truncate this or whether one should calculate it in SP directly

      // if the inner loop was successful at reducing the residual, reuse the search direction to some extent
      if(beta_dp < 1.0) {
        // if we just came out of a high precision loop (high_control==1), phigh currently contains the search vector
        // and there is no need to copy it over
        if(high_control==0){
          assign_to_64(phigh,p,N);
        }else{
          high_control = 0;
        }
        // attempt to make a new search vector, as conjugate as possible to the previous one
        // trying to enforce conjugacy (phigh^+ M p) = 0 directly by computing p' = rhigh - (rhigh^+ M p) / (p^+ M p) * p
        // does not work in general due to limited precision (it fails on lattices L>24)
        assign_mul_add_r(phigh,beta_dp,rhigh,N);
        assign_to_32(p,phigh,N);
      }else{
        assign_to_32(p,rhigh,N);
      }
    }

    zero_spinor_field_32(x,N);
    iter_in_sp += inner_loop(x, p, q, r, &rho_sp, delta, f32, (float)target_eps_sq, 
                             N, iter_out+iter_in_sp+iter_in_dp, 0.0, 0.0, 0, PR);
  }
  
  // convergence failure...
  g_sloppy_precision_flag = save_sloppy;
  finalize_solver(solver_field, nr_sf);
  finalize_solver_32(solver_field32, nr_sf32);
  return -1; 
}

void output_flops(const double seconds, const unsigned int N, const unsigned int iter_out, const unsigned int iter_in_sp, const unsigned int iter_in_dp, const double eps_sq){
  double flops;
  // TODO: compute real number of flops...
  int total_it = iter_in_sp+iter_in_dp+iter_out;
  if(g_debug_level > 0 && g_proc_id == 0) {
    printf("# mixed CG: iter_out: %d iter_in_sp: %d iter_in_dp: %d\n",iter_out,iter_in_sp,iter_in_dp);
    printf("# FIXME: note the following flop counts are wrong!\n");
  	if(N != VOLUME){
  	  /* 2 A + 2 Nc Ns + N_Count ( 2 A + 10 Nc Ns ) */
  	  /* 2*1608.0 because the linalg is over VOLUME/2 */
  	  flops = (2*(2*1608.0+2*3*4) + 2*3*4 + total_it*(2.*(2*1608.0+2*3*4) + 10*3*4))*N/1.0e6f;
  	}
  	else{
  	  /* 2 A + 2 Nc Ns + N_Count ( 2 A + 10 Nc Ns ) */
  	  flops = (2*(1608.0+2*3*4) + 2*3*4 + total_it*(2.*(1608.0+2*3*4) + 10*3*4))*N/1.0e6f;      
  	}
  	printf("#RG_mixed CG: iter: %d eps_sq: %1.4e t/s: %1.4e\n", total_it, eps_sq, seconds); 
  	printf("#RG_mixed CG: flopcount (for e/o tmWilson only): t/s: %1.4e mflops_local: %.1f mflops: %.1f\n", 
  	      seconds, flops/(seconds), g_nproc*flops/(seconds));
  }      
}
