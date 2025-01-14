.. Copyright (c) 2009-2022 The Regents of the University of Michigan.
.. Part of HOOMD-blue, released under the BSD 3-Clause License.

md.force
--------------

.. rubric:: Overview

.. py:currentmodule:: hoomd.md.force

.. autosummary::
    :nosignatures:

    Force
    Custom
    Active
    ActiveOnManifold

.. rubric:: Details

.. automodule:: hoomd.md.force
    :synopsis: Apply forces to particles.

    .. autoclass:: Force
        :members:

    .. autoclass:: Custom
        :members:

    .. autoclass:: Active
        :show-inheritance:
        :no-inherited-members:
        :members: create_diffusion_updater

    .. autoclass:: ActiveOnManifold
        :show-inheritance:
        :members: create_diffusion_updater
