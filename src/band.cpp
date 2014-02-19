#include "band.h"

namespace sirius {

void Band::apply_magnetic_field(mdarray<double_complex, 2>& fv_states, int mtgk_size, int num_gkvec, int* fft_index, 
                                Periodic_function<double>* effective_magnetic_field[3], mdarray<double_complex, 3>& hpsi)
{
    assert(hpsi.size(2) >= 2);
    assert(fv_states.size(0) == hpsi.size(0));
    assert(fv_states.size(1) == hpsi.size(1));

    Timer t("sirius::Band::apply_magnetic_field");

    double_complex zzero = double_complex(0, 0);
    double_complex zone = double_complex(1, 0);
    double_complex zi = double_complex(0, 1);

    mdarray<double_complex, 3> zm(parameters_.unit_cell()->max_mt_basis_size(), parameters_.unit_cell()->max_mt_basis_size(), 
                             parameters_.num_mag_dims());

    // column fv states are further distributed over rows to make use of all row processors
    int num_fv_local = parameters_.sub_spl_fv_states_col().local_size();
    int idx_fv_local = parameters_.sub_spl_fv_states_col().global_offset();
            
    for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
    {
        int offset = parameters_.unit_cell()->atom(ia)->offset_wf();
        int mt_basis_size = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();
        Atom* atom = parameters_.unit_cell()->atom(ia);
        
        zm.zero();

        #pragma omp parallel for default(shared)
        for (int j2 = 0; j2 < mt_basis_size; j2++)
        {
            int lm2 = atom->type()->indexb(j2).lm;
            int idxrf2 = atom->type()->indexb(j2).idxrf;
            
            for (int i = 0; i < parameters_.num_mag_dims(); i++)
            {
                for (int j1 = 0; j1 <= j2; j1++)
                {
                    int lm1 = atom->type()->indexb(j1).lm;
                    int idxrf1 = atom->type()->indexb(j1).idxrf;

                    zm(j1, j2, i) = gaunt_coefs_->sum_L3_gaunt(lm1, lm2, atom->b_radial_integrals(idxrf1, idxrf2, i)); 
                }
            }
        }
        // compute bwf = B_z*|wf_j>
        blas<cpu>::hemm(0, 0, mt_basis_size, num_fv_local, zone, &zm(0, 0, 0), zm.ld(), 
                        &fv_states(offset, idx_fv_local), fv_states.ld(), zzero, 
                        &hpsi(offset, idx_fv_local, 0), hpsi.ld());
        
        // compute bwf = (B_x - iB_y)|wf_j>
        if (hpsi.size(2) >= 3)
        {
            // reuse first (z) component of zm matrix to store (Bx - iBy)
            for (int j2 = 0; j2 < mt_basis_size; j2++)
            {
                for (int j1 = 0; j1 <= j2; j1++) zm(j1, j2, 0) = zm(j1, j2, 1) - zi * zm(j1, j2, 2);
                
                for (int j1 = j2 + 1; j1 < mt_basis_size; j1++) zm(j1, j2, 0) = conj(zm(j2, j1, 1)) - zi * conj(zm(j2, j1, 2));
            }
              
            blas<cpu>::gemm(0, 0, mt_basis_size, num_fv_local, mt_basis_size, &zm(0, 0, 0), zm.ld(), 
                            &fv_states(offset, idx_fv_local), fv_states.ld(), 
                            &hpsi(offset, idx_fv_local, 2), hpsi.ld());
        }
        
        // compute bwf = (B_x + iB_y)|wf_j>
        if ((hpsi.size(2) == 4) && 
            (parameters_.eigen_value_solver() == scalapack || parameters_.eigen_value_solver() == elpa))
        {
            // reuse first (z) component of zm matrix to store (Bx + iBy)
            for (int j2 = 0; j2 < mt_basis_size; j2++)
            {
                for (int j1 = 0; j1 <= j2; j1++) zm(j1, j2, 0) = zm(j1, j2, 1) + zi * zm(j1, j2, 2);
                
                for (int j1 = j2 + 1; j1 < mt_basis_size; j1++) zm(j1, j2, 0) = conj(zm(j2, j1, 1)) + zi * conj(zm(j2, j1, 2));
            }
              
            blas<cpu>::gemm(0, 0, mt_basis_size, num_fv_local, mt_basis_size, &zm(0, 0, 0), zm.ld(), 
                            &fv_states(offset, idx_fv_local), fv_states.ld(), 
                            &hpsi(offset, idx_fv_local, 3), hpsi.ld());
        }
    }
    
    Timer *t1 = new Timer("sirius::Band::apply_magnetic_field:it");

    mdarray<double_complex, 3> hpsi_pw(num_gkvec, parameters_.spl_fv_states_col().local_size(), hpsi.size(2));
    hpsi_pw.zero();

    int num_fft_threads = Platform::num_fft_threads();
    #pragma omp parallel default(shared) num_threads(num_fft_threads)
    {        
        int thread_id = omp_get_thread_num();
        
        std::vector<double_complex> psi_it(fft_->size());
        std::vector<double_complex> hpsi_it(fft_->size());
        
        #pragma omp for
        for (int iloc = 0; iloc < num_fv_local; iloc++)
        {
            int i = parameters_.sub_spl_fv_states_col(iloc);

            fft_->input(num_gkvec, fft_index, &fv_states(parameters_.unit_cell()->mt_basis_size(), i), thread_id);
            fft_->transform(1, thread_id);
            fft_->output(&psi_it[0], thread_id);
                                        
            for (int ir = 0; ir < fft_->size(); ir++)
            {
                // hpsi(r) = psi(r) * Bz(r) * Theta(r)
                hpsi_it[ir] = psi_it[ir] * effective_magnetic_field[0]->f_it<global>(ir) * parameters_.step_function(ir);
            }
            
            fft_->input(&hpsi_it[0], thread_id);
            fft_->transform(-1, thread_id);
            fft_->output(num_gkvec, fft_index, &hpsi_pw(0, i, 0), thread_id); 

            if (hpsi.size(2) >= 3)
            {
                for (int ir = 0; ir < fft_->size(); ir++)
                {
                    // hpsi(r) = psi(r) * (Bx(r) - iBy(r)) * Theta(r)
                    hpsi_it[ir] = psi_it[ir] * parameters_.step_function(ir) * 
                                  (effective_magnetic_field[1]->f_it<global>(ir) - 
                                   zi * effective_magnetic_field[2]->f_it<global>(ir));
                }
                
                fft_->input(&hpsi_it[0], thread_id);
                fft_->transform(-1, thread_id);
                fft_->output(num_gkvec, fft_index, &hpsi_pw(0, i, 2), thread_id); 
            }
            
            if ((hpsi.size(2)) == 4 && 
                (parameters_.eigen_value_solver() == scalapack || parameters_.eigen_value_solver() == elpa))
            {
                for (int ir = 0; ir < fft_->size(); ir++)
                {
                    // hpsi(r) = psi(r) * (Bx(r) + iBy(r)) * Theta(r)
                    hpsi_it[ir] = psi_it[ir] * parameters_.step_function(ir) *
                                  (effective_magnetic_field[1]->f_it<global>(ir) + 
                                   zi * effective_magnetic_field[2]->f_it<global>(ir));
                }
                
                fft_->input(&hpsi_it[0], thread_id);
                fft_->transform(-1, thread_id);
                fft_->output(num_gkvec, fft_index, &hpsi_pw(0, i, 3), thread_id); 
            }
        }
    }
    delete t1;

    for (int n = 0; n < hpsi.size(2); n++)
    {
        if (n != 1)
        {
            for (int iloc = 0; iloc < num_fv_local; iloc++)
            {
                int i = parameters_.sub_spl_fv_states_col(iloc);
                memcpy(&hpsi(parameters_.unit_cell()->mt_basis_size(), i, n), &hpsi_pw(0, i, n), num_gkvec * sizeof(double_complex));
            }
        }
        else
        {
            // copy Bz|\psi> to -Bz|\psi>
            for (int iloc = 0; iloc < num_fv_local; iloc++)
            {
                int i = parameters_.sub_spl_fv_states_col(iloc);
                for (int j = 0; j < mtgk_size; j++) hpsi(j, i, 1) = -hpsi(j, i, 0);
            }
        }
    }

    for (int n = 0; n < hpsi.size(2); n++)
    {
        Platform::allgather(&hpsi(0, 0, n), hpsi.size(0) * idx_fv_local, hpsi.size(0) * num_fv_local, 
                            parameters_.mpi_grid().communicator(1 << _dim_row_));
    }
}

void Band::apply_so_correction(mdarray<double_complex, 2>& fv_states, mdarray<double_complex, 3>& hpsi)
{
    Timer t("sirius::Band::apply_so_correction");

    for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
    {
        Atom_type* type = parameters_.unit_cell()->atom(ia)->type();

        int offset = parameters_.unit_cell()->atom(ia)->offset_wf();

        for (int l = 0; l <= parameters_.lmax_apw(); l++)
        {
            int nrf = type->indexr().num_rf(l);

            for (int order1 = 0; order1 < nrf; order1++)
            {
                for (int order2 = 0; order2 < nrf; order2++)
                {
                    double sori = parameters_.unit_cell()->atom(ia)->symmetry_class()->so_radial_integral(l, order1, order2);
                    
                    for (int m = -l; m <= l; m++)
                    {
                        int idx1 = type->indexb_by_l_m_order(l, m, order1);
                        int idx2 = type->indexb_by_l_m_order(l, m, order2);
                        int idx3 = (m + l != 0) ? type->indexb_by_l_m_order(l, m - 1, order2) : 0;
                        int idx4 = (m - l != 0) ? type->indexb_by_l_m_order(l, m + 1, order2) : 0;

                        for (int ist = 0; ist < parameters_.spl_fv_states_col().local_size(); ist++)
                        {
                            double_complex z1 = fv_states(offset + idx2, ist) * double(m) * sori;
                            hpsi(offset + idx1, ist, 0) += z1;
                            hpsi(offset + idx1, ist, 1) -= z1;
                            // apply L_{-} operator
                            if (m + l) hpsi(offset + idx1, ist, 2) += fv_states(offset + idx3, ist) * sori * 
                                                                      sqrt(double(l * (l + 1) - m * (m - 1)));
                            // apply L_{+} operator
                            if (m - l) hpsi(offset + idx1, ist, 3) += fv_states(offset + idx4, ist) * sori * 
                                                                      sqrt(double(l * (l + 1) - m * (m + 1)));
                        }
                    }
                }
            }
        }
    }
}

void Band::solve_sv(K_point* kp, Periodic_function<double>* effective_magnetic_field[3])
{
    log_function_enter(__func__);

    Timer t("sirius::Band::solve_sv");

    if (!parameters_.need_sv())
    {
        kp->bypass_sv();
        return;
    }
    
    if (kp->num_ranks() > 1 && (parameters_.eigen_value_solver() == lapack || parameters_.eigen_value_solver() == magma))
        error_local(__FILE__, __LINE__, "Can't use more than one MPI rank for LAPACK or MAGMA eigen-value solver");

    // number of h|\psi> components 
    int nhpsi = parameters_.num_mag_dims() + 1;

    int nrow = parameters_.spl_fv_states_row().local_size();
    int ncol = parameters_.spl_fv_states_col().local_size();
    int fvsz = kp->wf_size();

    mdarray<double_complex, 2>& fv_states_row = kp->fv_states_row();
    mdarray<double_complex, 2>& sv_eigen_vectors = kp->sv_eigen_vectors();
    std::vector<double> band_energies(parameters_.num_bands());

    // product of the second-variational Hamiltonian and a wave-function
    mdarray<double_complex, 3> hpsi(fvsz, ncol, nhpsi);
    hpsi.zero();

    // compute product of magnetic field and wave-function 
    if (parameters_.num_spins() == 2)
        apply_magnetic_field(kp->fv_states_col(), kp->wf_size(), kp->num_gkvec(), kp->fft_index(), effective_magnetic_field, hpsi);

    if (parameters_.uj_correction())
    {
        apply_uj_correction<uu>(kp->fv_states_col(), hpsi);
        if (parameters_.num_mag_dims() != 0) apply_uj_correction<dd>(kp->fv_states_col(), hpsi);
        if (parameters_.num_mag_dims() == 3) 
        {
            apply_uj_correction<ud>(kp->fv_states_col(), hpsi);
            if (parameters_.eigen_value_solver() == scalapack || parameters_.eigen_value_solver() == elpa)
                apply_uj_correction<du>(kp->fv_states_col(), hpsi);
        }
    }

    if (parameters_.so_correction()) apply_so_correction(kp->fv_states_col(), hpsi);


    standard_evp* solver = NULL;
    switch (parameters_.eigen_value_solver())
    {
        case lapack:
        {
            solver = new standard_evp_lapack();
            break;
        }
        case scalapack:
        {
            solver = new standard_evp_scalapack(parameters_.cyclic_block_size(), kp->num_ranks_row(), 
                                                kp->num_ranks_col(), parameters_.blacs_context());
            break;
        }
        case elpa:
        {
            solver = new standard_evp_scalapack(parameters_.cyclic_block_size(), kp->num_ranks_row(), 
                                                kp->num_ranks_col(), parameters_.blacs_context());
            break;
        }
        case magma:
        {
            solver = new standard_evp_lapack();
            break;
        }
        case plasma:
        {
            solver = new standard_evp_plasma();
            break;
        }
        default:
        {
            error_local(__FILE__, __LINE__, "eigen value solver is not defined");
        }
    }
    
    if (parameters_.num_mag_dims() == 1)
    {
        mdarray<double_complex, 2> h(nrow, ncol);
        
        //perform two consecutive diagonalizations
        for (int ispn = 0; ispn < 2; ispn++)
        {
            // compute <wf_i | (h * wf_j)> for up-up or dn-dn block
            blas<cpu>::gemm(2, 0, nrow, ncol, fvsz, &fv_states_row(0, 0), fv_states_row.ld(), 
                            &hpsi(0, 0, ispn), hpsi.ld(), &h(0, 0), h.ld());

            for (int icol = 0; icol < ncol; icol++)
            {
                int i = parameters_.spl_fv_states_col(icol);
                for (int irow = 0; irow < nrow; irow++)
                {
                    if (parameters_.spl_fv_states_row(irow) == i) h(irow, icol) += kp->fv_eigen_value(i);
                }
            }
        
            Timer t1("sirius::Band::solve_sv|stdevp");
            solver->solve(parameters_.num_fv_states(), h.get_ptr(), h.ld(),
                          &band_energies[ispn * parameters_.num_fv_states()],
                          &sv_eigen_vectors(0, ispn * ncol), sv_eigen_vectors.ld());
        }
    }

    if (parameters_.num_mag_dims() == 3)
    {
        mdarray<double_complex, 2> h(2 * nrow, 2 * ncol);
        h.zero();

        // compute <fv_i | (h * fv_j)> for up-up block
        blas<cpu>::gemm(2, 0, nrow, ncol, fvsz, &fv_states_row(0, 0), fv_states_row.ld(), &hpsi(0, 0, 0), hpsi.ld(), 
                        &h(0, 0), h.ld());

        // compute <fv_i | (h * fv_j)> for up-dn block
        blas<cpu>::gemm(2, 0, nrow, ncol, fvsz, &fv_states_row(0, 0), fv_states_row.ld(), &hpsi(0, 0, 2), hpsi.ld(), 
                        &h(0, ncol), h.ld());
       
        // compute <fv_i | (h * fv_j)> for dn-dn block
        blas<cpu>::gemm(2, 0, nrow, ncol, fvsz, &fv_states_row(0, 0), fv_states_row.ld(), &hpsi(0, 0, 1), hpsi.ld(), 
                        &h(nrow, ncol), h.ld());

        if (parameters_.eigen_value_solver() == scalapack || parameters_.eigen_value_solver() == elpa)
        {
            // compute <fv_i | (h * fv_j)> for dn-up block
            blas<cpu>::gemm(2, 0, nrow, ncol, fvsz, &fv_states_row(0, 0), fv_states_row.ld(), &hpsi(0, 0, 3), hpsi.ld(), 
                            &h(nrow, 0), h.ld());
        }
      
        for (int ispn = 0; ispn < 2; ispn++)
        {
            for (int icol = 0; icol < ncol; icol++)
            {
                int i = parameters_.spl_fv_states_col(icol) + ispn * parameters_.num_fv_states();
                for (int irow = 0; irow < nrow; irow++)
                {
                    int j = parameters_.spl_fv_states_row(irow) + ispn * parameters_.num_fv_states();
                    if (j == i) 
                    {
                        h(irow + ispn * nrow, icol + ispn * ncol) += kp->fv_eigen_value(parameters_.spl_fv_states_col(icol));
                    }
                }
            }
        }
    
        Timer t1("sirius::Band::solve_sv|stdevp");
        solver->solve(parameters_.num_bands(), h.get_ptr(), h.ld(), &band_energies[0], 
                      sv_eigen_vectors.get_ptr(), sv_eigen_vectors.ld());
    }
    delete solver;

    kp->set_band_energies(&band_energies[0]);
    log_function_exit(__func__);
}

//=====================================================================================================================
// CPU code, plane-wave basis
//=====================================================================================================================
//** template<> void Band::set_fv_h_o<cpu, pwlo>(K_point* kp, Periodic_function<double>* effective_potential, 
//**                                             mdarray<double_complex, 2>& h, mdarray<double_complex, 2>& o)
//** {
//**     Timer t("sirius::Band::set_fv_h_o");
//**     
//**     h.zero();
//**     o.zero();
//** 
//**     #pragma omp parallel for default(shared)
//**     for (int igkloc2 = 0; igkloc2 < k->num_gkvec_col(); igkloc2++) // loop over columns
//**     {
//**         for (int igkloc1 = 0; igkloc1 < kp->num_gkvec_row(); igkloc1++) // for each column loop over rows
//**         {
//**             if (kp->apwlo_basis_descriptors_row(igkloc1).idxglob == apwlo_basis_descriptors_col_[igkloc2].idxglob) 
//**             {
//**                 h(igkloc1, igkloc2) = 0.5 * pow(gkvec_len_[igkloc1], 2);
//**                 o(igkloc1, igkloc2) = double_complex(1, 0);
//**             }
//**                                
//**             int ig12 = parameters_.index_g12(apwlo_basis_descriptors_row_[igkloc1].ig,
//**                                              apwlo_basis_descriptors_col_[igkloc2].ig);
//**             h(igkloc1, igkloc2) += effective_potential->f_pw(ig12);
//**         }
//**     }
//**     
//**     set_fv_h_o_pw_lo<cpu>(effective_potential, num_ranks, h, o);
//** 
//**     set_fv_h_o_lo_lo(h, o);
//** }
//**
//** template<> void K_point::set_fv_h_o_pw_lo<cpu>(Periodic_function<double>* effective_potential, int num_ranks, 
//**                                               mdarray<double_complex, 2>& h, mdarray<double_complex, 2>& o)
//** {
//**     Timer t("sirius::K_point::set_fv_h_o_pw_lo");
//**     
//**     int offset_col = (num_ranks > 1) ? num_gkvec_row() : 0;
//**     
//**     mdarray<Spline<double_complex>*, 2> svlo(parameters_.lmmax_pw(), std::max(num_lo_col(), num_lo_row()));
//** 
//**     // first part: compute <G+k|H|lo> and <G+k|lo>
//** 
//**     Timer t1("sirius::K_point::set_fv_h_o_pw_lo:vlo", false);
//**     Timer t2("sirius::K_point::set_fv_h_o_pw_lo:ohk", false);
//**     Timer t3("sirius::K_point::set_fv_h_o_pw_lo:hvlo", false);
//** 
//**     // compute V|lo>
//**     t1.start();
//**     for (int icol = 0; icol < num_lo_col(); icol++)
//**     {
//**         int ia = apwlo_basis_descriptors_col_[num_gkvec_col() + icol].ia;
//**         int lm = apwlo_basis_descriptors_col_[num_gkvec_col() + icol].lm;
//**         int idxrf = apwlo_basis_descriptors_col_[num_gkvec_col() + icol].idxrf;
//**         
//**         for (int lm1 = 0; lm1 < parameters_.lmmax_pw(); lm1++)
//**         {
//**             svlo(lm1, icol) = new Spline<double_complex>(parameters_.atom(ia)->num_mt_points(), 
//**                                                     parameters_.atom(ia)->radial_grid());
//** 
//**             for (int k = 0; k < parameters_.gaunt().complex_gaunt_packed_L3_size(lm1, lm); k++)
//**             {
//**                 int lm3 = parameters_.gaunt().complex_gaunt_packed_L3(lm1, lm, k).lm3;
//**                 double_complex cg = parameters_.gaunt().complex_gaunt_packed_L3(lm1, lm, k).cg;
//** 
//**                 for (int ir = 0; ir < parameters_.atom(ia)->num_mt_points(); ir++)
//**                 {
//**                     (*svlo(lm1, icol))[ir] += (cg * effective_potential->f_mt<global>(lm3, ir, ia) * 
//**                                                parameters_.atom(ia)->symmetry_class()->radial_function(ir, idxrf));
//**                 }
//**             }
//** 
//**             svlo(lm1, icol)->interpolate();
//**         }
//**     }
//**     t1.stop();
//**     
//**     t2.start();
//**     // compute overlap and kinetic energy
//**     for (int icol = num_gkvec_col(); icol < apwlo_basis_size_col(); icol++)
//**     {
//**         int ia = apwlo_basis_descriptors_col_[icol].ia;
//**         int iat = parameters_.atom_type_index_by_id(parameters_.atom(ia)->type_id());
//** 
//**         int l = apwlo_basis_descriptors_col_[icol].l;
//**         int lm = apwlo_basis_descriptors_col_[icol].lm;
//**         int idxrf = apwlo_basis_descriptors_col_[icol].idxrf;
//** 
//**         Spline<double> slo(parameters_.atom(ia)->num_mt_points(), parameters_.atom(ia)->radial_grid());
//**         for (int ir = 0; ir < slo.num_points(); ir++)
//**             slo[ir] = parameters_.atom(ia)->symmetry_class()->radial_function(ir, idxrf);
//**         slo.interpolate();
//**         
//**         #pragma omp parallel for default(shared)
//**         for (int igkloc = 0; igkloc < num_gkvec_row(); igkloc++)
//**         {
//**             o(igkloc, icol) = (fourpi / sqrt(parameters_.omega())) * conj(zil_[l]) * gkvec_ylm_(lm, igkloc) * 
//**                               Spline<double>::integrate(&slo, (*sbessel_[igkloc])(l, iat)) * 
//**                               conj(gkvec_phase_factors_(igkloc, ia));
//** 
//**             // kinetic part <G+k| -1/2 \nabla^2 |lo> = 1/2 |G+k|^2 <G+k|lo>
//**             h(igkloc, icol) = 0.5 * pow(gkvec_len_[igkloc], 2) * o(igkloc, icol);
//**         }
//**     }
//**     t2.stop();
//** 
//**     t3.start();
//**     #pragma omp parallel for default(shared)
//**     for (int igkloc = 0; igkloc < num_gkvec_row(); igkloc++)
//**     {
//**         for (int icol = num_gkvec_col(); icol < apwlo_basis_size_col(); icol++)
//**         {
//**             int ia = apwlo_basis_descriptors_col_[icol].ia;
//**             int iat = parameters_.atom_type_index_by_id(parameters_.atom(ia)->type_id());
//** 
//**             //int l = apwlo_basis_descriptors_col_[icol].l;
//**             //int lm = apwlo_basis_descriptors_col_[icol].lm;
//**             //int idxrf = apwlo_basis_descriptors_col_[icol].idxrf;
//** 
//**             //*// compue overlap <G+k|lo>
//**             //*Spline<double> s(parameters_.atom(ia)->num_mt_points(), parameters_.atom(ia)->radial_grid());
//**             //*for (int ir = 0; ir < s.num_points(); ir++)
//**             //*{
//**             //*    s[ir] = (*sbessel_[igkloc])(ir, l, iat) * 
//**             //*            parameters_.atom(ia)->symmetry_class()->radial_function(ir, idxrf);
//**             //*}
//**             //*s.interpolate();
//**             //*    
//**             //*o(igkloc, icol) = (fourpi / sqrt(parameters_.omega())) * conj(zil_[l]) * gkvec_ylm_(lm, igkloc) * 
//**             //*                  s.integrate(2) * conj(gkvec_phase_factors_(igkloc, ia));
//** 
//**             //*// kinetic part <G+k| -1/2 \nabla^2 |lo> = 1/2 |G+k|^2 <G+k|lo>
//**             //*h(igkloc, icol) = 0.5 * pow(gkvec_len_[igkloc], 2) * o(igkloc, icol);
//** 
//**             // add <G+k|V|lo>
//**             double_complex zt1(0, 0);
//**             for (int l1 = 0; l1 <= parameters_.lmax_pw(); l1++)
//**             {
//**                 for (int m1 = -l1; m1 <= l1; m1++)
//**                 {
//**                     int lm1 = Utils::lm_by_l_m(l1, m1);
//** 
//**                     zt1 += Spline<double_complex>::integrate(svlo(lm1, icol - num_gkvec_col()), 
//**                                                         (*sbessel_[igkloc])(l1, iat)) * 
//**                            conj(zil_[l1]) * gkvec_ylm_(lm1, igkloc);
//**                 }
//**             }
//**             zt1 *= ((fourpi / sqrt(parameters_.omega())) * conj(gkvec_phase_factors_(igkloc, ia)));
//**             h(igkloc, icol) += zt1;
//**         }
//**     }
//**     t3.stop();
//**    
//**     // deallocate V|lo>
//**     for (int icol = 0; icol < num_lo_col(); icol++)
//**     {
//**         for (int lm = 0; lm < parameters_.lmmax_pw(); lm++) delete svlo(lm, icol);
//**     }
//** 
//**     // restore the <lo|H|G+k> and <lo|G+k> bocks and exit
//**     if (num_ranks == 1)
//**     {
//**         for (int igkloc = 0; igkloc < num_gkvec_col(); igkloc++)
//**         {
//**             for (int irow = num_gkvec_row(); irow < apwlo_basis_size_row(); irow++)
//**             {
//**                 h(irow, igkloc) = conj(h(igkloc, irow));
//**                 o(irow, igkloc) = conj(o(igkloc, irow));
//**             }
//**         }
//**         return;
//**     }
//** 
//**     // second part: compute <lo|H|G+k> and <lo|G+k>
//** 
//**     // compute V|lo>
//**     t1.start();
//**     for (int irow = 0; irow < num_lo_row(); irow++)
//**     {
//**         int ia = apwlo_basis_descriptors_row_[num_gkvec_row() + irow].ia;
//**         int lm = apwlo_basis_descriptors_row_[num_gkvec_row() + irow].lm;
//**         int idxrf = apwlo_basis_descriptors_row_[num_gkvec_row() + irow].idxrf;
//**         
//**         for (int lm1 = 0; lm1 < parameters_.lmmax_pw(); lm1++)
//**         {
//**             svlo(lm1, irow) = new Spline<double_complex>(parameters_.atom(ia)->num_mt_points(), 
//**                                                     parameters_.atom(ia)->radial_grid());
//** 
//**             for (int k = 0; k < parameters_.gaunt().complex_gaunt_packed_L3_size(lm1, lm); k++)
//**             {
//**                 int lm3 = parameters_.gaunt().complex_gaunt_packed_L3(lm1, lm, k).lm3;
//**                 double_complex cg = parameters_.gaunt().complex_gaunt_packed_L3(lm1, lm, k).cg;
//** 
//**                 for (int ir = 0; ir < parameters_.atom(ia)->num_mt_points(); ir++)
//**                 {
//**                     (*svlo(lm1, irow))[ir] += (cg * effective_potential->f_mt<global>(lm3, ir, ia) * 
//**                                                parameters_.atom(ia)->symmetry_class()->radial_function(ir, idxrf));
//**                 }
//**             }
//** 
//**             svlo(lm1, irow)->interpolate();
//**         }
//**     }
//**     t1.stop();
//**    
//**     t2.start();
//**     for (int irow = num_gkvec_row(); irow < apwlo_basis_size_row(); irow++)
//**     {
//**         int ia = apwlo_basis_descriptors_row_[irow].ia;
//**         int iat = parameters_.atom_type_index_by_id(parameters_.atom(ia)->type_id());
//** 
//**         int l = apwlo_basis_descriptors_row_[irow].l;
//**         int lm = apwlo_basis_descriptors_row_[irow].lm;
//**         int idxrf = apwlo_basis_descriptors_row_[irow].idxrf;
//** 
//**         // compue overlap <lo|G+k>
//**         Spline<double> slo(parameters_.atom(ia)->num_mt_points(), parameters_.atom(ia)->radial_grid());
//**         for (int ir = 0; ir < slo.num_points(); ir++)
//**             slo[ir] = parameters_.atom(ia)->symmetry_class()->radial_function(ir, idxrf);
//**         slo.interpolate();
//**         
//**         #pragma omp parallel for default(shared)
//**         for (int igkloc = 0; igkloc < num_gkvec_col(); igkloc++)
//**         {
//**             o(irow, igkloc) = (fourpi / sqrt(parameters_.omega())) * zil_[l] * 
//**                               conj(gkvec_ylm_(lm, offset_col + igkloc)) * 
//**                               Spline<double>::integrate(&slo, (*sbessel_[offset_col + igkloc])(l, iat)) * 
//**                               gkvec_phase_factors_(offset_col + igkloc, ia);
//** 
//**             // kinetic part <li| -1/2 \nabla^2 |G+k> = 1/2 |G+k|^2 <lo|G+k>
//**             h(irow, igkloc) = 0.5 * pow(gkvec_len_[offset_col + igkloc], 2) * o(irow, igkloc);
//**         }
//**     }
//**     t2.stop();
//** 
//**     t3.start();
//**     #pragma omp parallel for default(shared)
//**     for (int igkloc = 0; igkloc < num_gkvec_col(); igkloc++)
//**     {
//**         for (int irow = num_gkvec_row(); irow < apwlo_basis_size_row(); irow++)
//**         {
//**             int ia = apwlo_basis_descriptors_row_[irow].ia;
//**             int iat = parameters_.atom_type_index_by_id(parameters_.atom(ia)->type_id());
//** 
//**             //int l = apwlo_basis_descriptors_row_[irow].l;
//**             //int lm = apwlo_basis_descriptors_row_[irow].lm;
//**             //int idxrf = apwlo_basis_descriptors_row_[irow].idxrf;
//** 
//**             //*// compue overlap <lo|G+k>
//**             //*Spline<double> s(parameters_.atom(ia)->num_mt_points(), parameters_.atom(ia)->radial_grid());
//**             //*for (int ir = 0; ir < s.num_points(); ir++)
//**             //*    s[ir] = (*sbessel_[offset_col + igkloc])(ir, l, iat) * 
//**             //*            parameters_.atom(ia)->symmetry_class()->radial_function(ir, idxrf);
//**             //*s.interpolate();
//**             //*    
//**             //*o(irow, igkloc) = (fourpi / sqrt(parameters_.omega())) * zil_[l] * 
//**             //*                  conj(gkvec_ylm_(lm, offset_col + igkloc)) * s.integrate(2) * 
//**             //*                  gkvec_phase_factors_(offset_col + igkloc, ia);
//** 
//**             //*// kinetic part <li| -1/2 \nabla^2 |G+k> = 1/2 |G+k|^2 <lo|G+k>
//**             //*h(irow, igkloc) = 0.5 * pow(gkvec_len_[offset_col + igkloc], 2) * o(irow, igkloc);
//** 
//**             // add <lo|V|G+k>
//**             double_complex zt1(0, 0);
//**             for (int l1 = 0; l1 <= parameters_.lmax_pw(); l1++)
//**             {
//**                 for (int m1 = -l1; m1 <= l1; m1++)
//**                 {
//**                     int lm1 = Utils::lm_by_l_m(l1, m1);
//** 
//**                     zt1 += conj(Spline<double_complex>::integrate(svlo(lm1, irow - num_gkvec_row()), 
//**                                                              (*sbessel_[offset_col + igkloc])(l1, iat))) * 
//**                            zil_[l1] * conj(gkvec_ylm_(lm1, offset_col + igkloc));
//**                 }
//**             }
//**             zt1 *= ((fourpi / sqrt(parameters_.omega())) * gkvec_phase_factors_(offset_col + igkloc, ia));
//**             h(irow, igkloc) += zt1;
//**         }
//**     }
//**     t3.stop();
//**     
//**     for (int irow = 0; irow < num_lo_row(); irow++)
//**     {
//**         for (int lm = 0; lm < parameters_.lmmax_pw(); lm++) delete svlo(lm, irow);
//**     }
//** }
//** 
//** template<> void K_point::set_fv_h_o<cpu, pwlo>(Periodic_function<double>* effective_potential, int num_ranks,
//**                                               mdarray<double_complex, 2>& h, mdarray<double_complex, 2>& o)
//** {
//**     Timer t("sirius::K_point::set_fv_h_o");
//**     
//**     h.zero();
//**     o.zero();
//** 
//**     #pragma omp parallel for default(shared)
//**     for (int igkloc2 = 0; igkloc2 < num_gkvec_col(); igkloc2++) // loop over columns
//**     {
//**         for (int igkloc1 = 0; igkloc1 < num_gkvec_row(); igkloc1++) // for each column loop over rows
//**         {
//**             if (apwlo_basis_descriptors_row_[igkloc1].idxglob == apwlo_basis_descriptors_col_[igkloc2].idxglob) 
//**             {
//**                 h(igkloc1, igkloc2) = 0.5 * pow(gkvec_len_[igkloc1], 2);
//**                 o(igkloc1, igkloc2) = double_complex(1, 0);
//**             }
//**                                
//**             int ig12 = parameters_.index_g12(apwlo_basis_descriptors_row_[igkloc1].ig,
//**                                              apwlo_basis_descriptors_col_[igkloc2].ig);
//**             h(igkloc1, igkloc2) += effective_potential->f_pw(ig12);
//**         }
//**     }
//**     
//**     set_fv_h_o_pw_lo<cpu>(effective_potential, num_ranks, h, o);
//** 
//**     set_fv_h_o_lo_lo(h, o);
//** }
//** 
//** 


//=====================================================================================================================
// GPU code, plane-wave basis
//=====================================================================================================================
//** template<> void K_point::set_fv_h_o<gpu, pwlo>(Periodic_function<double>* effective_potential, int num_ranks,
//**                                               mdarray<double_complex, 2>& h, mdarray<double_complex, 2>& o)
//** {
//**     Timer t("sirius::K_point::set_fv_h_o");
//**     
//**     h.zero();
//**     o.zero();
//** 
//**     #pragma omp parallel for default(shared)
//**     for (int igkloc2 = 0; igkloc2 < num_gkvec_col(); igkloc2++) // loop over columns
//**     {
//**         for (int igkloc1 = 0; igkloc1 < num_gkvec_row(); igkloc1++) // for each column loop over rows
//**         {
//**             if (apwlo_basis_descriptors_row_[igkloc1].idxglob == apwlo_basis_descriptors_col_[igkloc2].idxglob) 
//**             {
//**                 h(igkloc1, igkloc2) = 0.5 * pow(gkvec_len_[igkloc1], 2);
//**                 o(igkloc1, igkloc2) = double_complex(1, 0);
//**             }
//**                                
//**             int ig12 = parameters_.index_g12(apwlo_basis_descriptors_row_[igkloc1].ig,
//**                                              apwlo_basis_descriptors_col_[igkloc2].ig);
//**             h(igkloc1, igkloc2) += effective_potential->f_pw(ig12);
//**         }
//**     }
//**     
//**     set_fv_h_o_pw_lo<gpu>(effective_potential, num_ranks, h, o);
//** 
//**     set_fv_h_o_lo_lo(h, o);
//** }
//** #endif
//** 
//** template<> void K_point::set_fv_h_o_pw_lo<gpu>(Periodic_function<double>* effective_potential, int num_ranks, 
//**                                               mdarray<double_complex, 2>& h, mdarray<double_complex, 2>& o)
//** {
//**     Timer t("sirius::K_point::set_fv_h_o_pw_lo");
//**     
//**     // ===========================================
//**     // first part: compute <G+k|H|lo> and <G+k|lo>
//**     // ===========================================
//** 
//**     Timer t1("sirius::K_point::set_fv_h_o_pw_lo:vlo", false);
//**     Timer t2("sirius::K_point::set_fv_h_o_pw_lo:ohk", false);
//**     Timer t3("sirius::K_point::set_fv_h_o_pw_lo:hvlo", false);
//** 
//**     mdarray<int, 1> kargs(4);
//**     kargs(0) = parameters_.num_atom_types();
//**     kargs(1) = parameters_.max_num_mt_points();
//**     kargs(2) = parameters_.lmax_pw();
//**     kargs(3) = parameters_.lmmax_pw();
//**     kargs.allocate_on_device();
//**     kargs.copy_to_device();
//** 
//**     // =========================
//**     // compute V|lo> for columns
//**     // =========================
//**     t1.start();
//**     mdarray<double_complex, 3> vlo_coefs(parameters_.max_num_mt_points() * 4, parameters_.lmmax_pw(), num_lo_col());
//**     #pragma omp parallel for default(shared)
//**     for (int icol = 0; icol < num_lo_col(); icol++)
//**     {
//**         int ia = apwlo_basis_descriptors_col_[num_gkvec_col() + icol].ia;
//**         int lm = apwlo_basis_descriptors_col_[num_gkvec_col() + icol].lm;
//**         int idxrf = apwlo_basis_descriptors_col_[num_gkvec_col() + icol].idxrf;
//**         
//**         for (int lm1 = 0; lm1 < parameters_.lmmax_pw(); lm1++)
//**         {
//**             Spline<double_complex> s(parameters_.atom(ia)->num_mt_points(), parameters_.atom(ia)->radial_grid());
//** 
//**             for (int k = 0; k < parameters_.gaunt().complex_gaunt_packed_L3_size(lm1, lm); k++)
//**             {
//**                 int lm3 = parameters_.gaunt().complex_gaunt_packed_L3(lm1, lm, k).lm3;
//**                 double_complex cg = parameters_.gaunt().complex_gaunt_packed_L3(lm1, lm, k).cg;
//** 
//**                 for (int ir = 0; ir < parameters_.atom(ia)->num_mt_points(); ir++)
//**                 {
//**                     s[ir] += (cg * effective_potential->f_rlm(lm3, ir, ia) * 
//**                               parameters_.atom(ia)->symmetry_class()->radial_function(ir, idxrf));
//**                 }
//**             }
//**             s.interpolate();
//**             s.get_coefs(&vlo_coefs(0, lm1, icol), parameters_.max_num_mt_points());
//**         }
//**     }
//**     vlo_coefs.pin_memory();
//**     vlo_coefs.allocate_on_device();
//**     vlo_coefs.async_copy_to_device(-1);
//**     t1.stop();
//**     
//**     // ===========================================
//**     // pack Bessel function splines into one array
//**     // ===========================================
//**     mdarray<double, 4> sbessel_coefs(parameters_.max_num_mt_points() * 4, parameters_.lmax_pw() + 1, 
//**                                      parameters_.num_atom_types(), num_gkvec_row());
//**     for (int igkloc = 0; igkloc < num_gkvec_row(); igkloc++)
//**     {
//**         for (int iat = 0; iat < parameters_.num_atom_types(); iat++)
//**         {
//**             for (int l = 0; l <= parameters_.lmax_pw(); l++)
//**             {
//**                 (*sbessel_[igkloc])(l, iat)->get_coefs(&sbessel_coefs(0, l, iat, igkloc), 
//**                                                        parameters_.max_num_mt_points());
//**             }
//**         }
//**     }
//**     sbessel_coefs.pin_memory();
//**     sbessel_coefs.allocate_on_device();
//**     sbessel_coefs.async_copy_to_device(-1);
//** 
//**     // ==============================
//**     // pack lo splines into one array
//**     // ==============================
//**     mdarray<double, 2> lo_coefs(parameters_.max_num_mt_points() * 4, num_lo_col());
//**     mdarray<int, 1> l_by_ilo(num_lo_col());
//**     mdarray<int, 1> iat_by_ilo(num_lo_col());
//**     for (int icol = 0; icol < num_lo_col(); icol++)
//**     {
//**         int ia = apwlo_basis_descriptors_col_[num_gkvec_col() + icol].ia;
//**         l_by_ilo(icol) = apwlo_basis_descriptors_col_[num_gkvec_col() + icol].l;
//**         iat_by_ilo(icol) = parameters_.atom_type_index_by_id(parameters_.atom(ia)->type_id());
//** 
//**         int idxrf = apwlo_basis_descriptors_col_[num_gkvec_col() + icol].idxrf;
//** 
//**         Spline<double> s(parameters_.atom(ia)->num_mt_points(), parameters_.atom(ia)->radial_grid());
//**         for (int ir = 0; ir < parameters_.atom(ia)->num_mt_points(); ir++)
//**             s[ir] = parameters_.atom(ia)->symmetry_class()->radial_function(ir, idxrf);
//**         s.interpolate();
//**         s.get_coefs(&lo_coefs(0, icol), parameters_.max_num_mt_points());
//**     }
//**     lo_coefs.pin_memory();
//**     lo_coefs.allocate_on_device();
//**     lo_coefs.async_copy_to_device(-1);
//**     l_by_ilo.allocate_on_device();
//**     l_by_ilo.async_copy_to_device(-1);
//**     iat_by_ilo.allocate_on_device();
//**     iat_by_ilo.async_copy_to_device(-1);
//** 
//**     // ============
//**     // radial grids
//**     // ============
//**     mdarray<double, 2> r_dr(parameters_.max_num_mt_points() * 2, parameters_.num_atom_types());
//**     mdarray<int, 1> nmtp_by_iat(parameters_.num_atom_types());
//**     for (int iat = 0; iat < parameters_.num_atom_types(); iat++)
//**     {
//**         nmtp_by_iat(iat) = parameters_.atom_type(iat)->num_mt_points();
//**         parameters_.atom_type(iat)->radial_grid().get_r_dr(&r_dr(0, iat), parameters_.max_num_mt_points());
//**     }
//**     r_dr.allocate_on_device();
//**     r_dr.async_copy_to_device(-1);
//**     nmtp_by_iat.allocate_on_device();
//**     nmtp_by_iat.async_copy_to_device(-1);
//** 
//**     mdarray<double, 2> jlo(num_gkvec_row(), num_lo_col());
//**     jlo.allocate_on_device();
//** 
//**     t2.start();
//**     sbessel_lo_inner_product_gpu(kargs.get_ptr_device(), num_gkvec_row(), num_lo_col(), l_by_ilo.get_ptr_device(), 
//**                                  iat_by_ilo.get_ptr_device(), nmtp_by_iat.get_ptr_device(), r_dr.get_ptr_device(), 
//**                                  sbessel_coefs.get_ptr_device(), lo_coefs.get_ptr_device(), jlo.get_ptr_device());
//**     jlo.copy_to_host();
//**     // compute overlap and kinetic energy
//**     for (int icol = num_gkvec_col(); icol < apwlo_basis_size_col(); icol++)
//**     {
//**         int ia = apwlo_basis_descriptors_col_[icol].ia;
//**         int l = apwlo_basis_descriptors_col_[icol].l;
//**         int lm = apwlo_basis_descriptors_col_[icol].lm;
//** 
//**         for (int igkloc = 0; igkloc < num_gkvec_row(); igkloc++)
//**         {
//**             o(igkloc, icol) = (fourpi / sqrt(parameters_.omega())) * conj(zil_[l]) * gkvec_ylm_(lm, igkloc) * 
//**                               jlo(igkloc, icol - num_gkvec_col()) * conj(gkvec_phase_factors_(igkloc, ia));
//** 
//**             // kinetic part <G+k| -1/2 \nabla^2 |lo> = 1/2 |G+k|^2 <G+k|lo>
//**             h(igkloc, icol) = 0.5 * pow(gkvec_len_[igkloc], 2) * o(igkloc, icol);
//**         }
//**     }
//**     t2.stop();
//** 
//**     l_by_lm_.allocate_on_device();
//**     l_by_lm_.copy_to_device();
//** 
//**     mdarray<double_complex, 3> jvlo(parameters_.lmmax_pw(), num_gkvec_row(), num_lo_col());
//**     jvlo.allocate_on_device();
//**     
//**     t3.start();
//**     sbessel_vlo_inner_product_gpu(kargs.get_ptr_device(), num_gkvec_row(), num_lo_col(), parameters_.lmmax_pw(), 
//**                                   l_by_lm_.get_ptr_device(), iat_by_ilo.get_ptr_device(), nmtp_by_iat.get_ptr_device(), 
//**                                   r_dr.get_ptr_device(), sbessel_coefs.get_ptr_device(), vlo_coefs.get_ptr_device(), 
//**                                   jvlo.get_ptr_device());
//**     jvlo.copy_to_host();
//** 
//**     l_by_lm_.deallocate_on_device();
//** 
//**     #pragma omp parallel for default(shared)
//**     for (int igkloc = 0; igkloc < num_gkvec_row(); igkloc++)
//**     {
//**         for (int icol = num_gkvec_col(); icol < apwlo_basis_size_col(); icol++)
//**         {
//**             int ia = apwlo_basis_descriptors_col_[icol].ia;
//** 
//**             // add <G+k|V|lo>
//**             double_complex zt1(0, 0);
//**             for (int l = 0; l <= parameters_.lmax_pw(); l++)
//**             {
//**                 for (int m = -l; m <= l; m++)
//**                 {
//**                     int lm = Utils::lm_by_l_m(l, m);
//**                     zt1 += jvlo(lm, igkloc, icol - num_gkvec_col()) * conj(zil_[l]) * gkvec_ylm_(lm, igkloc);
//**                 }
//**             }
//**             zt1 *= ((fourpi / sqrt(parameters_.omega())) * conj(gkvec_phase_factors_(igkloc, ia)));
//**             h(igkloc, icol) += zt1;
//**         }
//**     }
//**     t3.stop();
//**    
//**     // restore the <lo|H|G+k> and <lo|G+k> bocks and exit
//**     if (num_ranks == 1)
//**     {
//**         for (int igkloc = 0; igkloc < num_gkvec_col(); igkloc++)
//**         {
//**             for (int irow = num_gkvec_row(); irow < apwlo_basis_size_row(); irow++)
//**             {
//**                 h(irow, igkloc) = conj(h(igkloc, irow));
//**                 o(irow, igkloc) = conj(o(igkloc, irow));
//**             }
//**         }
//**         return;
//**     }
//** 
//**     //** // second part: compute <lo|H|G+k> and <lo|G+k>
//** 
//**     //** int offset_col = (num_ranks > 1) ? num_gkvec_row() : 0;
//**     //** // compute V|lo>
//**     //** t1.start();
//**     //** for (int irow = 0; irow < num_lo_row(); irow++)
//**     //** {
//**     //**     int ia = apwlo_basis_descriptors_row_[num_gkvec_row() + irow].ia;
//**     //**     int lm = apwlo_basis_descriptors_row_[num_gkvec_row() + irow].lm;
//**     //**     int idxrf = apwlo_basis_descriptors_row_[num_gkvec_row() + irow].idxrf;
//**     //**     
//**     //**     for (int lm1 = 0; lm1 < parameters_.lmmax_pw(); lm1++)
//**     //**     {
//**     //**         svlo(lm1, irow) = new Spline<double_complex>(parameters_.atom(ia)->num_mt_points(), 
//**     //**                                                 parameters_.atom(ia)->radial_grid());
//** 
//**     //**         for (int k = 0; k < parameters_.gaunt().complex_gaunt_packed_L3_size(lm1, lm); k++)
//**     //**         {
//**     //**             int lm3 = parameters_.gaunt().complex_gaunt_packed_L3(lm1, lm, k).lm3;
//**     //**             double_complex cg = parameters_.gaunt().complex_gaunt_packed_L3(lm1, lm, k).cg;
//** 
//**     //**             for (int ir = 0; ir < parameters_.atom(ia)->num_mt_points(); ir++)
//**     //**             {
//**     //**                 (*svlo(lm1, irow))[ir] += (cg * effective_potential->f_rlm(lm3, ir, ia) * 
//**     //**                                            parameters_.atom(ia)->symmetry_class()->radial_function(ir, idxrf));
//**     //**             }
//**     //**         }
//** 
//**     //**         svlo(lm1, irow)->interpolate();
//**     //**     }
//**     //** }
//**     //** t1.stop();
//**    
//**     //** t2.start();
//**     //** for (int irow = num_gkvec_row(); irow < apwlo_basis_size_row(); irow++)
//**     //** {
//**     //**     int ia = apwlo_basis_descriptors_row_[irow].ia;
//**     //**     int iat = parameters_.atom_type_index_by_id(parameters_.atom(ia)->type_id());
//** 
//**     //**     int l = apwlo_basis_descriptors_row_[irow].l;
//**     //**     int lm = apwlo_basis_descriptors_row_[irow].lm;
//**     //**     int idxrf = apwlo_basis_descriptors_row_[irow].idxrf;
//** 
//**     //**     // compue overlap <lo|G+k>
//**     //**     Spline<double> slo(parameters_.atom(ia)->num_mt_points(), parameters_.atom(ia)->radial_grid());
//**     //**     for (int ir = 0; ir < slo.num_points(); ir++)
//**     //**         slo[ir] = parameters_.atom(ia)->symmetry_class()->radial_function(ir, idxrf);
//**     //**     slo.interpolate();
//**     //**     
//**     //**     #pragma omp parallel for default(shared)
//**     //**     for (int igkloc = 0; igkloc < num_gkvec_col(); igkloc++)
//**     //**     {
//**     //**         o(irow, igkloc) = (fourpi / sqrt(parameters_.omega())) * zil_[l] * 
//**     //**                           conj(gkvec_ylm_(lm, offset_col + igkloc)) * 
//**     //**                           Spline<double>::integrate(&slo, (*sbessel_[offset_col + igkloc])(l, iat)) * 
//**     //**                           gkvec_phase_factors_(offset_col + igkloc, ia);
//** 
//**     //**         // kinetic part <li| -1/2 \nabla^2 |G+k> = 1/2 |G+k|^2 <lo|G+k>
//**     //**         h(irow, igkloc) = 0.5 * pow(gkvec_len_[offset_col + igkloc], 2) * o(irow, igkloc);
//**     //**     }
//**     //** }
//**     //** t2.stop();
//** 
//**     //** t3.start();
//**     //** #pragma omp parallel for default(shared)
//**     //** for (int igkloc = 0; igkloc < num_gkvec_col(); igkloc++)
//**     //** {
//**     //**     for (int irow = num_gkvec_row(); irow < apwlo_basis_size_row(); irow++)
//**     //**     {
//**     //**         int ia = apwlo_basis_descriptors_row_[irow].ia;
//**     //**         int iat = parameters_.atom_type_index_by_id(parameters_.atom(ia)->type_id());
//** 
//**     //**         //int l = apwlo_basis_descriptors_row_[irow].l;
//**     //**         //int lm = apwlo_basis_descriptors_row_[irow].lm;
//**     //**         //int idxrf = apwlo_basis_descriptors_row_[irow].idxrf;
//** 
//**     //**         //*// compue overlap <lo|G+k>
//**     //**         //*Spline<double> s(parameters_.atom(ia)->num_mt_points(), parameters_.atom(ia)->radial_grid());
//**     //**         //*for (int ir = 0; ir < s.num_points(); ir++)
//**     //**         //*    s[ir] = (*sbessel_[offset_col + igkloc])(ir, l, iat) * 
//**     //**         //*            parameters_.atom(ia)->symmetry_class()->radial_function(ir, idxrf);
//**     //**         //*s.interpolate();
//**     //**         //*    
//**     //**         //*o(irow, igkloc) = (fourpi / sqrt(parameters_.omega())) * zil_[l] * 
//**     //**         //*                  conj(gkvec_ylm_(lm, offset_col + igkloc)) * s.integrate(2) * 
//**     //**         //*                  gkvec_phase_factors_(offset_col + igkloc, ia);
//** 
//**     //**         //*// kinetic part <li| -1/2 \nabla^2 |G+k> = 1/2 |G+k|^2 <lo|G+k>
//**     //**         //*h(irow, igkloc) = 0.5 * pow(gkvec_len_[offset_col + igkloc], 2) * o(irow, igkloc);
//** 
//**     //**         // add <lo|V|G+k>
//**     //**         double_complex zt1(0, 0);
//**     //**         for (int l1 = 0; l1 <= parameters_.lmax_pw(); l1++)
//**     //**         {
//**     //**             for (int m1 = -l1; m1 <= l1; m1++)
//**     //**             {
//**     //**                 int lm1 = Utils::lm_by_l_m(l1, m1);
//** 
//**     //**                 zt1 += conj(Spline<double_complex>::integrate(svlo(lm1, irow - num_gkvec_row()), 
//**     //**                                                          (*sbessel_[offset_col + igkloc])(l1, iat))) * 
//**     //**                        zil_[l1] * conj(gkvec_ylm_(lm1, offset_col + igkloc));
//**     //**             }
//**     //**         }
//**     //**         zt1 *= ((fourpi / sqrt(parameters_.omega())) * gkvec_phase_factors_(offset_col + igkloc, ia));
//**     //**         h(irow, igkloc) += zt1;
//**     //**     }
//**     //** }
//**     //** t3.stop();
//**     //** 
//**     //** for (int irow = 0; irow < num_lo_row(); irow++)
//**     //** {
//**     //**     for (int lm = 0; lm < parameters_.lmmax_pw(); lm++) delete svlo(lm, irow);
//**     //** }
//** }
//** 
//**




//=====================================================================================================================
// CPU code, (L)APW+lo basis
//=====================================================================================================================
template<> void Band::set_fv_h_o<cpu, full_potential_lapwlo>(K_point* kp, Periodic_function<double>* effective_potential,
                                                             mdarray<double_complex, 2>& h, mdarray<double_complex, 2>& o)
{
    Timer t("sirius::Band::set_fv_h_o");
   
    // index of column apw coefficients in apw array
    int apw_offset_col = kp->apw_offset_col();
    
    mdarray<double_complex, 2> alm(kp->num_gkvec_loc(), parameters_.unit_cell()->max_mt_aw_basis_size());
    mdarray<double_complex, 2> halm(kp->num_gkvec_row(), parameters_.unit_cell()->max_mt_aw_basis_size());

    h.zero();
    o.zero();

    double_complex zone(1, 0);
    
    for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
    {
        Atom* atom = parameters_.unit_cell()->atom(ia);
        Atom_type* type = atom->type();
       
        // generate conjugated coefficients
        kp->generate_matching_coefficients<true>(kp->num_gkvec_loc(), ia, alm);
        
        // apply muffin-tin part to <bra|
        apply_hmt_to_apw<nm>(kp->num_gkvec_row(), ia, alm, halm);
        
        // generate <apw|apw> block; |ket> is conjugated, so it is "unconjugated" back
        blas<cpu>::gemm(0, 2, kp->num_gkvec_row(), kp->num_gkvec_col(), type->mt_aw_basis_size(), zone, 
                        &alm(0, 0), alm.ld(), &alm(apw_offset_col, 0), alm.ld(), zone, &o(0, 0), o.ld()); 
            
        // generate <apw|H|apw> block; |ket> is conjugated, so it is "unconjugated" back
        blas<cpu>::gemm(0, 2, kp->num_gkvec_row(), kp->num_gkvec_col(), type->mt_aw_basis_size(), zone, 
                        &halm(0, 0), halm.ld(), &alm(apw_offset_col, 0), alm.ld(), zone, &h(0, 0), h.ld());
       
        // setup apw-lo blocks
        set_fv_h_o_apw_lo(kp, type, atom, ia, alm, h, o);
    } //ia

    set_fv_h_o_it(kp, effective_potential, h, o);

    set_fv_h_o_lo_lo(kp, h, o);

    alm.deallocate();
    halm.deallocate();
}

void Band::set_fv_h_o_apw_lo(K_point* kp, Atom_type* type, Atom* atom, int ia, mdarray<double_complex, 2>& alm, 
                             mdarray<double_complex, 2>& h, mdarray<double_complex, 2>& o)
{
    Timer t("sirius::Band::set_fv_h_o_apw_lo");
    
    int apw_offset_col = kp->apw_offset_col();
    
    // apw-lo block
    for (int i = 0; i < kp->num_atom_lo_cols(ia); i++)
    {
        int icol = kp->lo_col(ia, i);

        int l = kp->apwlo_basis_descriptors_col(icol).l;
        int lm = kp->apwlo_basis_descriptors_col(icol).lm;
        int idxrf = kp->apwlo_basis_descriptors_col(icol).idxrf;
        int order = kp->apwlo_basis_descriptors_col(icol).order;
        
        for (int j1 = 0; j1 < type->mt_aw_basis_size(); j1++) 
        {
            int lm1 = type->indexb(j1).lm;
            int idxrf1 = type->indexb(j1).idxrf;
                    
            double_complex zsum = gaunt_coefs_->sum_L3_gaunt(lm1, lm, atom->h_radial_integrals(idxrf, idxrf1));

            if (abs(zsum) > 1e-14)
            {
                for (int igkloc = 0; igkloc < kp->num_gkvec_row(); igkloc++) h(igkloc, icol) += zsum * alm(igkloc, j1);
            }
        }

        for (int order1 = 0; order1 < (int)type->aw_descriptor(l).size(); order1++)
        {
            for (int igkloc = 0; igkloc < kp->num_gkvec_row(); igkloc++)
            {
                o(igkloc, icol) += atom->symmetry_class()->o_radial_integral(l, order1, order) * 
                                   alm(igkloc, type->indexb_by_lm_order(lm, order1));
            }
        }
    }

    std::vector<double_complex> ztmp(kp->num_gkvec_col());
    // lo-apw block
    for (int i = 0; i < kp->num_atom_lo_rows(ia); i++)
    {
        int irow = kp->lo_row(ia, i);

        int l = kp->apwlo_basis_descriptors_row(irow).l;
        int lm = kp->apwlo_basis_descriptors_row(irow).lm;
        int idxrf = kp->apwlo_basis_descriptors_row(irow).idxrf;
        int order = kp->apwlo_basis_descriptors_row(irow).order;

        memset(&ztmp[0], 0, kp->num_gkvec_col() * sizeof(double_complex));
    
        for (int j1 = 0; j1 < type->mt_aw_basis_size(); j1++) 
        {
            int lm1 = type->indexb(j1).lm;
            int idxrf1 = type->indexb(j1).idxrf;
                    
            double_complex zsum = gaunt_coefs_->sum_L3_gaunt(lm, lm1, atom->h_radial_integrals(idxrf, idxrf1));

            if (abs(zsum) > 1e-14)
            {
                for (int igkloc = 0; igkloc < kp->num_gkvec_col(); igkloc++)
                    ztmp[igkloc] += zsum * conj(alm(apw_offset_col + igkloc, j1));
            }
        }

        for (int igkloc = 0; igkloc < kp->num_gkvec_col(); igkloc++) h(irow, igkloc) += ztmp[igkloc]; 

        for (int order1 = 0; order1 < (int)type->aw_descriptor(l).size(); order1++)
        {
            for (int igkloc = 0; igkloc < kp->num_gkvec_col(); igkloc++)
            {
                o(irow, igkloc) += atom->symmetry_class()->o_radial_integral(l, order, order1) * 
                                   conj(alm(apw_offset_col + igkloc, type->indexb_by_lm_order(lm, order1)));
            }
        }
    }
}

void Band::set_fv_h_o_it(K_point* kp, Periodic_function<double>* effective_potential, 
                         mdarray<double_complex, 2>& h, mdarray<double_complex, 2>& o)
{
    Timer t("sirius::Band::set_fv_h_o_it");

    #pragma omp parallel for default(shared)
    for (int igk_col = 0; igk_col < kp->num_gkvec_col(); igk_col++) // loop over columns
    {
        for (int igk_row = 0; igk_row < kp->num_gkvec_row(); igk_row++) // for each column loop over rows
        {
            int ig12 = parameters_.reciprocal_lattice()->index_g12(kp->apwlo_basis_descriptors_row(igk_row).ig,
                                                                   kp->apwlo_basis_descriptors_col(igk_col).ig);
            
            // pw kinetic energy
            double t1 = 0.5 * Utils::scalar_product(kp->apwlo_basis_descriptors_row(igk_row).gkvec_cart, 
                                                    kp->apwlo_basis_descriptors_col(igk_col).gkvec_cart);
                               
            h(igk_row, igk_col) += (effective_potential->f_pw(ig12) + t1 * parameters_.step_function()->theta_pw(ig12));
            o(igk_row, igk_col) += parameters_.step_function()->theta_pw(ig12);
        }
    }
}

void Band::set_fv_h_o_lo_lo(K_point* kp, mdarray<double_complex, 2>& h, mdarray<double_complex, 2>& o)
{
    Timer t("sirius::Band::set_fv_h_o_lo_lo");

    // lo-lo block
    #pragma omp parallel for default(shared)
    for (int icol = kp->num_gkvec_col(); icol < kp->apwlo_basis_size_col(); icol++)
    {
        int ia = kp->apwlo_basis_descriptors_col(icol).ia;
        int lm2 = kp->apwlo_basis_descriptors_col(icol).lm; 
        int idxrf2 = kp->apwlo_basis_descriptors_col(icol).idxrf; 

        for (int irow = kp->num_gkvec_row(); irow < kp->apwlo_basis_size_row(); irow++)
        {
            if (ia == kp->apwlo_basis_descriptors_row(irow).ia)
            {
                Atom* atom = parameters_.unit_cell()->atom(ia);
                int lm1 = kp->apwlo_basis_descriptors_row(irow).lm; 
                int idxrf1 = kp->apwlo_basis_descriptors_row(irow).idxrf; 

                h(irow, icol) += gaunt_coefs_->sum_L3_gaunt(lm1, lm2, atom->h_radial_integrals(idxrf1, idxrf2));

                if (lm1 == lm2)
                {
                    int l = kp->apwlo_basis_descriptors_row(irow).l;
                    int order1 = kp->apwlo_basis_descriptors_row(irow).order; 
                    int order2 = kp->apwlo_basis_descriptors_col(icol).order; 
                    o(irow, icol) += atom->symmetry_class()->o_radial_integral(l, order1, order2);
                }
            }
        }
    }
}

void Band::solve_fv_evp_1stage(K_point* kp, mdarray<double_complex, 2>& h, mdarray<double_complex, 2>& o, 
                               std::vector<double>& fv_eigen_values, mdarray<double_complex, 2>& fv_eigen_vectors)
{
    Timer t("sirius::Band::solve_fv_evp");
    generalized_evp* solver = NULL;

    switch (parameters_.eigen_value_solver())
    {
        case lapack:
        {
            solver = new generalized_evp_lapack(-1.0);
            break;
        }
        case scalapack:
        {
            solver = new generalized_evp_scalapack(parameters_.cyclic_block_size(), kp->num_ranks_row(), 
                                                   kp->num_ranks_col(), parameters_.blacs_context(), -1.0);
            break;
        }
        case elpa:
        {
            solver = new generalized_evp_elpa(parameters_.cyclic_block_size(), 
                                              kp->apwlo_basis_size_row(), kp->num_ranks_row(), kp->rank_row(),
                                              kp->apwlo_basis_size_col(), kp->num_ranks_col(), kp->rank_col(), 
                                              parameters_.blacs_context(), 
                                              parameters_.mpi_grid().communicator(1 << _dim_row_),
                                              parameters_.mpi_grid().communicator(1 << _dim_col_),
                                              parameters_.mpi_grid().communicator(1 << _dim_col_ | 1 << _dim_row_));
            break;
        }
        case magma:
        {
            solver = new generalized_evp_magma();
            break;
        }
        case plasma:
        {
            solver = new generalized_evp_lapack(-1.0);
            break;
        }
        default:
        {
            error_local(__FILE__, __LINE__, "eigen value solver is not defined");
        }
    }

    solver->solve(kp->apwlo_basis_size(), parameters_.num_fv_states(), h.get_ptr(), h.ld(), o.get_ptr(), o.ld(), 
                  &fv_eigen_values[0], fv_eigen_vectors.get_ptr(), fv_eigen_vectors.ld());

    delete solver;
}

//== void K_point::solve_fv_evp_2stage(mdarray<double_complex, 2>& h, mdarray<double_complex, 2>& o)
//== {
//==     if (parameters_.eigen_value_solver() != lapack) error_local(__FILE__, __LINE__, "implemented for LAPACK only");
//==     
//==     standard_evp_lapack s;
//== 
//==     std::vector<double> o_eval(apwlo_basis_size());
//==     
//==     mdarray<double_complex, 2> o_tmp(apwlo_basis_size(), apwlo_basis_size());
//==     memcpy(o_tmp.get_ptr(), o.get_ptr(), o.size() * sizeof(double_complex));
//==     mdarray<double_complex, 2> o_evec(apwlo_basis_size(), apwlo_basis_size());
//==  
//==     s.solve(apwlo_basis_size(), o_tmp.get_ptr(), o_tmp.ld(), &o_eval[0], o_evec.get_ptr(), o_evec.ld());
//== 
//==     int num_dependent_apwlo = 0;
//==     for (int i = 0; i < apwlo_basis_size(); i++) 
//==     {
//==         if (fabs(o_eval[i]) < 1e-4) 
//==         {
//==             num_dependent_apwlo++;
//==         }
//==         else
//==         {
//==             o_eval[i] = 1.0 / sqrt(o_eval[i]);
//==         }
//==     }
//== 
//==     //std::cout << "num_dependent_apwlo = " << num_dependent_apwlo << std::endl;
//== 
//==     mdarray<double_complex, 2> h_tmp(apwlo_basis_size(), apwlo_basis_size());
//==     // compute h_tmp = Z^{h.c.} * H
//==     blas<cpu>::gemm(2, 0, apwlo_basis_size(), apwlo_basis_size(), apwlo_basis_size(), o_evec.get_ptr(), 
//==                     o_evec.ld(), h.get_ptr(), h.ld(), h_tmp.get_ptr(), h_tmp.ld());
//==     // compute \tilda H = Z^{h.c.} * H * Z = h_tmp * Z
//==     blas<cpu>::gemm(0, 0, apwlo_basis_size(), apwlo_basis_size(), apwlo_basis_size(), h_tmp.get_ptr(), 
//==                     h_tmp.ld(), o_evec.get_ptr(), o_evec.ld(), h.get_ptr(), h.ld());
//== 
//==     int reduced_apwlo_basis_size = apwlo_basis_size() - num_dependent_apwlo;
//==     
//==     for (int i = 0; i < reduced_apwlo_basis_size; i++)
//==     {
//==         for (int j = 0; j < reduced_apwlo_basis_size; j++)
//==         {
//==             double d = o_eval[num_dependent_apwlo + j] * o_eval[num_dependent_apwlo + i];
//==             h(num_dependent_apwlo + j, num_dependent_apwlo + i) *= d;
//==         }
//==     }
//== 
//==     std::vector<double> h_eval(reduced_apwlo_basis_size);
//==     s.solve(reduced_apwlo_basis_size, &h(num_dependent_apwlo, num_dependent_apwlo), h.ld(), &h_eval[0], 
//==             h_tmp.get_ptr(), h_tmp.ld());
//== 
//==     for (int i = 0; i < reduced_apwlo_basis_size; i++)
//==     {
//==         for (int j = 0; j < reduced_apwlo_basis_size; j++) h_tmp(j, i) *= o_eval[num_dependent_apwlo + j];
//==     }
//== 
//==     for (int i = 0; i < parameters_.num_fv_states(); i++) fv_eigen_values_[i] = h_eval[i];
//== 
//==     blas<cpu>::gemm(0, 0, apwlo_basis_size(), parameters_.num_fv_states(), reduced_apwlo_basis_size, 
//==                     &o_evec(0, num_dependent_apwlo), o_evec.ld(), h_tmp.get_ptr(), h_tmp.ld(), 
//==                     fv_eigen_vectors_.get_ptr(), fv_eigen_vectors_.ld());
//== }

void Band::diag_fv_full_potential(K_point* kp, Periodic_function<double>* effective_potential)
{
    log_function_enter(__func__);
    Timer t("sirius::Band::diag_fv_full_potential");

    if (kp->num_ranks() > 1 && (parameters_.eigen_value_solver() == lapack || parameters_.eigen_value_solver() == magma))
        error_local(__FILE__, __LINE__, "Can't use more than one MPI rank for LAPACK or MAGMA eigen-value solver");

    mdarray<double_complex, 2> h(kp->apwlo_basis_size_row(), kp->apwlo_basis_size_col());
    mdarray<double_complex, 2> o(kp->apwlo_basis_size_row(), kp->apwlo_basis_size_col());
    
    // Magma requires special allocation
    #ifdef _MAGMA_
    if (parameters_.eigen_value_solver() == magma)
    {
        h.pin_memory();
        o.pin_memory();
    }
    #endif
   
    // setup Hamiltonian and overlap
    switch (parameters_.processing_unit())
    {
        case cpu:
        {
            set_fv_h_o<cpu, full_potential_lapwlo>(kp, effective_potential, h, o);
            break;
        }
        #ifdef _GPU_
        case gpu:
        {
            //set_fv_h_o<gpu, basis_type>(kp, effective_potential, h, o);
            break;
        }
        #endif
        default:
        {
            error_local(__FILE__, __LINE__, "wrong processing unit");
        }
    }
    
    // TODO: move debug code to a separate function
    if (debug_level > 0 && parameters_.eigen_value_solver() == lapack)
    {
        Utils::check_hermitian("h", h);
        Utils::check_hermitian("o", o);
    }

    //sirius_io::hdf5_write_matrix("h.h5", h);
    //sirius_io::hdf5_write_matrix("o.h5", o);
    
    //Utils::write_matrix("h.txt", true, h);
    //Utils::write_matrix("o.txt", true, o);

    //** if (verbosity_level > 1)
    //** {
    //**     double h_max = 0;
    //**     double o_max = 0;
    //**     int h_irow = 0;
    //**     int h_icol = 0;
    //**     int o_irow = 0;
    //**     int o_icol = 0;
    //**     std::vector<double> h_diag(apwlo_basis_size(), 0);
    //**     std::vector<double> o_diag(apwlo_basis_size(), 0);
    //**     for (int icol = 0; icol < apwlo_basis_size_col(); icol++)
    //**     {
    //**         int idxglob = apwlo_basis_descriptors_col_[icol].idxglob;
    //**         for (int irow = 0; irow < apwlo_basis_size_row(); irow++)
    //**         {
    //**             if (apwlo_basis_descriptors_row_[irow].idxglob == idxglob)
    //**             {
    //**                 h_diag[idxglob] = abs(h(irow, icol));
    //**                 o_diag[idxglob] = abs(o(irow, icol));
    //**             }
    //**             if (abs(h(irow, icol)) > h_max)
    //**             {
    //**                 h_max = abs(h(irow, icol));
    //**                 h_irow = irow;
    //**                 h_icol = icol;
    //**             }
    //**             if (abs(o(irow, icol)) > o_max)
    //**             {
    //**                 o_max = abs(o(irow, icol));
    //**                 o_irow = irow;
    //**                 o_icol = icol;
    //**             }
    //**         }
    //**     }

    //**     Platform::allreduce(&h_diag[0], apwlo_basis_size(),
    //**                         parameters_.mpi_grid().communicator(1 << band->dim_row() | 1 << band->dim_col()));
    //**     
    //**     Platform::allreduce(&o_diag[0], apwlo_basis_size(),
    //**                         parameters_.mpi_grid().communicator(1 << band->dim_row() | 1 << band->dim_col()));
    //**     
    //**     if (parameters_.mpi_grid().root(1 << band->dim_row() | 1 << band->dim_col()))
    //**     {
    //**         std::stringstream s;
    //**         s << "h_diag : ";
    //**         for (int i = 0; i < apwlo_basis_size(); i++) s << h_diag[i] << " ";
    //**         s << std::endl;
    //**         s << "o_diag : ";
    //**         for (int i = 0; i < apwlo_basis_size(); i++) s << o_diag[i] << " ";
    //**         warning(__FILE__, __LINE__, s, 0);
    //**     }

    //**     std::stringstream s;
    //**     s << "h_max " << h_max << " irow, icol : " << h_irow << " " << h_icol << std::endl;
    //**     s << " (row) igk, ig, ia, l, lm, irdrf, order : " << apwlo_basis_descriptors_row_[h_irow].igk << " "  
    //**                                                       << apwlo_basis_descriptors_row_[h_irow].ig << " "  
    //**                                                       << apwlo_basis_descriptors_row_[h_irow].ia << " "  
    //**                                                       << apwlo_basis_descriptors_row_[h_irow].l << " "  
    //**                                                       << apwlo_basis_descriptors_row_[h_irow].lm << " "  
    //**                                                       << apwlo_basis_descriptors_row_[h_irow].idxrf << " "  
    //**                                                       << apwlo_basis_descriptors_row_[h_irow].order 
    //**                                                       << std::endl;
    //**     s << " (col) igk, ig, ia, l, lm, irdrf, order : " << apwlo_basis_descriptors_col_[h_icol].igk << " "  
    //**                                                       << apwlo_basis_descriptors_col_[h_icol].ig << " "  
    //**                                                       << apwlo_basis_descriptors_col_[h_icol].ia << " "  
    //**                                                       << apwlo_basis_descriptors_col_[h_icol].l << " "  
    //**                                                       << apwlo_basis_descriptors_col_[h_icol].lm << " "  
    //**                                                       << apwlo_basis_descriptors_col_[h_icol].idxrf << " "  
    //**                                                       << apwlo_basis_descriptors_col_[h_icol].order 
    //**                                                       << std::endl;

    //**     s << "o_max " << o_max << " irow, icol : " << o_irow << " " << o_icol << std::endl;
    //**     s << " (row) igk, ig, ia, l, lm, irdrf, order : " << apwlo_basis_descriptors_row_[o_irow].igk << " "  
    //**                                                       << apwlo_basis_descriptors_row_[o_irow].ig << " "  
    //**                                                       << apwlo_basis_descriptors_row_[o_irow].ia << " "  
    //**                                                       << apwlo_basis_descriptors_row_[o_irow].l << " "  
    //**                                                       << apwlo_basis_descriptors_row_[o_irow].lm << " "  
    //**                                                       << apwlo_basis_descriptors_row_[o_irow].idxrf << " "  
    //**                                                       << apwlo_basis_descriptors_row_[o_irow].order 
    //**                                                       << std::endl;
    //**     s << " (col) igk, ig, ia, l, lm, irdrf, order : " << apwlo_basis_descriptors_col_[o_icol].igk << " "  
    //**                                                       << apwlo_basis_descriptors_col_[o_icol].ig << " "  
    //**                                                       << apwlo_basis_descriptors_col_[o_icol].ia << " "  
    //**                                                       << apwlo_basis_descriptors_col_[o_icol].l << " "  
    //**                                                       << apwlo_basis_descriptors_col_[o_icol].lm << " "  
    //**                                                       << apwlo_basis_descriptors_col_[o_icol].idxrf << " "  
    //**                                                       << apwlo_basis_descriptors_col_[o_icol].order 
    //**                                                       << std::endl;
    //**     warning(__FILE__, __LINE__, s, 0);
    //** }
    
    assert(kp->apwlo_basis_size() > parameters_.num_fv_states());
    
    // debug scalapack
    //** std::vector<double> fv_eigen_values_glob(parameters_.num_fv_states());
    //** if ((debug_level > 2) && 
    //**     (parameters_.eigen_value_solver() == scalapack || parameters_.eigen_value_solver() == elpa))
    //** {
    //**     mdarray<double_complex, 2> h_glob(apwlo_basis_size(), apwlo_basis_size());
    //**     mdarray<double_complex, 2> o_glob(apwlo_basis_size(), apwlo_basis_size());
    //**     mdarray<double_complex, 2> fv_eigen_vectors_glob(apwlo_basis_size(), parameters_.num_fv_states());

    //**     h_glob.zero();
    //**     o_glob.zero();

    //**     for (int icol = 0; icol < apwlo_basis_size_col(); icol++)
    //**     {
    //**         int j = apwlo_basis_descriptors_col_[icol].idxglob;
    //**         for (int irow = 0; irow < apwlo_basis_size_row(); irow++)
    //**         {
    //**             int i = apwlo_basis_descriptors_row_[irow].idxglob;
    //**             h_glob(i, j) = h(irow, icol);
    //**             o_glob(i, j) = o(irow, icol);
    //**         }
    //**     }
    //**     
    //**     Platform::allreduce(h_glob.get_ptr(), (int)h_glob.size(), 
    //**                         parameters_.mpi_grid().communicator(1 << band->dim_row() | 1 << band->dim_col()));
    //**     
    //**     Platform::allreduce(o_glob.get_ptr(), (int)o_glob.size(), 
    //**                         parameters_.mpi_grid().communicator(1 << band->dim_row() | 1 << band->dim_col()));

    //**     Utils::check_hermitian("h_glob", h_glob);
    //**     Utils::check_hermitian("o_glob", o_glob);
    //**     
    //**     generalized_evp_lapack lapack_solver(-1.0);

    //**     lapack_solver.solve(apwlo_basis_size(), parameters_.num_fv_states(), h_glob.get_ptr(), h_glob.ld(), 
    //**                         o_glob.get_ptr(), o_glob.ld(), &fv_eigen_values_glob[0], fv_eigen_vectors_glob.get_ptr(),
    //**                         fv_eigen_vectors_glob.ld());
    //** }
    
    if (fix_apwlo_linear_dependence)
    {
        //solve_fv_evp_2stage(kp, h, o);
    }
    else
    {
        std::vector<double> eval(parameters_.num_fv_states());
        solve_fv_evp_1stage(kp, h, o, eval, kp->fv_eigen_vectors());
        kp->set_fv_eigen_values(&eval[0]);
    }
        
    #ifdef _MAGMA_
    if (parameters_.eigen_value_solver() == magma)
    {
        h.unpin_memory();
        o.unpin_memory();
    }
    #endif
   
    h.deallocate();
    o.deallocate();

    //** if ((debug_level > 2) && (parameters_.eigen_value_solver() == scalapack))
    //** {
    //**     double d = 0.0;
    //**     for (int i = 0; i < parameters_.num_fv_states(); i++) 
    //**         d += fabs(fv_eigen_values_[i] - fv_eigen_values_glob[i]);
    //**     std::stringstream s;
    //**     s << "Totoal eigen-value difference : " << d;
    //**     warning(__FILE__, __LINE__, s, 0);
    //** }

    log_function_exit(__func__);
}

void Band::apply_h_local(K_point* kp, std::vector<double>& effective_potential, std::vector<double>& pw_ekin, 
                         int n, double_complex* phi__, double_complex* hphi__)
{
    Timer t("sirius::Band::apply_h_local");

    mdarray<double_complex, 2> phi(phi__, kp->num_gkvec(), n);
    mdarray<double_complex, 2> hphi(hphi__, kp->num_gkvec(), n);

    auto fft = parameters_.reciprocal_lattice()->fft_coarse();
    
    int num_fft_threads = Platform::num_fft_threads();
    #pragma omp parallel default(shared) num_threads(num_fft_threads)
    {        
        int thread_id = omp_get_thread_num();
        std::vector<double_complex> phi_r(fft->size());
        
        #pragma omp for
        for (int i = 0; i < n; i++)
        {
            fft->input(kp->num_gkvec(), kp->fft_index_coarse(), &phi(0, i), thread_id);
            fft->transform(1, thread_id);
            fft->output(&phi_r[0], thread_id);

            for (int ir = 0; ir < fft->size(); ir++) phi_r[ir] *= effective_potential[ir];

            fft->input(&phi_r[0], thread_id);
            fft->transform(-1, thread_id);
            fft->output(kp->num_gkvec(), kp->fft_index_coarse(), &hphi(0, i), thread_id);
            
            for (int igk = 0; igk < kp->num_gkvec(); igk++) hphi(igk, i) += phi(igk, i) * pw_ekin[igk];
        }
    }
}

#ifdef _GPU_
struct exec_fft_args
{
    int thread_id;
    int num_phi;
    K_point* kp;
    FFT3D<cpu>* fft;
    mdarray<double_complex, 2>* phi;
    mdarray<double_complex, 2>* hphi;
    mdarray<double, 1>* veff;
    mdarray<double_complex, 2>* gamma;
    mdarray<double_complex, 2>* kappa;
};

pthread_mutex_t exec_fft_mutex;
int idxfft;

void* exec_gpu_fft(void* args__)
{
    exec_fft_args* args = (exec_fft_args*)args__;

    FFT3D<gpu> fft(args->fft->grid_size());

    mdarray<int, 1> fft_index_coarse(args->kp->fft_index_coarse(), args->kp->num_gkvec());
    fft_index_coarse.allocate_on_device();
    fft_index_coarse.copy_to_device();

    int nfft_buf = (int)(args->gamma->size() / fft.size());
    if (nfft_buf == 0) return NULL; // TODO: fix this

    int nfft_max = std::min(fft.num_fft_max(), std::min(args->num_phi / 4, nfft_buf));
   
    fft.initialize(nfft_max); 

    bool done = false;

    while (!done)
    {
        pthread_mutex_lock(&exec_fft_mutex);
        int i = idxfft;
        if (idxfft + nfft_max > args->num_phi) 
        {
            done = true;
        }
        else
        {
            idxfft += nfft_max;
        }
        pthread_mutex_unlock(&exec_fft_mutex);

        if (!done)
        {
            cublas_set_matrix(args->kp->num_gkvec(), nfft_max, sizeof(double_complex), &(*args->phi)(0, i), args->phi->ld(), 
                              args->kappa->get_ptr_device(), args->kappa->ld());
            
            // use gamma as fft buffer
            fft.batch_load(args->kp->num_gkvec(), fft_index_coarse.get_ptr_device(), args->kappa->get_ptr_device(), 
                           args->gamma->get_ptr_device());

            fft.transform(1, args->gamma->get_ptr_device());
            scale_matrix_rows_gpu(fft.size(), nfft_max, args->gamma->get_ptr_device(), args->veff->get_ptr_device());
            fft.transform(-1, args->gamma->get_ptr_device());

            fft.batch_unload(args->kp->num_gkvec(), fft_index_coarse.get_ptr_device(), args->gamma->get_ptr_device(), 
                             args->kappa->get_ptr_device());

            cublas_get_matrix(args->kp->num_gkvec(), nfft_max, sizeof(double_complex), 
                              args->kappa->get_ptr_device(), args->kappa->ld(),
                              &(*args->hphi)(0, i), args->hphi->ld());
        }
    }

    fft.finalize();
    
    return NULL;
}

void* exec_cpu_fft(void* args__)
{
    exec_fft_args* args = (exec_fft_args*)args__;
    
    auto fft = args->fft;

    int thread_id = args->thread_id;

    bool done = false;
    while (!done)
    {
        pthread_mutex_lock(&exec_fft_mutex);
        int i = idxfft;
        if (idxfft + 1 > args->num_phi)
        {
            done = true;
        }
        else
        {
            idxfft++;
        }
        pthread_mutex_unlock(&exec_fft_mutex);

        if (!done)
        {
            fft->input(args->kp->num_gkvec(), args->kp->fft_index_coarse(), &(*args->phi)(0, i), thread_id);
            fft->transform(1, thread_id);

            for (int ir = 0; ir < fft->size(); ir++) fft->output_buffer(ir, thread_id) *= (*args->veff)(ir);

            fft->input(&fft->output_buffer(0, thread_id), thread_id);
            fft->transform(-1, thread_id);
            fft->output(args->kp->num_gkvec(), args->kp->fft_index_coarse(), &(*args->hphi)(0, i), thread_id);
        }
    }

    return NULL;
}

void Band::apply_h_local_gpu(K_point* kp, std::vector<double>& effective_potential, std::vector<double>& pw_ekin, 
                             int num_phi, mdarray<double_complex, 2>& gamma, mdarray<double_complex, 2>& kappa,
                             double_complex* phi__, double_complex* hphi__)
{
    Timer t("sirius::Band::apply_h_local_gpu");

    auto fft = parameters_.reciprocal_lattice()->fft_coarse();

    pthread_mutex_init(&exec_fft_mutex, NULL);

    mdarray<double, 1> veff(&effective_potential[0], fft->size());
    veff.allocate_on_device();
    veff.copy_to_device();

    mdarray<double_complex, 2> phi(phi__, kp->num_gkvec(), num_phi);
    mdarray<double_complex, 2> hphi(hphi__, kp->num_gkvec(), num_phi);

    idxfft = 0;

    int num_fft_threads = std::min(Platform::num_fft_threads() + 1, Platform::max_num_threads());
    
    std::vector<pthread_t> pthread_id(num_fft_threads);
    std::vector<exec_fft_args> args(num_fft_threads);

    for (int i = 0; i < num_fft_threads; i++)
    {
        args[i].thread_id = i;
        args[i].num_phi = num_phi;
        args[i].kp = kp;
        args[i].fft = fft;
        args[i].phi = &phi;
        args[i].hphi = &hphi;
        args[i].veff = &veff;
        args[i].gamma = &gamma;
        args[i].kappa = &kappa;
        if (i == 0 && num_fft_threads > 1)
        {
            pthread_create(&pthread_id[i], NULL, exec_gpu_fft, &args[i]);
        }
        else
        {
            pthread_create(&pthread_id[i], NULL, exec_cpu_fft, &args[i]);
        }
    }

    // sync threads
    for (int i = 0; i < num_fft_threads; i++) pthread_join(pthread_id[i], NULL);

    if (idxfft != num_phi) 
    {
        std::stringstream s;
        s << "not all FFTs are executed" << std::endl
          << " number of FFTS : " << num_phi << ", number of executed FFTs : " << idxfft;
        error_local(__FILE__, __LINE__, s);
    }

    pthread_mutex_destroy(&exec_fft_mutex);
    
    #pragma omp parallel for
    for (int i = 0; i < num_phi; i++)
    {
        for (int igk = 0; igk < kp->num_gkvec(); igk++) hphi(igk, i) += phi(igk, i) * pw_ekin[igk];
    }
}
#endif

void Band::get_h_o_diag(K_point* kp, Periodic_function<double>* effective_potential, std::vector<double>& pw_ekin, 
                        std::vector<double_complex>& h_diag, std::vector<double_complex>& o_diag)
{
    Timer t("sirius::Band::get_h_o_diag");

    h_diag.resize(kp->num_gkvec());
    o_diag.resize(kp->num_gkvec());
    
    // compute V_{loc}(G=0)
    double v0 = 0;
    for (int ir = 0; ir < fft_->size(); ir++) v0 += effective_potential->f_it<global>(ir);
    v0 /= parameters_.unit_cell()->omega();
    
    for (int igk = 0; igk < kp->num_gkvec(); igk++) h_diag[igk] = pw_ekin[igk] + v0;

    mdarray<double_complex, 2> beta_pw(kp->num_gkvec(), parameters_.unit_cell()->max_mt_basis_size());
    mdarray<double_complex, 2> beta_pw_tmp(parameters_.unit_cell()->max_mt_basis_size(), kp->num_gkvec());
    for (int iat = 0; iat < parameters_.unit_cell()->num_atom_types(); iat++)
    {
        auto atom_type = parameters_.unit_cell()->atom_type(iat);
        int nbf = atom_type->mt_basis_size();
        mdarray<double_complex, 2> d_sum(nbf, nbf);
        mdarray<double_complex, 2> q_sum(nbf, nbf);
        d_sum.zero();
        q_sum.zero();

        for (int i = 0; i < atom_type->num_atoms(); i++)
        {
            int ia = atom_type->atom_id(i);
        
            for (int xi2 = 0; xi2 < nbf; xi2++)
            {
                for (int xi1 = 0; xi1 < nbf; xi1++)
                {
                    d_sum(xi1, xi2) += parameters_.unit_cell()->atom(ia)->d_mtrx(xi1, xi2);
                    q_sum(xi1, xi2) += parameters_.unit_cell()->atom(ia)->type()->uspp().q_mtrx(xi1, xi2);
                }
            }
        }

        //kp->generate_beta_pw(&beta_pw(0, 0), atom_type);
        int ofs = parameters_.unit_cell()->beta_t_ofs(iat);
        for (int igk = 0; igk < kp->num_gkvec(); igk++)
        {
            for (int xi = 0; xi < nbf; xi++) beta_pw_tmp(xi, igk) = kp->beta_pw_t(igk, ofs + xi);
        }

        std::vector< std::pair<int, int> > idx(nbf * nbf);
        int n = 0;
        for (int xi2 = 0; xi2 < nbf; xi2++)
        {
            for (int xi1 = 0; xi1 < nbf; xi1++)
            {
                idx[n++] = std::pair<int, int>(xi1, xi2);
            }
        }

        #pragma omp parallel for
        for (int igk = 0; igk < kp->num_gkvec(); igk++)
        {
            for (int i = 0; i < n; i++)
            {
                int xi1 = idx[i].first;
                int xi2 = idx[i].second;
                double_complex z = beta_pw_tmp(xi1, igk) * conj(beta_pw_tmp(xi2, igk));

                h_diag[igk] += z * d_sum(xi1, xi2);
                o_diag[igk] += z * q_sum(xi1, xi2);
            }
        }
    }
}

//== // memory-conservative implementation
//== void Band::apply_h_o(K_point* kp, Periodic_function<double>* effective_potential, std::vector<double>& pw_ekin, int n,
//==                      double_complex* phi__, double_complex* hphi__, double_complex* ophi__)
//== {
//==     Timer t("sirius::Band::apply_h_o");
//== 
//==     mdarray<double_complex, 2> phi(phi__, kp->num_gkvec(), n);
//==     mdarray<double_complex, 2> hphi(hphi__, kp->num_gkvec(), n);
//==     mdarray<double_complex, 2> ophi(ophi__, kp->num_gkvec(), n);
//==     
//==     // apply local part of Hamiltonian
//==     apply_h_local(kp, effective_potential, pw_ekin, n, phi__, hphi__);
//==    
//==     // set intial ophi
//==     memcpy(ophi__, phi__, kp->num_gkvec() * n * sizeof(double_complex));
//== 
//==     mdarray<double_complex, 2> beta_pw(kp->num_gkvec(), parameters_.unit_cell()->max_mt_basis_size());
//==     mdarray<double_complex, 2> beta_phi(parameters_.unit_cell()->max_mt_basis_size(), n);
//==     mdarray<double_complex, 2> d_beta_phi(parameters_.unit_cell()->max_mt_basis_size(), n);
//==     mdarray<double_complex, 2> q_beta_phi(parameters_.unit_cell()->max_mt_basis_size(), n);
//== 
//==     for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
//==     {   
//==         // number of beta functions for a given atom
//==         int nbf = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();
//== 
//==         kp->generate_beta_pw(&beta_pw(0, 0), ia);
//== 
//==         // compute <beta|phi>
//==         blas<cpu>::gemm(2, 0, nbf, n, kp->num_gkvec(), &beta_pw(0, 0), beta_pw.ld(), &phi(0, 0), phi.ld(), 
//==                         &beta_phi(0, 0), beta_phi.ld());
//== 
//==         // compute D*<beta|phi>
//==         blas<cpu>::gemm(0, 0, nbf, n, nbf, &parameters_.unit_cell()->atom(ia)->d_mtrx(0, 0), nbf, 
//==                         &beta_phi(0, 0), beta_phi.ld(), &d_beta_phi(0, 0), d_beta_phi.ld());
//==         
//==         // multiply by <G+k|beta> and add to hphi
//==         blas<cpu>::gemm(0, 0, kp->num_gkvec(), n, nbf, double_complex(1, 0), &beta_pw(0, 0), beta_pw.ld(), 
//==                         &d_beta_phi(0, 0), d_beta_phi.ld(), double_complex(1, 0), &hphi(0, 0), hphi.ld());
//==         
//==         // compute Q*<beta|phi>
//==         blas<cpu>::gemm(0, 0, nbf, n, nbf, &parameters_.unit_cell()->atom(ia)->type()->uspp().q_mtrx(0, 0), nbf, 
//==                         &beta_phi(0, 0), beta_phi.ld(), &q_beta_phi(0, 0), q_beta_phi.ld());
//==         
//==         // multiply by <G+k|beta> and add to ophi
//==         blas<cpu>::gemm(0, 0, kp->num_gkvec(), n, nbf, double_complex(1, 0), &beta_pw(0, 0), beta_pw.ld(), 
//==                         &q_beta_phi(0, 0), q_beta_phi.ld(), double_complex(1, 0), &ophi(0, 0), ophi.ld());
//==     }
//== }

// memory-greedy implementation
void Band::apply_h_o_uspp_cpu(K_point* kp, std::vector<double>& effective_potential, std::vector<double>& pw_ekin, int n,
                              double_complex* phi__, double_complex* hphi__, double_complex* ophi__)
{
    Timer t("sirius::Band::apply_h_o", _global_timer_);

    mdarray<double_complex, 2> phi(phi__, kp->num_gkvec(), n);
    mdarray<double_complex, 2> hphi(hphi__, kp->num_gkvec(), n);
    mdarray<double_complex, 2> ophi(ophi__, kp->num_gkvec(), n);
    
    // apply local part of Hamiltonian
    apply_h_local(kp, effective_potential, pw_ekin, n, phi__, hphi__);
   
    // set intial ophi
    memcpy(ophi__, phi__, kp->num_gkvec() * n * sizeof(double_complex));

    // <\beta_{\xi}^{\alpha}|\phi_j>
    mdarray<double_complex, 2> beta_phi(parameters_.unit_cell()->num_beta_a(), n);
    
    // Q or D multiplied by <\beta_{\xi}^{\alpha}|\phi_j>
    mdarray<double_complex, 2> tmp(parameters_.unit_cell()->num_beta_a(), n);

    Timer t1("sirius::Band::apply_h_o|beta_phi");

    // compute <beta|phi>
    blas<cpu>::gemm(2, 0, parameters_.unit_cell()->num_beta_a(), n, kp->num_gkvec(), &kp->beta_pw_a(0, 0), kp->num_gkvec(), 
                    &phi(0, 0), phi.ld(), &beta_phi(0, 0), beta_phi.ld());
    t1.stop();
    
    // compute D*<beta|phi>
    for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
    {   
        int ofs = parameters_.unit_cell()->beta_a_ofs(ia);
        // number of beta functions for a given atom
        int nbf = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();
        blas<cpu>::gemm(0, 0, nbf, n, nbf, &parameters_.unit_cell()->atom(ia)->d_mtrx(0, 0), nbf, 
                        &beta_phi(ofs, 0), beta_phi.ld(), &tmp(ofs, 0), tmp.ld());
    }

    Timer t3("sirius::Band::apply_h_o|beta_D_beta_phi");
    // compute <G+k|beta> * D*<beta|phi> and add to hphi
    blas<cpu>::gemm(0, 0, kp->num_gkvec(), n, parameters_.unit_cell()->num_beta_a(), complex_one, 
                    &kp->beta_pw_a(0, 0), kp->num_gkvec(), &tmp(0, 0), tmp.ld(), complex_one, &hphi(0, 0), hphi.ld());
    t3.stop();

    // compute Q*<beta|phi>
    for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
    {   
        int ofs = parameters_.unit_cell()->beta_a_ofs(ia);
        // number of beta functions for a given atom
        int nbf = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();
        blas<cpu>::gemm(0, 0, nbf, n, nbf, &parameters_.unit_cell()->atom(ia)->type()->uspp().q_mtrx(0, 0), nbf, 
                        &beta_phi(ofs, 0), beta_phi.ld(), &tmp(ofs, 0), tmp.ld());
    }

    Timer t5("sirius::Band::apply_h_o|beta_Q_beta_phi");
    // computr <G+k|beta> * Q*<beta|phi> and add to ophi
    blas<cpu>::gemm(0, 0, kp->num_gkvec(), n, parameters_.unit_cell()->num_beta_a(), complex_one, 
                    &kp->beta_pw_a(0, 0), kp->num_gkvec(), &tmp(0, 0), tmp.ld(), complex_one, &ophi(0, 0), ophi.ld());
    t5.stop();
}

#ifdef _GPU_

// memory-greedy implementation
void Band::apply_h_o_uspp_gpu(K_point* kp, std::vector<double>& effective_potential, std::vector<double>& pw_ekin, int n,
                              mdarray<double_complex, 2>& gamma, mdarray<double_complex, 2>& kappa, 
                              double_complex* phi__, double_complex* hphi__, double_complex* ophi__)
{
    Timer t("sirius::Band::apply_h_o_uspp_gpu");

    mdarray<double_complex, 2> phi(phi__, kp->num_gkvec(), n);
    mdarray<double_complex, 2> hphi(hphi__, kp->num_gkvec(), n);
    mdarray<double_complex, 2> ophi(ophi__, kp->num_gkvec(), n);

    // apply local part of Hamiltonian
    apply_h_local_gpu(kp, effective_potential, pw_ekin, n, gamma, kappa, phi__, hphi__);
    
    // load hphi to the first part of kappa; TODO: apply_h_local_gpu must return hpi on gpu
    cublas_set_matrix(kp->num_gkvec(), n, sizeof(double_complex), hphi.get_ptr(), hphi.ld(), kappa.ptr_device(0, 0), kappa.ld());

    // load phi to the second part of kappa; this will be the initial ophi
    cublas_set_matrix(kp->num_gkvec(), n, sizeof(double_complex), phi.get_ptr(), phi.ld(), kappa.ptr_device(0, n), kappa.ld());
    
    
    // offset in the packed array of on-site matrices
    mdarray<int, 1> mtrx_ofs(parameters_.unit_cell()->num_atoms());     
    int packed_mtrx_size = 0;
    for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
    {   
        int nbf = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();
        mtrx_ofs(ia) = packed_mtrx_size;
        packed_mtrx_size += nbf * nbf;
    }

    // pack D and Q matrices
    mdarray<double_complex, 1> d_mtrx_packed(packed_mtrx_size);
    mdarray<double_complex, 1> q_mtrx_packed(packed_mtrx_size);
    for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
    {
        int nbf = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();
        for (int xi2 = 0; xi2 < nbf; xi2++)
        {
            for (int xi1 = 0; xi1 < nbf; xi1++)
            {
                d_mtrx_packed(mtrx_ofs(ia) + xi2 * nbf + xi1) = parameters_.unit_cell()->atom(ia)->d_mtrx(xi1, xi2);
                q_mtrx_packed(mtrx_ofs(ia) + xi2 * nbf + xi1) = parameters_.unit_cell()->atom(ia)->type()->uspp().q_mtrx(xi1, xi2);
            }
        }
    }

    kp->beta_pw_t().allocate_on_device();
    kp->beta_pw_t().copy_to_device();

    kp->gkvec().allocate_on_device(); 
    kp->gkvec().copy_to_device();

    parameters_.unit_cell()->atom_pos().allocate_on_device(); 
    parameters_.unit_cell()->atom_pos().copy_to_device();

    parameters_.unit_cell()->beta_t_idx().allocate_on_device(); 
    parameters_.unit_cell()->beta_t_idx().copy_to_device();

    // create <G+k|beta> and store it in gamma matrix
    create_beta_pw_gpu(kp->num_gkvec(), 
                       parameters_.unit_cell()->num_beta_a(), 
                       parameters_.unit_cell()->beta_t_idx().get_ptr_device(),
                       kp->beta_pw_t().get_ptr_device(),
                       kp->gkvec().get_ptr_device(),
                       parameters_.unit_cell()->atom_pos().get_ptr_device(),
                       gamma.get_ptr_device());

    parameters_.unit_cell()->beta_t_idx().deallocate_on_device();
    parameters_.unit_cell()->atom_pos().deallocate_on_device();
    kp->gkvec().deallocate_on_device();
    kp->beta_pw_t().deallocate_on_device();

    // <\beta_{\xi}^{\alpha}|\phi_j>
    mdarray<double_complex, 2> beta_phi(NULL, parameters_.unit_cell()->num_beta_a(), n);
    beta_phi.allocate_on_device();
    
    blas<gpu>::gemm(2, 0, parameters_.unit_cell()->num_beta_a(), n, kp->num_gkvec(), gamma.ptr_device(0, 0), gamma.ld(), 
                    kappa.ptr_device(0, n), kappa.ld(), beta_phi.ptr_device(0, 0), beta_phi.ld());

    // Q or D multiplied by <\beta_{\xi}^{\alpha}|\phi_j>
    mdarray<double_complex, 2> tmp(NULL, parameters_.unit_cell()->num_beta_a(), n);
    tmp.allocate_on_device();

    d_mtrx_packed.allocate_on_device();
    d_mtrx_packed.copy_to_device();
    // compute D*<beta|phi>
    for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
    {   
        int ofs = parameters_.unit_cell()->beta_a_ofs(ia);
        // number of beta functions for a given atom
        int nbf = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();
        blas<gpu>::gemm(0, 0, nbf, n, nbf, d_mtrx_packed.ptr_device(mtrx_ofs(ia)), nbf, 
                        beta_phi.ptr_device(ofs, 0), beta_phi.ld(), tmp.ptr_device(ofs, 0), tmp.ld());
    }
    d_mtrx_packed.deallocate_on_device();

    double_complex zone(1, 0);
    // compute <G+k|beta> * D*<beta|phi> and add to hphi
    blas<gpu>::gemm(0, 0, kp->num_gkvec(), n, parameters_.unit_cell()->num_beta_a(), &zone, gamma.get_ptr_device(), gamma.ld(), 
                    tmp.get_ptr_device(), tmp.ld(), &zone, kappa.get_ptr_device(), kappa.ld());

    q_mtrx_packed.allocate_on_device();
    q_mtrx_packed.copy_to_device();

    // compute Q*<beta|phi>
    for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
    {   
        int ofs = parameters_.unit_cell()->beta_a_ofs(ia);
        // number of beta functions for a given atom
        int nbf = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();
        blas<gpu>::gemm(0, 0, nbf, n, nbf, q_mtrx_packed.get_ptr_device(mtrx_ofs(ia)), nbf, 
                        beta_phi.get_ptr_device(ofs), beta_phi.ld(), tmp.get_ptr_device(ofs), tmp.ld());
    }
    q_mtrx_packed.deallocate_on_device();

    // computr <G+k|beta> * Q*<beta|phi> and add to ophi
    blas<gpu>::gemm(0, 0, kp->num_gkvec(), n, parameters_.unit_cell()->num_beta_a(), &zone, gamma.get_ptr_device(), gamma.ld(), 
                    tmp.get_ptr_device(), tmp.ld(), &zone, kappa.get_ptr_device(n * kp->num_gkvec()), kappa.ld());
    
    kappa.copy_to_host();
    for (int j = 0; j < n; j++)
    {
        for (int igk = 0; igk < kp->num_gkvec(); igk++)
        {
            hphi(igk, j) = kappa(igk, j);
            ophi(igk, j) = kappa(igk, j + n);
        }
    }
    tmp.deallocate_on_device();
    beta_phi.deallocate_on_device();
}

//== extern "C" void create_single_beta_pw_gpu(int num_gkvec, 
//==                                           int num_beta_a, 
//==                                           int beta_a_ofs, 
//==                                           int* beta_t_idx,
//==                                           void* beta_pw_type,
//==                                           double* gkvec,
//==                                           double* atom_pos,
//==                                           void* beta_pw);
//== 
//== extern "C" void generate_beta_phi_gpu(int num_gkvec, 
//==                                       int num_beta, 
//==                                       int num_phi, 
//==                                       int* beta_t_idx, 
//==                                       double* atom_pos,
//==                                       double* gkvec,
//==                                       void* beta_pw_type,
//==                                       void* phi,
//==                                       void* beta_phi);
//== 
//== // memory-safe implementation
//== void Band::apply_h_o_uspp_gpu(K_point* kp, std::vector<double>& effective_potential, std::vector<double>& pw_ekin, int n,
//==                               double_complex* phi__, double_complex* hphi__, double_complex* ophi__)
//== {
//==     Timer t("sirius::Band::apply_h_o_uspp_gpu");
//== 
//==     mdarray<double_complex, 2> phi(phi__, kp->num_gkvec(), n);
//==     mdarray<double_complex, 2> hphi(hphi__, kp->num_gkvec(), n);
//==     mdarray<double_complex, 2> ophi(ophi__, kp->num_gkvec(), n);
//== 
//==     phi.allocate_on_device();
//==     phi.copy_to_device();
//== 
//==     hphi.allocate_on_device();
//==     hphi.zero_on_device();
//== 
//==     ophi.allocate_on_device();
//==     ophi.zero_on_device();
//== 
//==     // apply local part of Hamiltonian
//==     apply_h_local(kp, effective_potential, pw_ekin, n, phi__, hphi__);
//==     
//== 
//==     mdarray<int, 1> mtrx_ofs(parameters_.unit_cell()->num_atoms()); // offset in the packed array of on-site matrices
//==     int packed_mtrx_size = 0;
//==     for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
//==     {   
//==         int nbf = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();
//==         mtrx_ofs(ia) = packed_mtrx_size;
//==         packed_mtrx_size += nbf * nbf;
//==     }
//== 
//==     mdarray<double_complex, 1> d_mtrx_packed(packed_mtrx_size);
//==     mdarray<double_complex, 1> q_mtrx_packed(packed_mtrx_size);
//== 
//==     for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
//==     {
//==         int nbf = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();
//==         for (int xi2 = 0; xi2 < nbf; xi2++)
//==         {
//==             for (int xi1 = 0; xi1 < nbf; xi1++)
//==             {
//==                 d_mtrx_packed(mtrx_ofs(ia) + xi2 * nbf + xi1) = parameters_.unit_cell()->atom(ia)->d_mtrx(xi1, xi2);
//==                 q_mtrx_packed(mtrx_ofs(ia) + xi2 * nbf + xi1) = parameters_.unit_cell()->atom(ia)->type()->uspp().q_mtrx(xi1, xi2);
//==             }
//==         }
//==     }
//== 
//==     d_mtrx_packed.allocate_on_device();
//==     d_mtrx_packed.copy_to_device();
//==     q_mtrx_packed.allocate_on_device();
//==     q_mtrx_packed.copy_to_device();
//== 
//==     // <G+k|\beta_{\xi}^{\alpha}>
//==     mdarray<double_complex, 2> beta_pw(NULL, kp->num_gkvec(), parameters_.unit_cell()->max_mt_basis_size());
//==     beta_pw.allocate_on_device();
//== 
//==     // <\beta_{\xi}^{\alpha}|\phi_j>
//==     //== mdarray<double_complex, 2> beta_phi(NULL, parameters_.unit_cell()->max_mt_basis_size(), n);
//==     //== beta_phi.allocate_on_device();
//==     mdarray<double_complex, 2> beta_phi(NULL, parameters_.unit_cell()->num_beta_a(), n);
//==     beta_phi.allocate_on_device();
//== 
//==     // Q or D multiplied by <\beta_{\xi}^{\alpha}|\phi_j>
//==     mdarray<double_complex, 2> tmp(NULL, parameters_.unit_cell()->max_mt_basis_size(), n);
//==     tmp.allocate_on_device();
//== 
//==     kp->beta_pw().allocate_on_device();
//==     kp->beta_pw().copy_to_device();
//== 
//==     kp->gkvec_gpu().allocate_on_device(); 
//==     kp->gkvec_gpu().copy_to_device();
//== 
//==     parameters_.unit_cell()->atom_pos().allocate_on_device(); 
//==     parameters_.unit_cell()->atom_pos().copy_to_device();
//== 
//==     parameters_.unit_cell()->beta_t_idx().allocate_on_device(); 
//==     parameters_.unit_cell()->beta_t_idx().copy_to_device();
//== 
//==     generate_beta_phi_gpu(kp->num_gkvec(), 
//==                           parameters_.unit_cell()->num_beta_a(), 
//==                           n, 
//==                           parameters_.unit_cell()->beta_t_idx().get_ptr_device(),
//==                           parameters_.unit_cell()->atom_pos().get_ptr_device(),
//==                           kp->gkvec_gpu().get_ptr_device(),
//==                           kp->beta_pw().get_ptr_device(),
//==                           phi.get_ptr_device(),
//==                           beta_phi.get_ptr_device());
//== 
//== 
//==     
//==     double_complex zone(1, 0);
//==     for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
//==     {
//==         int ofs = parameters_.unit_cell()->beta_a_ofs(ia);
//==         // number of beta functions for a given atom
//==         int nbf = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();
//== 
//==         create_single_beta_pw_gpu(kp->num_gkvec(), 
//==                                   nbf,
//==                                   ofs,
//==                                   parameters_.unit_cell()->beta_t_idx().get_ptr_device(),
//==                                   kp->beta_pw().get_ptr_device(),
//==                                   kp->gkvec_gpu().get_ptr_device(),
//==                                   parameters_.unit_cell()->atom_pos().get_ptr_device(),
//==                                   beta_pw.get_ptr_device());
//== 
//== 
//==         // compute <beta|phi>
//==         //blas<gpu>::gemm(2, 0, nbf, n, kp->num_gkvec(), beta_pw.get_ptr_device(), beta_pw.ld(), 
//==         //                phi.get_ptr_device(), phi.ld(), beta_phi.get_ptr_device(), beta_phi.ld());
//== 
//== 
//==         // compute D*<beta|phi>
//==         blas<gpu>::gemm(0, 0, nbf, n, nbf, d_mtrx_packed.get_ptr_device(mtrx_ofs(ia)), nbf, 
//==                         beta_phi.get_ptr_device(ofs), beta_phi.ld(), tmp.get_ptr_device(), tmp.ld());
//== 
//==         // multiply by <G+k|beta> and add to hphi
//==         blas<gpu>::gemm(0, 0, kp->num_gkvec(), n, nbf, &zone, beta_pw.get_ptr_device(), beta_pw.ld(), 
//==                         tmp.get_ptr_device(), tmp.ld(), &zone, hphi.get_ptr_device(), hphi.ld());
//== 
//==         // compute Q*<beta|phi>
//==         blas<gpu>::gemm(0, 0, nbf, n, nbf, q_mtrx_packed.get_ptr_device(mtrx_ofs(ia)), nbf, 
//==                         beta_phi.get_ptr_device(ofs), beta_phi.ld(), tmp.get_ptr_device(), tmp.ld());
//==         
//==         // multiply by <G+k|beta> and add to ophi
//==         blas<gpu>::gemm(0, 0, kp->num_gkvec(), n, nbf, &zone, beta_pw.get_ptr_device(), beta_pw.ld(), 
//==                         tmp.get_ptr_device(), tmp.ld(), &zone, ophi.get_ptr_device(), ophi.ld());
//==     }
//== 
//==     parameters_.unit_cell()->beta_t_idx().deallocate_on_device();
//==     parameters_.unit_cell()->atom_pos().deallocate_on_device();
//==     kp->gkvec_gpu().deallocate_on_device();
//==     kp->beta_pw().deallocate_on_device();
//== 
//==     tmp.deallocate_on_device();
//==     beta_phi.deallocate_on_device();
//==     beta_pw.deallocate_on_device();
//== 
//==     q_mtrx_packed.deallocate_on_device();
//==     d_mtrx_packed.deallocate_on_device();
//== 
//== 
//==     ophi.copy_to_host(hphi.get_ptr_device());
//== 
//==     for (int j = 0; j < n; j++)
//==     {
//==         for (int igk = 0; igk < kp->num_gkvec(); igk++) hphi(igk, j) += ophi(igk, j);
//==     }
//== 
//==     ophi.copy_to_host();
//==     for (int j = 0; j < n; j++)
//==     {
//==         for (int igk = 0; igk < kp->num_gkvec(); igk++) ophi(igk, j) += phi(igk, j);
//==     }
//== 
//==     phi.deallocate_on_device();
//==     hphi.deallocate_on_device();
//==     ophi.deallocate_on_device();
//== }
#endif

void Band::diag_fv_uspp_cpu(K_point* kp, Periodic_function<double>* effective_potential)
{
    Timer t("sirius::Band::diag_fv_uspp_cpu", _global_timer_);

    // map effective potential to a corase grid
    std::vector<double> veff_it_coarse(parameters_.reciprocal_lattice()->fft_coarse()->size());
    std::vector<double_complex> veff_pw_coarse(parameters_.reciprocal_lattice()->num_gvec_coarse());

    // take only first num_gvec_coarse plane-wave harmonics; this is enough to apply V_eff to \Psi
    for (int igc = 0; igc < parameters_.reciprocal_lattice()->num_gvec_coarse(); igc++)
    {
        int ig = parameters_.reciprocal_lattice()->gvec_index(igc);
        veff_pw_coarse[igc] = effective_potential->f_pw(ig);
    }
    parameters_.reciprocal_lattice()->fft_coarse()->input(parameters_.reciprocal_lattice()->num_gvec_coarse(), 
                                                          parameters_.reciprocal_lattice()->fft_index_coarse(),
                                                          &veff_pw_coarse[0]);
    parameters_.reciprocal_lattice()->fft_coarse()->transform(1);
    parameters_.reciprocal_lattice()->fft_coarse()->output(&veff_it_coarse[0]);

    // short notation for target wave-functions
    mdarray<double_complex, 2>& psi = kp->fv_states_col();

    // short notation for number of target wave-functions
    int num_bands = parameters_.num_fv_states();     

    // cache kinetic energy,
    std::vector<double> pw_ekin = kp->get_pw_ekin();

    // get diagonal elements for preconditioning
    std::vector<double_complex> h_diag;
    std::vector<double_complex> o_diag;
    get_h_o_diag(kp, effective_potential, pw_ekin, h_diag, o_diag);
    
    int max_iter = 10;
    int num_phi = std::min(4 * num_bands, kp->num_gkvec());

    mdarray<double_complex, 2> phi(kp->num_gkvec(), num_phi);
    mdarray<double_complex, 2> hphi(kp->num_gkvec(), num_phi);
    mdarray<double_complex, 2> ophi(kp->num_gkvec(), num_phi);

    mdarray<double_complex, 2> hmlt(num_phi, num_phi);
    mdarray<double_complex, 2> ovlp(num_phi, num_phi);
    mdarray<double_complex, 2> hmlt_old(num_phi, num_phi);
    mdarray<double_complex, 2> ovlp_old(num_phi, num_phi);
    mdarray<double_complex, 2> evec(num_phi, num_phi);
    std::vector<double> eval(num_bands);
    std::vector<double> eval_old(num_bands, 1e100);
    
    mdarray<double_complex, 2> res(kp->num_gkvec(), num_bands); // residuals

    std::vector<double> res_norm(num_bands); // norm of residuals
    std::vector<double> res_rms(num_bands); // RMS of residual
    std::vector<double> res_e(num_bands);

    generalized_evp* gevp = new generalized_evp_lapack(-1.0);

    bool convergence_by_energy = false;

    int N = 0; // current eigen-value problem size
    int n = num_bands; // number of added residuals

    // trial basis functions
    assert(phi.size(0) == psi.size(0));
    for (int i = 0; i < num_bands; i++) memcpy(&phi(0, i), &psi(0, i), kp->num_gkvec() * sizeof(double_complex));
    
    // start iterative diagonalization
    for (int k = 0; k < max_iter; k++)
    {
        {
            Timer t1("sirius::Band::diag_fv_uspp_cpu|set_gevp");

            // copy old Hamiltonian and overlap
            for (int i = 0; i < N; i++)
            {
                memcpy(&hmlt(0, i), &hmlt_old(0, i), N * sizeof(double_complex));
                memcpy(&ovlp(0, i), &ovlp_old(0, i), N * sizeof(double_complex));
            }

            // apply Hamiltonian and overlap operators to the new basis functions
            apply_h_o_uspp_cpu(kp, veff_it_coarse, pw_ekin, n, &phi(0, N), &hphi(0, N), &ophi(0, N));
            
            // <{phi,res}|H|res>
            blas<cpu>::gemm(2, 0, N + n, n, kp->num_gkvec(), &phi(0, 0), phi.ld(), &hphi(0, N), hphi.ld(), &hmlt(0, N), hmlt.ld());
            
            // <{phi,res}|O|res>
            blas<cpu>::gemm(2, 0, N + n, n, kp->num_gkvec(), &phi(0, 0), phi.ld(), &ophi(0, N), ophi.ld(), &ovlp(0, N), ovlp.ld());
            
            // increase the size of the variation space
            N += n;
            
            // save Hamiltonian and overlap
            for (int i = 0; i < N; i++)
            {
                memcpy(&hmlt_old(0, i), &hmlt(0, i), N * sizeof(double_complex));
                memcpy(&ovlp_old(0, i), &ovlp(0, i), N * sizeof(double_complex));
            }
        }
        
        {
            Timer t2("sirius::Band::diag_fv_uspp_cpu|solve_gevp");
            gevp->solve(N, num_bands, hmlt.get_ptr(), hmlt.ld(), ovlp.get_ptr(), ovlp.ld(), &eval[0], 
                        evec.get_ptr(), evec.ld());
        }

        Timer t3("sirius::Band::diag_fv_uspp_cpu|residuals");
        /* Quantum Espresso way of estimating basis update: residuals for which |e - e_old| > eps 
           are accepted as the additional basis functions */
        if (convergence_by_energy)
        {
            if (k == 0)
            {
                n = num_bands;
                memcpy(&hmlt(0, 0), &evec(0, 0), N * n * sizeof(double_complex));
            }
            else
            {
                n = 0;
                // check eigen-values for convergence
                for (int i = 0; i < num_bands; i++)
                {
                    if (fabs(eval[i] - eval_old[i]) > parameters_.iterative_solver_tolerance())
                    {
                        res_e[n] = eval[i];
                        
                        // use hmlt as a temporary storage for evec
                        memcpy(&hmlt(0, n), &evec(0, i), N * sizeof(double_complex));
 
                        n++;
                    }
                    eval_old[i] = eval[i];
                }
            }

            // if we have unconverged eigen-states
            if (n != 0)
            {
                // compute residuals
                // 1. O\Psi_{i} = O\phi_{mu} * Z_{mu, i}
                blas<cpu>::gemm(0, 0, kp->num_gkvec(), n, N, &ophi(0, 0), ophi.ld(), &hmlt(0, 0), hmlt.ld(), 
                                &res(0, 0), res.ld());
                // 2. multiply O\Psi_{i} with energy
                for (int i = 0; i < n; i++)
                {
                    for (int igk = 0; igk < kp->num_gkvec(); igk++) res(igk, i) *= eval[i];
                }
                // 3. r_{i} = H\Psi_{i} - E_{i}O\Psi_{i}
                blas<cpu>::gemm(0, 0, kp->num_gkvec(), n, N, double_complex(1, 0), &hphi(0, 0), hphi.ld(), 
                                &hmlt(0, 0), hmlt.ld(), double_complex(-1, 0), &res(0, 0), res.ld());

                // apply preconditioner
                #pragma omp parallel for
                for (int i = 0; i < n; i++)
                {
                    // apply preconditioner
                    for (int igk = 0; igk < kp->num_gkvec(); igk++)
                    {
                        //double_complex z = h_diag[igk] - res_e[i] * o_diag[igk];
                        //if (abs(z) < 1e-12) error_local(__FILE__, __LINE__, "problematic division");
                        double d = real(h_diag[igk] - res_e[i] * o_diag[igk]);
                        if (fabs(d) < 1e-12) error_local(__FILE__, __LINE__, "problematic division");
                        res(igk, i) /= d;
                    }
                }
            }
        }
        /* Alternative way to estimate basis update: take residuals with norm > eps */
        else
        {
            // compute residuals
            // 1. O\Psi_{i} = O\phi_{mu} * Z_{mu, i}
            blas<cpu>::gemm(0, 0, kp->num_gkvec(), num_bands, N, &ophi(0, 0), ophi.ld(), &evec(0, 0), evec.ld(), 
                            &res(0, 0), res.ld());
            // 2. multiply O\Psi_{i} with energy
            for (int i = 0; i < num_bands; i++)
            {
                for (int igk = 0; igk < kp->num_gkvec(); igk++) res(igk, i) *= eval[i];
            }
            // 3. r_{i} = H\Psi_{i} - E_{i}O\Psi_{i}
            blas<cpu>::gemm(0, 0, kp->num_gkvec(), num_bands, N, double_complex(1, 0), &hphi(0, 0), hphi.ld(), 
                            &evec(0, 0), evec.ld(), double_complex(-1, 0), &res(0, 0), res.ld());

            // compute norm and apply preconditioner
            #pragma omp parallel for
            for (int i = 0; i < num_bands; i++)
            {
                double r = 0;
                for (int igk = 0; igk < kp->num_gkvec(); igk++) r += real(conj(res(igk, i)) * res(igk, i));
                res_norm[i] = r;
                res_rms[i] = sqrt(r / kp->num_gkvec());
                
                // apply preconditioner
                for (int igk = 0; igk < kp->num_gkvec(); igk++)
                {
                    double_complex z = h_diag[igk] - eval[i] * o_diag[igk];
                    if (abs(z) < 1e-12) error_local(__FILE__, __LINE__, "problematic division");
                    res(igk, i) /= z;
                }
            }

            // check which residuals are converged
            n = 0;
            std::vector< std::pair<double, int> > res_rms_sorted;
            for (int i = 0; i < num_bands; i++)
            {
                res_rms_sorted.push_back(std::pair<double, int>(res_rms[i], i));

                // take the residual if it's norm is above the threshold
                if (res_rms[i] > parameters_.iterative_solver_tolerance()) n++;
            }
            
            if (n > 0 && n < num_bands)
            {
                n = std::max(n, (num_bands - 1) / (k + 1));

                std::sort(res_rms_sorted.begin(), res_rms_sorted.end());

                double tol = res_rms_sorted[num_bands - n].first;

                n = 0;
                for (int i = 0; i < num_bands; i++)
                {
                    // take the residual if it's norm is above the threshold
                    if (res_rms[i] > tol) 
                    {
                        // shift unconverged residuals to the beginning of array
                        if (n != i) memcpy(&res(0, n), &res(0, i), kp->num_gkvec() * sizeof(double_complex));
                        n++;
                    }
                }
            }
 
            //== n = 0;
            //== for (int i = 0; i < num_bands; i++)
            //== {
            //==     // take the residual if it's norm is above the threshold
            //==     if (res_rms[i] > parameters_.iterative_solver_tolerance()) 
            //==     {
            //==         // shift unconverged residuals to the beginning of array
            //==         if (n != i) memcpy(&res(0, n), &res(0, i), kp->num_gkvec() * sizeof(double_complex));
            //==         n++;
            //==     }
            //== }
        }
        t3.stop();

        //if (Platform::mpi_rank() == 0)
        //{
        //    std::cout << "iteration:" << k << ", current eigen-value size = " << N << ", number of added residuals = " << n << std::endl;
        //    //printf("lower and upper eigen-values : %16.8f %16.8f\n", eval[0], eval[num_bands - 1]);
        //}

        // check if we run out of variational space or eigen-vectors are converged or it's a last iteration
        if (N + n > num_phi || n == 0 || k == (max_iter - 1))
        {   
            Timer t3("sirius::Band::diag_fv_uspp_cpu|update_phi");
            // \Psi_{i} = \phi_{mu} * Z_{mu, i}
            blas<cpu>::gemm(0, 0, kp->num_gkvec(), num_bands, N, &phi(0, 0), phi.ld(), &evec(0, 0), evec.ld(), 
                            &psi(0, 0), psi.ld());

            if (n == 0 || k == (max_iter - 1)) // exit the loop if the eigen-vectors are converged or it's a last iteration
            {
                std::cout << "converged in " << k << " iterations" << std::endl;
                break;
            }
            else // otherwise set Psi as a new trial basis
            {
                // use phi as a temporary vector to compute H\Psi
                blas<cpu>::gemm(0, 0, kp->num_gkvec(), num_bands, N, &hphi(0, 0), hphi.ld(), 
                                &evec(0, 0), evec.ld(), &phi(0, 0), phi.ld());
                
                // compute the Hamiltonian matrix: <Psi|H|Psi>
                blas<cpu>::gemm(2, 0, num_bands, num_bands, kp->num_gkvec(), &psi(0, 0), psi.ld(), &phi(0, 0), phi.ld(), 
                                &hmlt_old(0, 0), hmlt_old.ld());
             
                memcpy(hphi.get_ptr(), phi.get_ptr(), num_bands * kp->num_gkvec() * sizeof(double_complex));

                // use phi as a temporary vector to compute O\Psi
                blas<cpu>::gemm(0, 0, kp->num_gkvec(), num_bands, N, &ophi(0, 0), ophi.ld(), 
                                &evec(0, 0), evec.ld(), &phi(0, 0), phi.ld());
                
                // compute the overlap matrix: <Psi|O|Psi>
                blas<cpu>::gemm(2, 0, num_bands, num_bands, kp->num_gkvec(), &psi(0, 0), psi.ld(), &phi(0, 0), phi.ld(), 
                                &ovlp_old(0, 0), ovlp_old.ld());
            
                memcpy(ophi.get_ptr(), phi.get_ptr(), num_bands * kp->num_gkvec() * sizeof(double_complex));
                
                // set new basis functions
                memcpy(phi.get_ptr(), psi.get_ptr(), num_bands * kp->num_gkvec() * sizeof(double_complex));
                N = num_bands;
            }
        }
        // expand variational subspace with new basis vectors obtatined from residuals
        memcpy(&phi(0, N), &res(0, 0), n * kp->num_gkvec() * sizeof(double_complex));
    }

    delete gevp;

    kp->set_fv_eigen_values(&eval[0]);
}

#ifdef _GPU_
void Band::diag_fv_uspp_gpu(K_point* kp, Periodic_function<double>* effective_potential)
{
    Timer t("sirius::Band::diag_fv_uspp_gpu");

    // map effective potential to a corase grid
    std::vector<double> veff_it_coarse(parameters_.reciprocal_lattice()->fft_coarse()->size());
    std::vector<double_complex> veff_pw_coarse(parameters_.reciprocal_lattice()->num_gvec_coarse());

    // take only first num_gvec_coarse plane-wave harmonics; this is enough to apply V_eff to \Psi
    for (int igc = 0; igc < parameters_.reciprocal_lattice()->num_gvec_coarse(); igc++)
    {
        int ig = parameters_.reciprocal_lattice()->gvec_index(igc);
        veff_pw_coarse[igc] = effective_potential->f_pw(ig);
    }
    parameters_.reciprocal_lattice()->fft_coarse()->input(parameters_.reciprocal_lattice()->num_gvec_coarse(), 
                                                          parameters_.reciprocal_lattice()->fft_index_coarse(),
                                                          &veff_pw_coarse[0]);
    parameters_.reciprocal_lattice()->fft_coarse()->transform(1);
    parameters_.reciprocal_lattice()->fft_coarse()->output(&veff_it_coarse[0]);

    // short notation for target wave-functions
    mdarray<double_complex, 2>& psi = kp->fv_states_col();

    // short notation for number of target wave-functions
    int num_bands = parameters_.num_fv_states();     

    // cache kinetic energy,
    std::vector<double> pw_ekin = kp->get_pw_ekin();

    // get diagonal elements for preconditioning
    std::vector<double_complex> h_diag;
    std::vector<double_complex> o_diag;
    get_h_o_diag(kp, effective_potential, pw_ekin, h_diag, o_diag);
    
    int max_iter = 10;
    int num_phi = std::min(4 * num_bands, kp->num_gkvec());

    // big array to store one of the beta, phi, hphi or ophi on the GPU
    mdarray<double_complex, 2> gamma(NULL, kp->num_gkvec(), std::max(num_phi, parameters_.unit_cell()->num_beta_a()));
    gamma.allocate_on_device();

    // small array to store residuals, wave-functions and updates to hphi and ophi
    mdarray<double_complex, 2> kappa(kp->num_gkvec(), 2 * num_bands);
    kappa.allocate_on_device();
    kappa.pin_memory();



    mdarray<double_complex, 2> phi(kp->num_gkvec(), num_phi);
    mdarray<double_complex, 2> hphi(kp->num_gkvec(), num_phi);
    mdarray<double_complex, 2> ophi(kp->num_gkvec(), num_phi);
    
    mdarray<double_complex, 2> hmlt(num_phi, num_phi);
    mdarray<double_complex, 2> ovlp(num_phi, num_phi);
    mdarray<double_complex, 2> hmlt_old(num_phi, num_phi);
    mdarray<double_complex, 2> ovlp_old(num_phi, num_phi);
    
    mdarray<double_complex, 2> evec(num_phi, num_phi);
    evec.allocate_on_device();
    evec.pin_memory();

    std::vector<double> eval(num_bands);
    std::vector<double> eval_old(num_bands, 1e100);
    
    std::vector<double> res_norm(num_bands); // norm of residuals
    std::vector<double> res_rms(num_bands); // RMS of residual
    std::vector<double> res_e(num_bands);

    generalized_evp* gevp = NULL;
    switch (parameters_.eigen_value_solver())
    {
        case lapack:
        {
            gevp = new generalized_evp_lapack(-1.0);
            break;
        }
        #ifdef _MAGMA_
        case magma:
        {
            gevp = new generalized_evp_magma();
            hmlt.pin_memory();
            ovlp.pin_memory();
            break;
        }
        #endif
        default:
        {
            error_local(__FILE__, __LINE__, "eigen value solver is not defined");
        }
    }

    bool convergence_by_energy = false;

    int N = 0; // current eigen-value problem size
    int n = num_bands; // number of added residuals

    // trial basis functions
    assert(phi.size(0) == psi.size(0));
    for (int i = 0; i < num_bands; i++) memcpy(&phi(0, i), &psi(0, i), kp->num_gkvec() * sizeof(double_complex));
    
    // start iterative diagonalization
    for (int k = 0; k < max_iter; k++)
    {
        Timer t1("sirius::Band::diag_fv_uspp_gpu|set_gevp");

        // copy old Hamiltonian and overlap
        for (int i = 0; i < N; i++)
        {
            memcpy(&hmlt(0, i), &hmlt_old(0, i), N * sizeof(double_complex));
            memcpy(&ovlp(0, i), &ovlp_old(0, i), N * sizeof(double_complex));
        }

        // apply Hamiltonian and overlap operators
        apply_h_o_uspp_gpu(kp, veff_it_coarse, pw_ekin, n, gamma, kappa, &phi(0, N), &hphi(0, N), &ophi(0, N));

        // copy all phi to GPU
        cublas_set_matrix(kp->num_gkvec(), n + N, sizeof(double_complex), phi.get_ptr(), phi.ld(), gamma.get_ptr_device(), gamma.ld());

        // temporary storage for Hamiltonian and overlap 
        mdarray<double_complex, 2> tmp(NULL, N + n, n);
        tmp.allocate_on_device();

        // compute the Hamiltonian matrix: <phi|H|phi>
        blas<gpu>::gemm(2, 0, N + n, n, kp->num_gkvec(), gamma.get_ptr_device(), gamma.ld(), 
                        kappa.get_ptr_device(), kappa.ld(), tmp.get_ptr_device(), tmp.ld());

        cublas_get_matrix(N + n, n, sizeof(double_complex), tmp.get_ptr_device(), tmp.ld(), &hmlt(0, N), hmlt.ld());

        // compute overlap matrix <phi|O|phi>
        blas<gpu>::gemm(2, 0, N + n, n, kp->num_gkvec(), gamma.get_ptr_device(), gamma.ld(), 
                        kappa.get_ptr_device(kp->num_gkvec() * n), kappa.ld(), tmp.get_ptr_device(), tmp.ld());

        cublas_get_matrix(N + n, n, sizeof(double_complex), tmp.get_ptr_device(), tmp.ld(), &ovlp(0, N), ovlp.ld());

        tmp.deallocate_on_device();

        // MAGMA works with lower triangular part
        #ifdef _MAGMA_
        for (int i = 0; i < N; i++)
        {
            for (int j = N; j < N + n; j++)
            {
                hmlt(j, i) = conj(hmlt(i, j));
                ovlp(j, i) = conj(ovlp(i, j));
            }
        }
        #endif

        // increase the size of the variation space
        N += n;

        // save Hamiltonian and overlap
        for (int i = 0; i < N; i++)
        {
            memcpy(&hmlt_old(0, i), &hmlt(0, i), N * sizeof(double_complex));
            memcpy(&ovlp_old(0, i), &ovlp(0, i), N * sizeof(double_complex));
        }
        t1.stop();
        
        Timer t2("sirius::Band::diag_fv_uspp_gpu|solve_gevp");
        gevp->solve(N, num_bands, hmlt.get_ptr(), hmlt.ld(), ovlp.get_ptr(), ovlp.ld(), &eval[0], 
                    evec.get_ptr(), evec.ld());
        t2.stop();

        Timer t3("sirius::Band::diag_fv_uspp_gpu|residuals");
        /* Quantum Espresso way of estimating basis update: residuals for which |e - e_old| > eps 
           are accepted as the additional basis functions */
        if (convergence_by_energy)
        {
            //== if (k == 0)
            //== {
            //==     n = num_bands;
            //==     memcpy(&hmlt(0, 0), &evec(0, 0), N * n * sizeof(double_complex));
            //== }
            //== else
            //== {
            //==     n = 0;
            //==     // check eigen-values for convergence
            //==     for (int i = 0; i < num_bands; i++)
            //==     {
            //==         if (fabs(eval[i] - eval_old[i]) > parameters_.iterative_solver_tolerance())
            //==         {
            //==             res_e[n] = eval[i];
            //==             
            //==             // use hmlt as a temporary storage for evec
            //==             memcpy(&hmlt(0, n), &evec(0, i), N * sizeof(double_complex));
 
            //==             n++;
            //==         }
            //==         eval_old[i] = eval[i];
            //==     }
            //== }

            //== // if we have unconverged eigen-states
            //== if (n != 0)
            //== {
            //==     // compute residuals
            //==     // 1. O\Psi_{i} = O\phi_{mu} * Z_{mu, i}
            //==     blas<cpu>::gemm(0, 0, kp->num_gkvec(), n, N, &ophi(0, 0), ophi.ld(), &hmlt(0, 0), hmlt.ld(), 
            //==                     &res(0, 0), res.ld());
            //==     // 2. multiply O\Psi_{i} with energy
            //==     for (int i = 0; i < n; i++)
            //==     {
            //==         for (int igk = 0; igk < kp->num_gkvec(); igk++) res(igk, i) *= eval[i];
            //==     }
            //==     // 3. r_{i} = H\Psi_{i} - E_{i}O\Psi_{i}
            //==     blas<cpu>::gemm(0, 0, kp->num_gkvec(), n, N, double_complex(1, 0), &hphi(0, 0), hphi.ld(), 
            //==                     &hmlt(0, 0), hmlt.ld(), double_complex(-1, 0), &res(0, 0), res.ld());

            //==     // apply preconditioner
            //==     #pragma omp parallel for
            //==     for (int i = 0; i < n; i++)
            //==     {
            //==         // apply preconditioner
            //==         for (int igk = 0; igk < kp->num_gkvec(); igk++)
            //==         {
            //==             //double_complex z = h_diag[igk] - res_e[i] * o_diag[igk];
            //==             //if (abs(z) < 1e-12) error_local(__FILE__, __LINE__, "problematic division");
            //==             double d = real(h_diag[igk] - res_e[i] * o_diag[igk]);
            //==             if (fabs(d) < 1e-12) error_local(__FILE__, __LINE__, "problematic division");
            //==             res(igk, i) /= d;
            //==         }
            //==     }
            //== }
        }
        /* Alternative way to estimate basis update: take residuals with norm > eps */
        else
        {
            cublas_set_matrix(N, N, sizeof(double_complex), evec.get_ptr(), evec.ld(), evec.get_ptr_device(), evec.ld());

            // copy all ophi to GPU
            cublas_set_matrix(kp->num_gkvec(), N, sizeof(double_complex), ophi.get_ptr(), ophi.ld(), gamma.get_ptr_device(), gamma.ld());
            
            // O\Psi_{i} = O\phi_{mu} * Z_{mu, i}
            blas<gpu>::gemm(0, 0, kp->num_gkvec(), num_bands, N, gamma.get_ptr_device(), gamma.ld(), 
                            evec.get_ptr_device(), evec.ld(), kappa.get_ptr_device(), kappa.ld());
        
            mdarray<double, 1> eval_gpu(&eval[0], num_bands);
            eval_gpu.allocate_on_device();
            eval_gpu.copy_to_device();
            // multiply O\Psi_{i} with energy
            scale_matrix_columns_gpu(kp->num_gkvec(), num_bands, kappa.get_ptr_device(), eval_gpu.get_ptr_device());
            eval_gpu.deallocate_on_device();
            
            // copy all hphi to GPU
            cublas_set_matrix(kp->num_gkvec(), N, sizeof(double_complex), hphi.get_ptr(), hphi.ld(), gamma.get_ptr_device(), gamma.ld());
            
            double_complex zone(1, 0);
            double_complex mzone(-1, 0);
            // r_{i} = H\Psi_{i} - E_{i}O\Psi_{i}
            blas<gpu>::gemm(0, 0, kp->num_gkvec(), num_bands, N, &zone, gamma.get_ptr_device(), gamma.ld(), 
                            evec.get_ptr_device(), evec.ld(), &mzone, kappa.get_ptr_device(), kappa.ld());
           
            // copy residuals to the host memory
            cublas_get_matrix(kp->num_gkvec(), num_bands, sizeof(double_complex), kappa.get_ptr_device(), kappa.ld(), 
                              kappa.get_ptr(), kappa.ld());

            Timer t("sirius::Band::diag_fv_uspp_gpu|residuals|cpu_part");
            // compute norm and apply preconditioner
            #pragma omp parallel for
            for (int i = 0; i < num_bands; i++)
            {
                double r = 0;
                for (int igk = 0; igk < kp->num_gkvec(); igk++) r += real(conj(kappa(igk, i)) * kappa(igk, i));
                res_norm[i] = r;
                res_rms[i] = sqrt(r / kp->num_gkvec());
                
                // apply preconditioner
                for (int igk = 0; igk < kp->num_gkvec(); igk++)
                {
                    double_complex z = h_diag[igk] - eval[i] * o_diag[igk];
                    if (abs(z) < 1e-12) error_local(__FILE__, __LINE__, "problematic division");
                    kappa(igk, i) /= z;
                }
            }

            // check which residuals are converged
            n = 0;
            std::vector< std::pair<double, int> > res_rms_sorted;
            for (int i = 0; i < num_bands; i++)
            {
                res_rms_sorted.push_back(std::pair<double, int>(res_rms[i], i));

                // take the residual if it's norm is above the threshold
                if (res_rms[i] > parameters_.iterative_solver_tolerance()) n++;
            }
            
            if (n > 0 && n < num_bands)
            {
                n = std::max(n, (num_bands - 1) / (k + 1));

                std::sort(res_rms_sorted.begin(), res_rms_sorted.end());

                double tol = res_rms_sorted[num_bands - n].first;

                n = 0;
                for (int i = 0; i < num_bands; i++)
                {
                    // take the residual if it's norm is above the threshold
                    if (res_rms[i] > tol) 
                    {
                        // shift unconverged residuals to the beginning of array
                        if (n != i) memcpy(&kappa(0, n), &kappa(0, i), kp->num_gkvec() * sizeof(double_complex));
                        n++;
                    }
                }
            }
            t.stop();
 
            //== n = 0;
            //== for (int i = 0; i < num_bands; i++)
            //== {
            //==     // take the residual if it's norm is above the threshold
            //==     if (res_rms[i] > parameters_.iterative_solver_tolerance()) 
            //==     {
            //==         // shift unconverged residuals to the beginning of array
            //==         if (n != i) memcpy(&res(0, n), &res(0, i), kp->num_gkvec() * sizeof(double_complex));
            //==         n++;
            //==     }
            //== }
        }
        t3.stop();

        //if (Platform::mpi_rank() == 0)
        //{
        //    std::cout << "iteration:" << k << ", current eigen-value size = " << N << ", number of added residuals = " << n << std::endl;
        //    //printf("lower and upper eigen-values : %16.8f %16.8f\n", eval[0], eval[num_bands - 1]);
        //}

        // check if we run out of variational space or eigen-vectors are converged or it's a last iteration
        if (N + n > num_phi || n == 0 || k == (max_iter - 1))
        {   
            Timer t3("sirius::Band::diag_fv_uspp_gpu|update_phi");
            // copy all phi to GPU
            cublas_set_matrix(kp->num_gkvec(), N, sizeof(double_complex), phi.get_ptr(), phi.ld(), 
                              gamma.get_ptr_device(), gamma.ld());
            // \Psi_{i} = \phi_{mu} * Z_{mu, i}
            blas<gpu>::gemm(0, 0, kp->num_gkvec(), num_bands, N, gamma.get_ptr_device(), gamma.ld(), 
                            evec.get_ptr_device(), evec.ld(), kappa.get_ptr_device(), kappa.ld());

            cublas_get_matrix(kp->num_gkvec(), num_bands, sizeof(double_complex), 
                              kappa.get_ptr_device(), kappa.ld(), psi.get_ptr(), psi.ld());
            t3.stop();

            if (n == 0 || k == (max_iter - 1)) // exit the loop if the eigen-vectors are converged or it's a last iteration
            {
                std::cout << "converged in " << k << " iterations" << std::endl;
                break;
            }
            else // otherwise set \Psi as a new trial basis and update related arrays
            {
                Timer t("sirius::Band::diag_fv_uspp_gpu|update_h_o");

                // temporary storage for Hamiltonian and overlap 
                mdarray<double_complex, 2> tmp(NULL, num_bands, num_bands);
                tmp.allocate_on_device();

                // compute H\Psi
                cublas_set_matrix(kp->num_gkvec(), N, sizeof(double_complex), hphi.get_ptr(), hphi.ld(), 
                                  gamma.get_ptr_device(), gamma.ld());

                blas<gpu>::gemm(0, 0, kp->num_gkvec(), num_bands, N, gamma.get_ptr_device(), gamma.ld(), 
                                evec.get_ptr_device(), evec.ld(), kappa.get_ptr_device(kp->num_gkvec() * num_bands), kappa.ld());
                
                // copy H\Psi to host memory
                cublas_get_matrix(kp->num_gkvec(), num_bands, sizeof(double_complex),
                                  kappa.get_ptr_device(kp->num_gkvec() * num_bands), kappa.ld(), 
                                  hphi.get_ptr(), hphi.ld());

                // compute the Hamiltonian matrix: <Psi|H|Psi>
                blas<gpu>::gemm(2, 0, num_bands, num_bands, kp->num_gkvec(), kappa.get_ptr_device(), kappa.ld(), 
                                kappa.get_ptr_device(kp->num_gkvec() * num_bands), kappa.ld(), 
                                tmp.get_ptr_device(), tmp.ld());

                // copy Hamiltonian to host
                cublas_get_matrix(num_bands, num_bands, sizeof(double_complex), tmp.get_ptr_device(), tmp.ld(), 
                                  hmlt_old.get_ptr(), hmlt_old.ld());
                
                // compute O\Psi
                cublas_set_matrix(kp->num_gkvec(), N, sizeof(double_complex), ophi.get_ptr(), ophi.ld(), 
                                  gamma.get_ptr_device(), gamma.ld());

                blas<gpu>::gemm(0, 0, kp->num_gkvec(), num_bands, N, gamma.get_ptr_device(), gamma.ld(), 
                                evec.get_ptr_device(), evec.ld(), kappa.get_ptr_device(kp->num_gkvec() * num_bands), kappa.ld());

                // copy O\Psi to host memory
                cublas_get_matrix(kp->num_gkvec(), num_bands, sizeof(double_complex),
                                  kappa.get_ptr_device(kp->num_gkvec() * num_bands), kappa.ld(), 
                                  ophi.get_ptr(), ophi.ld());

                // compute the overlap matrix: <Psi|O|Psi>
                blas<gpu>::gemm(2, 0, num_bands, num_bands, kp->num_gkvec(), kappa.get_ptr_device(), kappa.ld(), 
                                kappa.get_ptr_device(kp->num_gkvec() * num_bands), kappa.ld(), 
                                tmp.get_ptr_device(), tmp.ld());

                // copy overlap matrix to host
                cublas_get_matrix(num_bands, num_bands, sizeof(double_complex), tmp.get_ptr_device(), tmp.ld(), 
                                  ovlp_old.get_ptr(), ovlp_old.ld());
             
                // update phi with Psi
                memcpy(phi.get_ptr(), psi.get_ptr(), num_bands * kp->num_gkvec() * sizeof(double_complex));

                // new size of eigen-value problem 
                N = num_bands;

                tmp.deallocate_on_device();
            }
        }
        // expand variational space with new preconditioned residuals
        memcpy(&phi(0, N), &kappa(0, 0), n * kp->num_gkvec() * sizeof(double_complex));
    }

    delete gevp;

    kp->set_fv_eigen_values(&eval[0]);
}
#endif

void Band::solve_fv(K_point* kp, Periodic_function<double>* effective_potential)
{
    if (kp->num_gkvec() < parameters_.num_fv_states())
        error_global(__FILE__, __LINE__, "number of G+k vectors is too small");

    switch (parameters_.esm_type())
    {
        case full_potential_pwlo:
        case full_potential_lapwlo:
        {
            diag_fv_full_potential(kp, effective_potential);
            break;
        }
        case ultrasoft_pseudopotential:
        {
            switch (parameters_.processing_unit())
            {
                case cpu:
                {
                    diag_fv_uspp_cpu(kp, effective_potential);
                    break;
                }
                case gpu:
                {
                    #ifdef _GPU_
                    diag_fv_uspp_gpu(kp, effective_potential);
                    #else
                    error_local(__FILE__, __LINE__, "not compiled with GPU support");
                    #endif
                    break;
                }
            }
        }
    }
}


//=====================================================================================================================
// GPU code, (L)APW+lo basis
//=====================================================================================================================
//** #ifdef _GPU_
//** template<> void K_point::set_fv_h_o<gpu, apwlo>(Periodic_function<double>* effective_potential, int num_ranks,
//**                                                mdarray<double_complex, 2>& h, mdarray<double_complex, 2>& o)
//** 
//** {
//**     Timer t("sirius::K_point::set_fv_h_o");
//**     
//**     int apw_offset_col = (num_ranks > 1) ? num_gkvec_row() : 0;
//**     
//**     mdarray<double_complex, 2> alm(num_gkvec_loc(), parameters_.max_mt_aw_basis_size());
//**     mdarray<double_complex, 2> halm(num_gkvec_row(), parameters_.max_mt_aw_basis_size());
//**     
//**     alm.pin_memory();
//**     alm.allocate_on_device();
//**     halm.pin_memory();
//**     halm.allocate_on_device();
//**     h.allocate_on_device();
//**     h.zero_on_device();
//**     o.allocate_on_device();
//**     o.zero_on_device();
//** 
//**     double_complex zone(1, 0);
//**     
//**     for (int ia = 0; ia < parameters_.num_atoms(); ia++)
//**     {
//**         Atom* atom = parameters_.atom(ia);
//**         Atom_type* type = atom->type();
//**         
//**         generate_matching_coefficients(num_gkvec_loc(), ia, alm);
//**         
//**         apply_hmt_to_apw(num_gkvec_row(), ia, alm, halm);
//**         
//**         alm.copy_to_device();
//**         halm.copy_to_device();
//**         
//**         blas<gpu>::gemm(0, 2, num_gkvec_row(), num_gkvec_col(), type->mt_aw_basis_size(), &zone, 
//**                         alm.get_ptr_device(), alm.ld(), &(alm.get_ptr_device()[apw_offset_col]), alm.ld(), 
//**                         &zone, o.get_ptr_device(), o.ld()); 
//**         
//**         blas<gpu>::gemm(0, 2, num_gkvec_row(), num_gkvec_col(), type->mt_aw_basis_size(), &zone, 
//**                         halm.get_ptr_device(), halm.ld(), &(alm.get_ptr_device()[apw_offset_col]), alm.ld(),
//**                         &zone, h.get_ptr_device(), h.ld());
//**             
//**         set_fv_h_o_apw_lo(type, atom, ia, apw_offset_col, alm, h, o);
//**     } //ia
//** 
//**     cublas_get_matrix(num_gkvec_row(), num_gkvec_col(), sizeof(double_complex), h.get_ptr_device(), h.ld(), 
//**                       h.get_ptr(), h.ld());
//**     
//**     cublas_get_matrix(num_gkvec_row(), num_gkvec_col(), sizeof(double_complex), o.get_ptr_device(), o.ld(), 
//**                       o.get_ptr(), o.ld());
//** 
//**     set_fv_h_o_it(effective_potential, h, o);
//** 
//**     set_fv_h_o_lo_lo(h, o);
//** 
//**     h.deallocate_on_device();
//**     o.deallocate_on_device();
//**     alm.deallocate_on_device();
//**     alm.unpin_memory();
//**     alm.deallocate();
//**     halm.deallocate_on_device();
//**     halm.unpin_memory();
//**     halm.deallocate();
//** }
//** 

void Band::set_o_apw_lo(K_point* kp, Atom_type* type, Atom* atom, int ia, mdarray<double_complex, 2>& alm, 
                        mdarray<double_complex, 2>& o)
{
    Timer t("sirius::Band::set_o_apw_lo");
    
    int apw_offset_col = kp->apw_offset_col();
    
    // apw-lo block
    for (int i = 0; i < kp->num_atom_lo_cols(ia); i++)
    {
        int icol = kp->lo_col(ia, i);

        int l = kp->apwlo_basis_descriptors_col(icol).l;
        int lm = kp->apwlo_basis_descriptors_col(icol).lm;
        int order = kp->apwlo_basis_descriptors_col(icol).order;
        
        for (int order1 = 0; order1 < (int)type->aw_descriptor(l).size(); order1++)
        {
            for (int igkloc = 0; igkloc < kp->num_gkvec_row(); igkloc++)
            {
                o(igkloc, icol) += atom->symmetry_class()->o_radial_integral(l, order1, order) * 
                                   alm(igkloc, type->indexb_by_lm_order(lm, order1));
            }
        }
    }

    std::vector<double_complex> ztmp(kp->num_gkvec_col());
    // lo-apw block
    for (int i = 0; i < kp->num_atom_lo_rows(ia); i++)
    {
        int irow = kp->lo_row(ia, i);

        int l = kp->apwlo_basis_descriptors_row(irow).l;
        int lm = kp->apwlo_basis_descriptors_row(irow).lm;
        int order = kp->apwlo_basis_descriptors_row(irow).order;

        for (int order1 = 0; order1 < (int)type->aw_descriptor(l).size(); order1++)
        {
            for (int igkloc = 0; igkloc < kp->num_gkvec_col(); igkloc++)
            {
                o(irow, igkloc) += atom->symmetry_class()->o_radial_integral(l, order, order1) * 
                                   conj(alm(apw_offset_col + igkloc, type->indexb_by_lm_order(lm, order1)));
            }
        }
    }
}

void Band::set_o_it(K_point* kp, mdarray<double_complex, 2>& o)
{
    Timer t("sirius::Band::set_o_it");

    #pragma omp parallel for default(shared)
    for (int igk_col = 0; igk_col < kp->num_gkvec_col(); igk_col++) // loop over columns
    {
        for (int igk_row = 0; igk_row < kp->num_gkvec_row(); igk_row++) // for each column loop over rows
        {
            int ig12 = parameters_.reciprocal_lattice()->index_g12(kp->apwlo_basis_descriptors_row(igk_row).ig,
                                             kp->apwlo_basis_descriptors_col(igk_col).ig);
            
            o(igk_row, igk_col) += parameters_.step_function()->theta_pw(ig12);
        }
    }
}

void Band::set_o_lo_lo(K_point* kp, mdarray<double_complex, 2>& o)
{
    Timer t("sirius::Band::set_o_lo_lo");

    // lo-lo block
    #pragma omp parallel for default(shared)
    for (int icol = kp->num_gkvec_col(); icol < kp->apwlo_basis_size_col(); icol++)
    {
        int ia = kp->apwlo_basis_descriptors_col(icol).ia;
        int lm2 = kp->apwlo_basis_descriptors_col(icol).lm; 

        for (int irow = kp->num_gkvec_row(); irow < kp->apwlo_basis_size_row(); irow++)
        {
            if (ia == kp->apwlo_basis_descriptors_row(irow).ia)
            {
                Atom* atom = parameters_.unit_cell()->atom(ia);
                int lm1 = kp->apwlo_basis_descriptors_row(irow).lm; 

                if (lm1 == lm2)
                {
                    int l = kp->apwlo_basis_descriptors_row(irow).l;
                    int order1 = kp->apwlo_basis_descriptors_row(irow).order; 
                    int order2 = kp->apwlo_basis_descriptors_col(icol).order; 
                    o(irow, icol) += atom->symmetry_class()->o_radial_integral(l, order1, order2);
                }
            }
        }
    }
}

void Band::set_o(K_point* kp, mdarray<double_complex, 2>& o)
{
    Timer t("sirius::Band::set_o");
   
    // index of column apw coefficients in apw array
    int apw_offset_col = kp->apw_offset_col();
    
    mdarray<double_complex, 2> alm(kp->num_gkvec_loc(), parameters_.unit_cell()->max_mt_aw_basis_size());
    o.zero();

    double_complex zone(1, 0);
    
    for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
    {
        Atom* atom = parameters_.unit_cell()->atom(ia);
        Atom_type* type = atom->type();
       
        // generate conjugated coefficients
        kp->generate_matching_coefficients<true>(kp->num_gkvec_loc(), ia, alm);
        
        // generate <apw|apw> block; |ket> is conjugated, so it is "unconjugated" back
        blas<cpu>::gemm(0, 2, kp->num_gkvec_row(), kp->num_gkvec_col(), type->mt_aw_basis_size(), zone, 
                        &alm(0, 0), alm.ld(), &alm(apw_offset_col, 0), alm.ld(), zone, &o(0, 0), o.ld()); 
            
        // setup apw-lo blocks
        set_o_apw_lo(kp, type, atom, ia, alm, o);
    } //ia

    set_o_it(kp, o);

    set_o_lo_lo(kp, o);

    alm.deallocate();
}

void Band::solve_fd(K_point* kp, Periodic_function<double>* effective_potential, 
                    Periodic_function<double>* effective_magnetic_field[3])
{
    Timer t("sirius::Band::solve_fd");

    if (kp->num_ranks() > 1 && (parameters_.eigen_value_solver() == lapack || parameters_.eigen_value_solver() == magma))
        error_local(__FILE__, __LINE__, "Can't use more than one MPI rank for LAPACK or MAGMA eigen-value solver");

    generalized_evp* solver = NULL;

    // create eigen-value solver
    switch (parameters_.eigen_value_solver())
    {
        case lapack:
        {
            solver = new generalized_evp_lapack(-1.0);
            break;
        }
        case scalapack:
        {
            solver = new generalized_evp_scalapack(parameters_.cyclic_block_size(), kp->num_ranks_row(), 
                                                   kp->num_ranks_col(), parameters_.blacs_context(), -1.0);
            break;
        }
        case elpa:
        {
            solver = new generalized_evp_elpa(parameters_.cyclic_block_size(), 
                                              kp->apwlo_basis_size_row(), kp->num_ranks_row(), kp->rank_row(),
                                              kp->apwlo_basis_size_col(), kp->num_ranks_col(), kp->rank_col(), 
                                              parameters_.blacs_context(), 
                                              parameters_.mpi_grid().communicator(1 << _dim_row_),
                                              parameters_.mpi_grid().communicator(1 << _dim_col_),
                                              parameters_.mpi_grid().communicator(1 << _dim_col_ | 1 << _dim_row_));
            break;
        }
        case magma:
        {
            solver = new generalized_evp_magma();
            break;
        }
        default:
        {
            error_local(__FILE__, __LINE__, "eigen value solver is not defined");
        }
    }

    mdarray<double_complex, 2> h(kp->apwlo_basis_size_row(), kp->apwlo_basis_size_col());
    mdarray<double_complex, 2> o(kp->apwlo_basis_size_row(), kp->apwlo_basis_size_col());
    
    set_o(kp, o);

    std::vector<double> eval(parameters_.num_bands());
    mdarray<double_complex, 2>& fd_evec = kp->fd_eigen_vectors();

    if (parameters_.num_mag_dims() == 0)
    {
        assert(kp->apwlo_basis_size() >= parameters_.num_fv_states());
        set_h<nm>(kp, effective_potential, effective_magnetic_field, h);
       
        Timer t2("sirius::Band::solve_fd|diag");
        solver->solve(kp->apwlo_basis_size(), parameters_.num_fv_states(), h.get_ptr(), h.ld(), o.get_ptr(), o.ld(), 
                      &eval[0], fd_evec.get_ptr(), fd_evec.ld());
    }
    
    if (parameters_.num_mag_dims() == 1)
    {
        assert(kp->apwlo_basis_size() >= parameters_.num_fv_states());

        mdarray<double_complex, 2> o1(kp->apwlo_basis_size_row(), kp->apwlo_basis_size_col());
        memcpy(&o1(0, 0), &o(0, 0), o.size() * sizeof(double_complex));

        set_h<uu>(kp, effective_potential, effective_magnetic_field, h);
       
        Timer t2("sirius::Band::solve_fd|diag");
        solver->solve(kp->apwlo_basis_size(), parameters_.num_fv_states(), h.get_ptr(), h.ld(), o.get_ptr(), o.ld(), 
                      &eval[0], &fd_evec(0, 0), fd_evec.ld());
        t2.stop();

        set_h<dd>(kp, effective_potential, effective_magnetic_field, h);
        
        t2.start();
        solver->solve(kp->apwlo_basis_size(), parameters_.num_fv_states(), h.get_ptr(), h.ld(), o1.get_ptr(), o1.ld(), 
                      &eval[parameters_.num_fv_states()], &fd_evec(0, parameters_.spl_fv_states_col().local_size()), fd_evec.ld());
        t2.stop();
    }

    kp->set_band_energies(&eval[0]);

    delete solver;
}

}

