/*
Highly Optimized Object-oriented Many-particle Dynamics -- Blue Edition
(HOOMD-blue) Open Source Software License Copyright 2009-2016 The Regents of
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

#include "TwoStepNPTMTKGPU.h"
#include "TwoStepNPTMTKGPU.cuh"

#include "TwoStepNVEGPU.cuh"

#ifdef ENABLE_MPI
#include "hoomd/Communicator.h"
#include "hoomd/HOOMDMPI.h"
#endif

#include <boost/python.hpp>
using namespace boost::python;
using namespace std;

/*! \file TwoStepNPTMTKGPU.h
    \brief Contains code for the TwoStepNPTMTKGPU class
*/

/*! \param sysdef SystemDefinition this method will act on. Must not be NULL.
    \param group The group of particles this integration method is to work on
    \param thermo_group ComputeThermo to compute thermo properties of the integrated \a group
    \param thermo_group ComputeThermo to compute thermo properties of the integrated \a group at full time step
    \param tau NPT temperature period
    \param tauP NPT pressure period
    \param T Temperature set point
    \param P Pressure set point
    \param couple Coupling mode
    \param flags Barostatted simulation box degrees of freedom
*/
TwoStepNPTMTKGPU::TwoStepNPTMTKGPU(boost::shared_ptr<SystemDefinition> sysdef,
                       boost::shared_ptr<ParticleGroup> group,
                       boost::shared_ptr<ComputeThermo> thermo_group,
                       boost::shared_ptr<ComputeThermo> thermo_group_t,
                       Scalar tau,
                       Scalar tauP,
                       boost::shared_ptr<Variant> T,
                       boost::shared_ptr<Variant> P,
                       couplingMode couple,
                       unsigned int flags,
                       const bool nph)

    : TwoStepNPTMTK(sysdef, group, thermo_group, thermo_group_t, tau, tauP, T, P, couple, flags,nph)
    {
    if (!m_exec_conf->isCUDAEnabled())
        {
        m_exec_conf->msg->error() << "Creating a TwoStepNPTMTKGPU with CUDA disabled" << endl;
        throw std::runtime_error("Error initializing TwoStepNPTMTKGPU");
        }

    m_exec_conf->msg->notice(5) << "Constructing TwoStepNPTMTKGPU" << endl;

    m_reduction_block_size = 512;

    // this breaks memory scaling (calculate memory requirements from global group size)
    // unless we reallocate memory with every change of the maximum particle number
    m_num_blocks = m_group->getNumMembersGlobal() / m_reduction_block_size + 1;
    GPUArray< Scalar > scratch(m_num_blocks, m_exec_conf);
    m_scratch.swap(scratch);

    GPUArray< Scalar> temperature(1, m_exec_conf);
    m_temperature.swap(temperature);
    }

TwoStepNPTMTKGPU::~TwoStepNPTMTKGPU()
    {
    m_exec_conf->msg->notice(5) << "Destroying TwoStepNPTMTKGPU" << endl;
    }

/*! \param timestep Current time step
    \post Particle positions are moved forward to timestep+1 and velocities to timestep+1/2 per the Nose-Hoover
     thermostat and Anderson barostat
*/
void TwoStepNPTMTKGPU::integrateStepOne(unsigned int timestep)
    {
    if (m_group->getNumMembersGlobal() == 0)
        {
        m_exec_conf->msg->error() << "integrate.npt(): Integration group empty." << std::endl;
        throw std::runtime_error("Error during NPT integration.");
        }

    unsigned int group_size = m_group->getNumMembers();

    // profile this step
    if (m_prof)
        m_prof->push("NPT step 1");

    // update degrees of freedom for MTK term
    m_ndof = m_thermo_group->getNDOF();

    // advance barostat (nuxx, nuyy, nuzz) half a time step
    advanceBarostat(timestep);

    IntegratorVariables v = getIntegratorVariables();
    Scalar nuxx = v.variable[2];  // Barostat tensor, xx component
    Scalar nuxy = v.variable[3];  // Barostat tensor, xy component
    Scalar nuxz = v.variable[4];  // Barostat tensor, xz component
    Scalar nuyy = v.variable[5];  // Barostat tensor, yy component
    Scalar nuyz = v.variable[6];  // Barostat tensor, yz component
    Scalar nuzz = v.variable[7];  // Barostat tensor, zz component

    // Martyna-Tobias-Klein correction
    Scalar mtk = (nuxx+nuyy+nuzz)/(Scalar)m_ndof;

    // update the propagator matrix using current barostat momenta
    updatePropagator(nuxx, nuxy, nuxz, nuyy, nuyz, nuzz);

    // advance box lengths
    BoxDim global_box = m_pdata->getGlobalBox();
    Scalar3 a = global_box.getLatticeVector(0);
    Scalar3 b = global_box.getLatticeVector(1);
    Scalar3 c = global_box.getLatticeVector(2);

    // (a,b,c) are the columns of the (upper triangular) cell parameter matrix
    // multiply with upper triangular matrix
    a.x = m_mat_exp_r[0] * a.x;
    b.x = m_mat_exp_r[0] * b.x + m_mat_exp_r[1] * b.y;
    b.y = m_mat_exp_r[3] * b.y;
    c.x = m_mat_exp_r[0] * c.x + m_mat_exp_r[1] * c.y + m_mat_exp_r[2] * c.z;
    c.y = m_mat_exp_r[3] * c.y + m_mat_exp_r[4] * c.z;
    c.z = m_mat_exp_r[5] * c.z;

    // update box dimensions
    bool twod = m_sysdef->getNDimensions()==2;

    global_box.setL(make_scalar3(a.x,b.y,c.z));
    Scalar xy = b.x/b.y;

    Scalar xz(0.0);
    Scalar yz(0.0);

    if (!twod)
        {
        xz = c.x/c.z;
        yz = c.y/c.z;
        }

    global_box.setTiltFactors(xy, xz, yz);

    // set global box
    m_pdata->setGlobalBox(global_box);
    m_V = global_box.getVolume(twod);  // volume

    // update the propagator matrix
    updatePropagator(nuxx, nuxy, nuxz, nuyy, nuyz, nuzz);

    if (m_rescale_all)
        {
        ArrayHandle<Scalar4> d_pos(m_pdata->getPositions(), access_location::device, access_mode::readwrite);

        // perform the particle update on the GPU
        gpu_npt_mtk_rescale(m_pdata->getN(),
                             d_pos.data,
                             m_mat_exp_r[0],
                             m_mat_exp_r[1],
                             m_mat_exp_r[2],
                             m_mat_exp_r[3],
                             m_mat_exp_r[4],
                             m_mat_exp_r[5]);

        if (m_exec_conf->isCUDAErrorCheckingEnabled())
            CHECK_CUDA_ERROR();
        }

        {
        ArrayHandle<Scalar4> d_vel(m_pdata->getVelocities(), access_location::device, access_mode::readwrite);
        ArrayHandle<Scalar3> d_accel(m_pdata->getAccelerations(), access_location::device, access_mode::read);
        ArrayHandle<Scalar4> d_pos(m_pdata->getPositions(), access_location::device, access_mode::readwrite);

        ArrayHandle< unsigned int > d_index_array(m_group->getIndexArray(), access_location::device, access_mode::read);

        // precompute loop invariant quantity
        Scalar xi_trans = v.variable[1];
        Scalar exp_thermo_fac = exp(-Scalar(1.0/2.0)*(xi_trans+mtk)*m_deltaT);

        // perform the particle update on the GPU
        gpu_npt_mtk_step_one(d_pos.data,
                             d_vel.data,
                             d_accel.data,
                             d_index_array.data,
                             group_size,
                             exp_thermo_fac,
                             m_mat_exp_v,
                             m_mat_exp_r,
                             m_mat_exp_r_int,
                             m_deltaT,
                             m_rescale_all);

        if (m_exec_conf->isCUDAErrorCheckingEnabled())
            CHECK_CUDA_ERROR();

        } // end of GPUArray scope

    // Get new (local) box lengths
    BoxDim box = m_pdata->getBox();

        {
        ArrayHandle<Scalar4> d_pos(m_pdata->getPositions(), access_location::device, access_mode::readwrite);
        ArrayHandle<int3> d_image(m_pdata->getImages(), access_location::device, access_mode::readwrite);

        // Wrap particles
        gpu_npt_mtk_wrap(m_pdata->getN(),
                         d_pos.data,
                         d_image.data,
                         box);
        }

    if (m_aniso)
        {
        // first part of angular update
        ArrayHandle<Scalar4> d_orientation(m_pdata->getOrientationArray(), access_location::device, access_mode::readwrite);
        ArrayHandle<Scalar4> d_angmom(m_pdata->getAngularMomentumArray(), access_location::device, access_mode::readwrite);
        ArrayHandle<Scalar4> d_net_torque(m_pdata->getNetTorqueArray(), access_location::device, access_mode::read);
        ArrayHandle<Scalar3> d_inertia(m_pdata->getMomentsOfInertiaArray(), access_location::device, access_mode::read);
        ArrayHandle< unsigned int > d_index_array(m_group->getIndexArray(), access_location::device, access_mode::read);

        // precompute loop invariant quantity
        Scalar xi_rot = v.variable[8];
        Scalar exp_thermo_fac_rot = exp(-(xi_rot+mtk)*m_deltaT/Scalar(2.0));

        gpu_nve_angular_step_one(d_orientation.data,
                                 d_angmom.data,
                                 d_inertia.data,
                                 d_net_torque.data,
                                 d_index_array.data,
                                 group_size,
                                 m_deltaT,
                                 exp_thermo_fac_rot);

        if (m_exec_conf->isCUDAErrorCheckingEnabled())
            CHECK_CUDA_ERROR();
        }

    if (! m_nph)
        {
        // propagate thermostat variables forward
        advanceThermostat(timestep);
        }

    #ifdef ENABLE_MPI
    if (m_comm)
        {
        // broadcast integrator variables from rank 0 to other processors
        v = getIntegratorVariables();
        MPI_Bcast(&v.variable.front(), 10, MPI_HOOMD_SCALAR, 0, m_exec_conf->getMPICommunicator());
        setIntegratorVariables(v);
        }
    #endif

    // done profiling
    if (m_prof)
        m_prof->pop();

    }

/*! \param timestep Current time step
    \post particle velocities are moved forward to timestep+1
*/
void TwoStepNPTMTKGPU::integrateStepTwo(unsigned int timestep)
    {
    unsigned int group_size = m_group->getNumMembers();

    const GPUArray< Scalar4 >& net_force = m_pdata->getNetForce();

    // profile this step
    if (m_prof)
        m_prof->push("NPT step 2");


    IntegratorVariables v = getIntegratorVariables();
    Scalar nuxx = v.variable[2];  // Barostat tensor, xx component
    Scalar nuyy = v.variable[5];  // Barostat tensor, yy component
    Scalar nuzz = v.variable[7];  // Barostat tensor, zz component

    // Martyna-Tobias-Klein correction
    Scalar mtk = (nuxx+nuyy+nuzz)/(Scalar)m_ndof;

    {
    ArrayHandle<Scalar4> d_vel(m_pdata->getVelocities(), access_location::device, access_mode::readwrite);
    ArrayHandle<Scalar3> d_accel(m_pdata->getAccelerations(), access_location::device, access_mode::overwrite);

    ArrayHandle<Scalar4> d_net_force(net_force, access_location::device, access_mode::read);
    ArrayHandle< unsigned int > d_index_array(m_group->getIndexArray(), access_location::device, access_mode::read);

    // precompute loop invariant quantity
    Scalar xi_trans = v.variable[1];
    Scalar exp_thermo_fac = exp(-Scalar(1.0/2.0)*(xi_trans+mtk)*m_deltaT);

    // perform second half step of NPT integration (update velocities and accelerations)
    gpu_npt_mtk_step_two(d_vel.data,
                     d_accel.data,
                     d_index_array.data,
                     group_size,
                     d_net_force.data,
                     m_mat_exp_v,
                     m_deltaT,
                     exp_thermo_fac);

    if (m_exec_conf->isCUDAErrorCheckingEnabled())
        CHECK_CUDA_ERROR();

    } // end GPUArray scope

    if (m_aniso)
        {
        // apply angular (NO_SQUISH) equations of motion
        ArrayHandle<Scalar4> d_orientation(m_pdata->getOrientationArray(), access_location::device, access_mode::read);
        ArrayHandle<Scalar4> d_angmom(m_pdata->getAngularMomentumArray(), access_location::device, access_mode::readwrite);
        ArrayHandle<Scalar4> d_net_torque(m_pdata->getNetTorqueArray(), access_location::device, access_mode::read);
        ArrayHandle<Scalar3> d_inertia(m_pdata->getMomentsOfInertiaArray(), access_location::device, access_mode::read);
        ArrayHandle< unsigned int > d_index_array(m_group->getIndexArray(), access_location::device, access_mode::read);

        // precompute loop invariant quantity
        Scalar xi_rot = v.variable[8];
        Scalar exp_thermo_fac_rot = exp(-(xi_rot+mtk)*m_deltaT/Scalar(2.0));

        gpu_nve_angular_step_two(d_orientation.data,
                                 d_angmom.data,
                                 d_inertia.data,
                                 d_net_torque.data,
                                 d_index_array.data,
                                 group_size,
                                 m_deltaT,
                                 exp_thermo_fac_rot);

        if (m_exec_conf->isCUDAErrorCheckingEnabled())
            CHECK_CUDA_ERROR();
        }

    // advance barostat (nuxx, nuyy, nuzz) half a time step
    advanceBarostat(timestep+1);

    // done profiling
    if (m_prof)
        m_prof->pop();
    }


void export_TwoStepNPTMTKGPU()
    {
    class_<TwoStepNPTMTKGPU, boost::shared_ptr<TwoStepNPTMTKGPU>, bases<TwoStepNPTMTK>, boost::noncopyable>
        ("TwoStepNPTMTKGPU", init< boost::shared_ptr<SystemDefinition>,
                       boost::shared_ptr<ParticleGroup>,
                       boost::shared_ptr<ComputeThermo>,
                       boost::shared_ptr<ComputeThermo>,
                       Scalar,
                       Scalar,
                       boost::shared_ptr<Variant>,
                       boost::shared_ptr<Variant>,
                       TwoStepNPTMTKGPU::couplingMode,
                       unsigned int,
                       const bool>())
        ;

    }