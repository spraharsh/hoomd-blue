/*
Highly Optimized Object-oriented Many-particle Dynamics -- Blue Edition
(HOOMD-blue) Open Source Software License Copyright 2009-2015 The Regents of
the University of Michigan All rights reserved.

HOOMD-blue may contain modifications ("Contributions") provided, and to which
copyright is held, by various Contributors who have granted The Regents of the
University of Michigan the right to modify and/or distribute such Contributions.

You may redistribute, use, and create derivate works of HOOMD-blue, in source
and binary forms, provided you abide by the following conditions:

* Redistributions of source code must retain the above copyright notice, this
list of conditions, and the following disclaimer both in the code and
prominently in any materials provided with the distribution.

* Redistributions in binary form must reproduce the above copyright notice, this
list of conditions, and the following disclaimer in the documentation and/or
other materials provided with the distribution.

* All publications and presentations based on HOOMD-blue, including any reports
or published results obtained, in whole or in part, with HOOMD-blue, will
acknowledge its use according to the terms posted at the time of submission on:
http://codeblue.umich.edu/hoomd-blue/citations.html

* Any electronic documents citing HOOMD-Blue will link to the HOOMD-Blue website:
http://codeblue.umich.edu/hoomd-blue/

* Apart from the above required attributions, neither the name of the copyright
holder nor the names of HOOMD-blue's contributors may be used to endorse or
promote products derived from this software without specific prior written
permission.

Disclaimer

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND/OR ANY
WARRANTIES THAT THIS SOFTWARE IS FREE OF INFRINGEMENT ARE DISCLAIMED.

IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// Maintainer: jglaser

#include "ForceDistanceConstraintGPU.h"
#include "ForceDistanceConstraintGPU.cuh"

#include <cuda_runtime.h>

#include <string.h>

#include <boost/python.hpp>
#include <boost/bind.hpp>

/*! \file ForceDistanceConstraintGPU.cc
    \brief Contains code for the ForceDistanceConstraintGPU class
*/

/*! \param sysdef SystemDefinition containing the ParticleData to compute forces on
*/
ForceDistanceConstraintGPU::ForceDistanceConstraintGPU(boost::shared_ptr<SystemDefinition> sysdef)
        : ForceDistanceConstraint(sysdef), m_cusolver_rf_initialized(false),
        m_nnz_L_tot(0), m_nnz_U_tot(0),
        m_csr_val_L(m_exec_conf), m_csr_rowptr_L(m_exec_conf), m_csr_colind_L(m_exec_conf),
        m_csr_val_U(m_exec_conf), m_csr_rowptr_U(m_exec_conf), m_csr_colind_U(m_exec_conf),
        m_P(m_exec_conf), m_Q(m_exec_conf), m_T(m_exec_conf),
        m_constraints_dirty(true), m_nnz(m_exec_conf), m_nnz_tot(0),
        m_csr_val(m_exec_conf), m_csr_rowptr(m_exec_conf), m_csr_colind(m_exec_conf)
    {
    m_tuner_fill.reset(new Autotuner(32, 1024, 32, 5, 100000, "dist_constraint_fill_matrix_vec", this->m_exec_conf));
    m_tuner_force.reset(new Autotuner(32, 1024, 32, 5, 100000, "dist_constraint_force", this->m_exec_conf));

    // initialize cuSPARSE
    cusparseCreate(&m_cusparse_handle);

    // cusparse matrix descriptor
    cusparseCreateMatDescr(&m_cusparse_mat_descr);
    cusparseSetMatType(m_cusparse_mat_descr,CUSPARSE_MATRIX_TYPE_GENERAL);
    cusparseSetMatIndexBase(m_cusparse_mat_descr,CUSPARSE_INDEX_BASE_ZERO);
    cusparseSetMatDiagType(m_cusparse_mat_descr, CUSPARSE_DIAG_TYPE_NON_UNIT);

    // L matrix
    cusparseCreateMatDescr(&m_cusparse_mat_descr_L);
    cusparseSetMatType(m_cusparse_mat_descr_L,CUSPARSE_MATRIX_TYPE_GENERAL);
    cusparseSetMatIndexBase(m_cusparse_mat_descr_L,CUSPARSE_INDEX_BASE_ZERO);
    cusparseSetMatDiagType(m_cusparse_mat_descr_L, CUSPARSE_DIAG_TYPE_UNIT);

    // U matrix
    cusparseCreateMatDescr(&m_cusparse_mat_descr_U);
    cusparseSetMatType(m_cusparse_mat_descr_U,CUSPARSE_MATRIX_TYPE_GENERAL);
    cusparseSetMatIndexBase(m_cusparse_mat_descr_U,CUSPARSE_INDEX_BASE_ZERO);
    cusparseSetMatDiagType(m_cusparse_mat_descr_U, CUSPARSE_DIAG_TYPE_NON_UNIT);

    // connect to the ConstraintData to recieve notifications when constraints change order in memory
    m_constraints_dirty_connection = m_cdata->connectGroupsDirty(boost::bind(&ForceDistanceConstraintGPU::slotConstraintsDirty, this));
    }

//! Destructor
ForceDistanceConstraintGPU::~ForceDistanceConstraintGPU()
    {
    // clean up cusparse
    cusparseDestroy(m_cusparse_handle);
    cusparseDestroyMatDescr(m_cusparse_mat_descr);

    if (m_cusolver_rf_initialized)
        {
        cusolverRfDestroy(m_cusolver_rf_handle);
        }

    cusparseDestroyMatDescr(m_cusparse_mat_descr_L);
    cusparseDestroyMatDescr(m_cusparse_mat_descr_U);

    // disconnect from signal in ConstaraintData
    m_constraints_dirty_connection.disconnect();
    }

void ForceDistanceConstraintGPU::fillMatrixVector(unsigned int timestep)
    {
    if (m_prof)
        m_prof->push(m_exec_conf, "fill matrix");

    // fill the matrix in row-major order
    unsigned int n_constraint = m_cdata->getN();

    // access particle data
    ArrayHandle<Scalar4> d_pos(m_pdata->getPositions(), access_location::device, access_mode::read);
    ArrayHandle<Scalar4> d_vel(m_pdata->getVelocities(), access_location::device, access_mode::read);
    ArrayHandle<unsigned int> d_rtag(m_pdata->getRTags(), access_location::device, access_mode::read);
    ArrayHandle<Scalar4> d_netforce(m_pdata->getNetForce(), access_location::device, access_mode::read);

        {
        // access matrix elements
        ArrayHandle<double> d_cmatrix(m_cmatrix, access_location::device, access_mode::overwrite);
        ArrayHandle<double> d_cvec(m_cvec, access_location::device, access_mode::overwrite);

        // access GPU constraint table on device
        const GPUArray<BondData::members_t>& gpu_constraint_list = this->m_cdata->getGPUTable();
        const Index2D& gpu_table_indexer = this->m_cdata->getGPUTableIndexer();

        ArrayHandle<BondData::members_t> d_gpu_clist(gpu_constraint_list, access_location::device, access_mode::read);
        ArrayHandle<unsigned int > d_gpu_n_constraints(this->m_cdata->getNGroupsArray(),
                                                 access_location::device, access_mode::read);
        ArrayHandle<unsigned int> d_gpu_cpos(m_cdata->getGPUPosTable(), access_location::device, access_mode::read);
        ArrayHandle<typeval_t> d_group_typeval(m_cdata->getTypeValArray(), access_location::device, access_mode::read);

        // launch GPU kernel
        m_tuner_fill->begin();
        gpu_fill_matrix_vector(
            n_constraint,
            m_pdata->getN(),
            d_cmatrix.data,
            d_cvec.data,
            d_pos.data,
            d_vel.data,
            d_netforce.data,
            d_gpu_clist.data,
            gpu_table_indexer,
            d_gpu_n_constraints.data,
            d_gpu_cpos.data,
            d_group_typeval.data,
            m_deltaT,
            m_pdata->getBox(),
            m_tuner_fill->getParam());

        if (m_exec_conf->isCUDAErrorCheckingEnabled())
            CHECK_CUDA_ERROR();

        m_tuner_fill->end();
        }

    if (m_prof)
        m_prof->pop(m_exec_conf);
    }

void ForceDistanceConstraintGPU::computeConstraintForces(unsigned int timestep)
    {
    if (m_prof)
        m_prof->push(m_exec_conf,"constraint forces");

    unsigned int n_constraint = m_cdata->getN();

    // reallocate array of constraint forces
    m_lagrange.resize(n_constraint);

    // resize sparse matrix storage
    m_nnz.resize(n_constraint);
    m_csr_rowptr.resize(n_constraint+1);
    m_csr_colind.resize(n_constraint*n_constraint);
    m_csr_val.resize(n_constraint*n_constraint);

    // ==1 if the sparsity pattern of the matrix changes (in particular if connectivity changes)
    unsigned int sparsity_pattern_changed = 0;
        {
        // access matrix and vector
        ArrayHandle<double> d_cmatrix(m_cmatrix, access_location::device, access_mode::read);
        ArrayHandle<double> d_cvec(m_cvec, access_location::device, access_mode::read);

        // access sparse matrix structural data
        ArrayHandle<int> d_nnz(m_nnz, access_location::device, access_mode::overwrite);
        ArrayHandle<int> d_csr_colind(m_csr_colind, access_location::device, access_mode::overwrite);
        ArrayHandle<int> d_csr_rowptr(m_csr_rowptr, access_location::device, access_mode::overwrite);
        ArrayHandle<double> d_csr_val(m_csr_val, access_location::device, access_mode::overwrite);

        // count zeros and convert matrix
        gpu_dense2sparse(n_constraint,
            d_cmatrix.data,
            d_nnz.data,
            m_nnz_tot,
            m_cusparse_handle,
            m_cusparse_mat_descr,
            d_csr_rowptr.data,
            d_csr_colind.data,
            d_csr_val.data,
            sparsity_pattern_changed);
        }

    sparsity_pattern_changed |= m_constraints_dirty;

    // reset flag
    m_constraints_dirty = false;

    if (sparsity_pattern_changed)
        {
        m_exec_conf->msg->notice(6) << "ForceDistanceConstraintGPU: constraint matrix changed. Setting up cuSolver" << std::endl;

        /*
         * re-initialize sparse matrix solver on host
         */

        // access sparse matrix on host
        ArrayHandle<int> h_csr_colind(m_csr_colind, access_location::host, access_mode::read);
        ArrayHandle<int> h_csr_rowptr(m_csr_rowptr, access_location::host, access_mode::read);
        ArrayHandle<double> h_csr_val(m_csr_val, access_location::host, access_mode::read);

        // resize permutation matrix
        m_Qreorder.resize(n_constraint);

        // initialize cuSolver
        cusolverSpCreate(&m_cusolver_sp_handle);

        // determine reordering matrixQ to minimize zero fill-in
        cusolverSpXcsrsymrcmHost(m_cusolver_sp_handle, n_constraint, m_nnz_tot,
            m_cusparse_mat_descr, h_csr_rowptr.data, h_csr_colind.data,
            &m_Qreorder.front());

        // get size of scratch space for permutation B=Q*A*Q^T
        size_t size_perm = 0;
        cusolverSpXcsrperm_bufferSizeHost(
                m_cusolver_sp_handle, n_constraint, n_constraint, m_nnz_tot,
                m_cusparse_mat_descr, h_csr_rowptr.data, h_csr_colind.data,
                &m_Qreorder.front(), &m_Qreorder.front(),
                &size_perm);

        // identity mapping
        m_mapBfromA.resize(m_nnz_tot);
        for (unsigned int i = 0; i < m_nnz_tot; ++i)
            {
            m_mapBfromA[i] = i;
            }

        // allocate scratch space
        m_reorder_work.resize(size_perm);

        // resize B matrix
        m_csr_rowptr_B.resize(n_constraint+1);
        m_csr_colind_B.resize(m_nnz_tot);
        m_csr_val_B.resize(m_nnz_tot);

        // copy over A values
        memcpy(&m_csr_rowptr_B.front(), h_csr_rowptr.data, sizeof(int)*(n_constraint+1));
        memcpy(&m_csr_colind_B.front(), h_csr_colind.data, sizeof(int)*m_nnz_tot);

        // apply permutation
        cusolverSpXcsrpermHost(m_cusolver_sp_handle, n_constraint, n_constraint, m_nnz_tot,
            m_cusparse_mat_descr, &m_csr_rowptr_B.front(), &m_csr_colind_B.front(), &m_Qreorder.front(),
            &m_Qreorder.front(), &m_mapBfromA.front(), &m_reorder_work.front());

        // B = A(mapBfromA)
        for (int i = 0; i < m_nnz_tot; ++i)
            {
            m_csr_val_B[i] = h_csr_val.data[ m_mapBfromA[i] ];
            }

        /*
         * solve A*x = b using LU(B)
         */

        // create data structure for LU factorization
        cusolverSpCreateCsrluInfoHost(&m_cusolver_csrlu_info);

        // analyze
        cusolverSpXcsrluAnalysisHost(m_cusolver_sp_handle, n_constraint, m_nnz_tot,
            m_cusparse_mat_descr, &m_csr_rowptr_B.front(), &m_csr_colind_B.front(), m_cusolver_csrlu_info);

        size_t size_internal = 0;
        size_t size_lu = 0;

        // workspace
        cusolverSpDcsrluBufferInfoHost(m_cusolver_sp_handle, n_constraint, m_nnz_tot,
            m_cusparse_mat_descr, &m_csr_val_B.front(), &m_csr_rowptr_B.front(), &m_csr_colind_B.front(),
            m_cusolver_csrlu_info, &size_internal, &size_lu);

        // reallocate
        m_lu_work.resize(size_lu);

        // solve
        const double pivot_thresh(1.0);

        cusolverSpDcsrluFactorHost(m_cusolver_sp_handle, n_constraint, m_nnz_tot, m_cusparse_mat_descr,
            &m_csr_val_B.front(), &m_csr_rowptr_B.front(), &m_csr_colind_B.front(),
            m_cusolver_csrlu_info, pivot_thresh, &m_lu_work.front());

        // check for singularity
        const double tol(1e-14);
        int singularity = 0;

        cusolverSpDcsrluZeroPivotHost(m_cusolver_sp_handle, m_cusolver_csrlu_info, tol, &singularity);

        if (0 <= singularity)
            {
            m_exec_conf->msg->error() << "Singular constraint matrix." << std::endl;
            throw std::runtime_error("Error computing constraint forces\n");
            }

        /*
         * extract P, Q, L and U from P*B*Q^T = L*U
         * L has unit diagonal
         */
        cusolverSpXcsrluNnzHost(m_cusolver_sp_handle, &m_nnz_L_tot, &m_nnz_U_tot, m_cusolver_csrlu_info);

        // reallocate
        m_Plu.resize(n_constraint);
        m_Qlu.resize(n_constraint);

        m_csr_val_L.resize(m_nnz_L_tot);
        m_csr_rowptr_L.resize(n_constraint+1);
        m_csr_colind_L.resize(m_nnz_L_tot);

        m_csr_val_U.resize(m_nnz_U_tot);
        m_csr_rowptr_U.resize(n_constraint+1);
        m_csr_colind_U.resize(m_nnz_U_tot);

        // access L, U sparse matrices on host
        ArrayHandle<double> h_csr_val_L(m_csr_val_L, access_location::host, access_mode::overwrite);
        ArrayHandle<int> h_csr_rowptr_L(m_csr_rowptr_L, access_location::host, access_mode::overwrite);
        ArrayHandle<int> h_csr_colind_L(m_csr_colind_L, access_location::host, access_mode::overwrite);

        ArrayHandle<double> h_csr_val_U(m_csr_val_U, access_location::host, access_mode::overwrite);
        ArrayHandle<int> h_csr_rowptr_U(m_csr_rowptr_U, access_location::host, access_mode::overwrite);
        ArrayHandle<int> h_csr_colind_U(m_csr_colind_U, access_location::host, access_mode::overwrite);

        // extract
        cusolverStatus_t stat;
        stat = cusolverSpDcsrluExtractHost(m_cusolver_sp_handle, &m_Plu.front(), &m_Qlu.front(),
            m_cusparse_mat_descr_L, h_csr_val_L.data, h_csr_rowptr_L.data, h_csr_colind_L.data,
            m_cusparse_mat_descr_U, h_csr_val_U.data, h_csr_rowptr_U.data, h_csr_colind_U.data,
            m_cusolver_csrlu_info, &m_lu_work.front());

        // clean up cusolverSp
        cusolverSpDestroyCsrluInfoHost(m_cusolver_csrlu_info);
        cusolverSpDestroy(m_cusolver_sp_handle);

        /* P = Plu*Qreorder
         * Q = Qlu*Qreorder
         *
         * then the factorization is complete (P*A*Q^T = L*U)
         */

        // reallocate
        m_P.resize(n_constraint);
        m_Q.resize(n_constraint);

        // access P,Q
        ArrayHandle<int> h_P(m_P, access_location::host, access_mode::overwrite);
        ArrayHandle<int> h_Q(m_Q, access_location::host, access_mode::overwrite);

        // P = Plu*Qreorder
        // Q = Qlu*Qreorder
        for (int i = 0; i < n_constraint; ++i)
            {
            h_P.data[i] = m_Qreorder[m_Plu[i]];
            h_Q.data[i] = m_Qreorder[m_Qlu[i]];
            }

        if (!m_cusolver_rf_initialized)
            {
            // initialize cusolverRF
            cusolverRfCreate(&m_cusolver_rf_handle);
            m_cusolver_rf_initialized = true;

            // set parameters
            // nzero is the value below which zero pivot is flagged.
            // nboost is the value which is substitured for zero pivot.
            double nzero = 0.0;
            double nboost= 0.0;

            cusolverRfSetNumericProperties(m_cusolver_rf_handle, nzero, nboost);

            // choose algorithm
            const cusolverRfFactorization_t fact_alg = CUSOLVERRF_FACTORIZATION_ALG0; // default
            const cusolverRfTriangularSolve_t solve_alg = CUSOLVERRF_TRIANGULAR_SOLVE_ALG1; // default

            cusolverRfSetAlgs(m_cusolver_rf_handle, fact_alg, solve_alg);

            // matrix mode: L and U are CSR, L has implicit unit diagonal
            cusolverRfSetMatrixFormat(m_cusolver_rf_handle, CUSOLVERRF_MATRIX_FORMAT_CSR,
                CUSOLVERRF_UNIT_DIAGONAL_ASSUMED_L);

            // fast mode for matrix assembling (I found it to crash on my notebook)
            //cusolverRfSetResetValuesFastMode(m_cusolver_rf_handle, CUSOLVERRF_RESET_VALUES_FAST_MODE_ON);
            }

        /*
         * assemble P*A*Q = L*U
         */
        cusolverRfSetupHost(n_constraint, m_nnz_tot, h_csr_rowptr.data, h_csr_colind.data, h_csr_val.data,
            m_nnz_L_tot, h_csr_rowptr_L.data, h_csr_colind_L.data, h_csr_val_L.data,
            m_nnz_U_tot, h_csr_rowptr_U.data, h_csr_colind_U.data, h_csr_val_U.data,
            h_P.data,
            h_Q.data,
            m_cusolver_rf_handle);

        /*
         * analyze sparsity pattern
         */
        cusolverRfAnalyze(m_cusolver_rf_handle);
        } // end if sparsity pattern changed

    // reallocate work space for cusolverRf
    m_T.resize(n_constraint);

        {
        // access sparse matrix structural data
        ArrayHandle<int> d_csr_colind(m_csr_colind, access_location::device, access_mode::read);
        ArrayHandle<int> d_csr_rowptr(m_csr_rowptr, access_location::device, access_mode::read);
        ArrayHandle<double> d_csr_val(m_csr_val, access_location::device, access_mode::read);

        // permutations
        ArrayHandle<int> d_P(m_P, access_location::device, access_mode::read);
        ArrayHandle<int> d_Q(m_Q, access_location::device, access_mode::read);

        // import matrix to cusolverRf
        cusolverRfResetValues(n_constraint, m_nnz_tot, d_csr_rowptr.data, d_csr_colind.data, d_csr_val.data,
            d_P.data, d_Q.data, m_cusolver_rf_handle);

        // refactor using updated values
        cusolverRfRefactor(m_cusolver_rf_handle);

        // solve A*x = b

        // access work space
        ArrayHandle<double> d_T(m_T, access_location::device, access_mode::readwrite);

        // access solution vector
        ArrayHandle<double> d_lagrange(m_lagrange, access_location::device, access_mode::overwrite);

        // copy RHS into solution vector
        ArrayHandle<double> d_vec(m_cvec, access_location::device, access_mode::read);
        cudaMemcpy(d_lagrange.data, d_vec.data, sizeof(double)*n_constraint,cudaMemcpyDeviceToDevice);

        int nrhs = 1;
        cusolverRfSolve(m_cusolver_rf_handle, d_P.data, d_Q.data, nrhs, d_T.data, n_constraint, d_lagrange.data,
            n_constraint);

        // access particle data arrays
        ArrayHandle<Scalar4> d_pos(m_pdata->getPositions(), access_location::device, access_mode::read);

        // access force array
        ArrayHandle<Scalar4> d_force(m_force, access_location::device, access_mode::overwrite);

        // access GPU constraint table on device
        const GPUArray<BondData::members_t>& gpu_constraint_list = this->m_cdata->getGPUTable();
        const Index2D& gpu_table_indexer = this->m_cdata->getGPUTableIndexer();

        ArrayHandle<BondData::members_t> d_gpu_clist(gpu_constraint_list, access_location::device, access_mode::read);
        ArrayHandle<unsigned int > d_gpu_n_constraints(this->m_cdata->getNGroupsArray(),
                                                 access_location::device, access_mode::read);
        ArrayHandle<unsigned int> d_gpu_cpos(m_cdata->getGPUPosTable(), access_location::device, access_mode::read);

        const BoxDim& box = m_pdata->getBox();

        unsigned int n_ptl = m_pdata->getN();

        // compute constraint forces by solving linear system of equations
        m_tuner_force->begin();
        gpu_compute_constraint_forces(d_pos.data,
            d_gpu_clist.data,
            gpu_table_indexer,
            d_gpu_n_constraints.data,
            d_gpu_cpos.data,
            d_force.data,
            box,
            n_ptl,
            m_tuner_force->getParam(),
            d_lagrange.data);

        if (m_exec_conf->isCUDAErrorCheckingEnabled())
            CHECK_CUDA_ERROR();

            m_tuner_force->end();
        }

    if (m_prof)
        m_prof->pop(m_exec_conf);

    }

void export_ForceDistanceConstraintGPU()
    {
    class_< ForceDistanceConstraintGPU, boost::shared_ptr<ForceDistanceConstraintGPU>, bases<ForceConstraint>, boost::noncopyable >
    ("ForceDistanceConstraintGPU", init< boost::shared_ptr<SystemDefinition> >())
    ;
    }
