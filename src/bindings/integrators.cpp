#include <core/integrators/factory.h>
#include <core/interactions/interface.h>

#include "bindings.h"
#include "class_wrapper.h"

using namespace pybind11::literals;

void exportIntegrators(py::module& m)
{
    py::handlers_class<Integrator> pyint(m, "Integrator", R"(
        Base integration class
    )");

    py::handlers_class<IntegratorConstOmega>(m, "Rotate", pyint, R"(
        Rotate particles around the specified point in space with a constant angular velocity :math:`\mathbf{\Omega}`
    )")
        .def(py::init(&IntegratorFactory::createConstOmega),
             "state"_a, "name"_a, "center"_a, "omega"_a, R"(
                Args:
                    name: name of the integrator
                    center: point around which to rotate
                    omega: angular velocity :math:`\mathbf{\Omega}`
            )");
        
    py::handlers_class<IntegratorOscillate>(m, "Oscillate", pyint, R"(
        Move particles with the periodically changing velocity
        :math:`\mathbf{u}(t) = \cos(2 \pi \, t / T) \mathbf{u}_0`
    )")
        .def(py::init(&IntegratorFactory::createOscillating),
             "state"_a, "name"_a, "velocity"_a, "period"_a, R"(
                Args:
                    name: name of the integrator
                    velocity: :math:`\mathbf{u}_0`
                    period: oscillation period :math:`T`
            )");
        
    py::handlers_class<IntegratorVVRigid>(m, "RigidVelocityVerlet", pyint, R"(
        Integrate the position and rotation (in terms of quaternions) of the rigid bodies as per Velocity-Verlet scheme.
        Can only applied to :any:`RigidObjectVector` or :any:`RigidEllipsoidVector`.
    )")
        .def(py::init(&IntegratorFactory::createRigidVV),
             "state"_a, "name"_a, R"(
                Args:
                    name: name of the integrator
            )");
        
    py::handlers_class<IntegratorTranslate>(m, "Translate", pyint, R"(
        Translate particles with a constant velocity :math:`\mathbf{u}` regardless forces acting on them.
    )")
        .def(py::init(&IntegratorFactory::createTranslate),
             "state"_a, "name"_a, "velocity"_a, R"(
                Args:
                    name: name of the integrator
                    velocity: translational velocity :math:`\mathbf{\Omega}`
            )");
        
    py::handlers_class<IntegratorVV<Forcing_None>>
        (m, "VelocityVerlet", pyint, R"(
            Classical Velocity-Verlet integrator with fused steps for coordinates and velocities.
            The velocities are shifted with respect to the coordinates by one half of the time-step
            
            .. math::

                \mathbf{a}^{n} &= \frac{1}{m} \mathbf{F}(\mathbf{x}^{n}, \mathbf{v}^{n-1/2}) \\
                \mathbf{v}^{n+1/2} &= \mathbf{v}^{n-1/2} + \mathbf{a}^n \Delta t \\
                \mathbf{x}^{n+1} &= \mathbf{x}^{n} + \mathbf{v}^{n+1/2} \Delta t 

            where bold symbol means a vector, :math:`m` is a particle mass, and superscripts denote the time: :math:`\mathbf{x}^{k} = \mathbf{x}(k \, \Delta t)`
        )")
        .def(py::init(&IntegratorFactory::createVV),
             "state"_a, "name"_a, R"(
                Args:
                    name: name of the integrator
            )");
        
    py::handlers_class<IntegratorVV<Forcing_ConstDP>>
        (m, "VelocityVerlet_withConstForce", pyint, R"(
            Same as regular :any:`VelocityVerlet`, but the forces on all the particles are modified with the constant pressure term:
   
            .. math::

                \mathbf{a}^{n} &= \frac{1}{m} \left( \mathbf{F}(\mathbf{x}^{n}, \mathbf{v}^{n-1/2}) + \mathbf{F}_{extra} \right) \\
        )")
        .def(py::init(&IntegratorFactory::createVV_constDP),
             "state"_a, "name"_a, "force"_a, R"(

                Args:
                    name: name of the integrator
                    force: :math:`\mathbf{F}_{extra}`
            )");
        
    py::handlers_class<IntegratorVV<Forcing_PeriodicPoiseuille>>
        (m, "VelocityVerlet_withPeriodicForce", pyint, R"(
            Same as regular Velocity-Verlet, but the forces on all the particles are modified with periodic Poiseuille term.
            This means that all the particles in half domain along certain axis (Ox, Oy or Oz) are pushed with force
            :math:`F_{Poiseuille}` parallel to Oy, Oz or Ox correspondingly, and the particles in another half of the domain are pushed in the same direction
            with force :math:`-F_{Poiseuille}`    
        )")
        .def(py::init(&IntegratorFactory::createVV_PeriodicPoiseuille),
             "state"_a, "name"_a, "force"_a, "direction"_a, R"(                
                Args:
                    name: name of the integrator
                    force: force magnitude, :math:`F_{Poiseuille}`
                    direction: Valid values: \"x\", \"y\", \"z\". Defines the direction of the pushing force
                               if direction is \"x\", the sign changes along \"y\".
                               if direction is \"y\", the sign changes along \"z\".
                               if direction is \"z\", the sign changes along \"x\".
            )");

    py::handlers_class<IntegratorSubStepMembrane>
        (m, "SubStepMembrane", pyint, R"(
            Takes advantage of separation of time scales between membrane forces (fast forces) and other forces acting on the membrane (slow forces).
            This integrator advances the membrane with constant slow forces for 'substeps' sub time steps.
            The fast forces are updated after each sub step.
            Positions and velocity are updated using an internal velocity verlet integrator.
        )")
        .def(py::init(&IntegratorFactory::createSubStepMembrane),
             "state"_a, "name"_a, "substeps"_a, "fastForces"_a, R"(
                Args:
                    name: name of the integrator
                    substeps: number of sub steps
                    fastForces: the fast interaction module. Only accepts :any:`MembraneForces`
                
                .. warning::
                    The fastForces :any:`MembraneForces` does not need to be set for the :any:`MembraneVector` explicitely.
                    The interaction will be set when setting this integrator to the :any:`MembraneVector`.
                
            )");
}

