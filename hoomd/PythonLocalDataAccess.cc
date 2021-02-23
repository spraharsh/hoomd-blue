#include "PythonLocalDataAccess.h"


void export_GhostDataFlag(pybind11::module &m)
    {
    pybind11::enum_<GhostDataFlag>(m, "GhostDataFlag")
        .value("standard", GhostDataFlag::standard)
        .value("ghost", GhostDataFlag::ghost)
        .value("both", GhostDataFlag::both)
        ;
    }


void export_HOOMDHostBuffer(pybind11::module &m)
    {
    pybind11::class_<HOOMDHostBuffer>(
        m, "HOOMDHostBuffer", pybind11::buffer_protocol())
        .def_buffer([](HOOMDHostBuffer &b) -> pybind11::buffer_info 
                {
                return b.new_buffer();
                })
        .def_property_readonly("read_only", &HOOMDHostBuffer::getReadOnly);
        ;
    }


#if ENABLE_HIP
void export_HOOMDDeviceBuffer(pybind11::module &m)
    {
    pybind11::class_<HOOMDDeviceBuffer>(m, "HOOMDDeviceBuffer")
        .def_property_readonly("__cuda_array_interface__",
                               &HOOMDDeviceBuffer::getCudaArrayInterface)
        .def_property_readonly("read_only", &HOOMDDeviceBuffer::getReadOnly);
        ;
    }
#endif
