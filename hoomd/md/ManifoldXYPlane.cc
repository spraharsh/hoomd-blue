// Copyright (c) 2009-2021 The Regents of the University of Michigan
// This file is part of the HOOMD-blue project, released under the BSD 3-Clause License.


// Maintainer: pschoenhoefer

#include "ManifoldXYPlane.h"
#include <pybind11/pybind11.h>



//! Exports the XYPlane manifold class to python
void export_ManifoldXYPlane(pybind11::module& m)
    {
    pybind11::class_< ManifoldXYPlane, std::shared_ptr<ManifoldXYPlane> >(m, "ManifoldXYPlane")
    .def(pybind11::init<Scalar >())
    .def_property_readonly("shift", &ManifoldXYPlane::getShift)
    ;
    }
