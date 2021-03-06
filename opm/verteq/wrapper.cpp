#include <opm/verteq/wrapper.hpp>
#include <string>
#include <opm/verteq/verteq.hpp>
#include <opm/verteq/state.hpp>
#include <opm/verteq/utility/exc.hpp>
#include <opm/core/simulator/SimulatorIncompTwophase.hpp>
#include <opm/core/simulator/SimulatorReport.hpp>
#include <opm/core/simulator/TwophaseState.hpp>
#include <opm/core/utility/Event.hpp>
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif /* __clang__ */
#include <opm/core/wells/WellsManager.hpp>
#ifdef __clang__
#pragma clang diagnostic pop
#endif /* __clang__ */
using namespace std;

namespace Opm {

VertEqWrapperBase::VertEqWrapperBase (
		unique_ptr <Simulator> underlaying,
		const parameter::ParameterGroup& param,
		const UnstructuredGrid& grid,
		const IncompPropertiesInterface& props,
		const RockCompressibility* rock_comp_props,
		WellsManager& wells_manager,
		const std::vector<double>& src,
		const FlowBoundaryConditions* bcs,
		LinearSolverInterface& linsolver,
		const double* gravity)
	// use this simulator
	: sim (std::move (underlaying))

	// initialize pointer to null, so it is deleted properly
	// if the sim. ctor throws
	, ve (0)
	, wells_mgr (0)
	, timestep_callbacks (new EventSource ())
	, fineState (0)
	, coarseState (0)
	, syncDone (false) {

	// VE model that is injected in between the fine-scale
	// model that is sent to us, and the simulator
	ve = VertEq::create ("",
	                     param,
	                     grid,
	                     props,
	                     wells_manager.c_wells (),
	                     src,
	                     bcs,
	                     gravity);

	// copying the well manager is explicitly forbidden (!)
	// so we loose the hierarchy of wells, but that shouldn't
	// matter; what counts is that there is an equal number of
	// wells that are put in the equations. unfortunately, this
	// scheme will make yet another copy of the well list.
	// the well manager must be a member of the wrapper, since
	// it needs to be live when we call the run() method of the
	// simulator (it cannot be a local here)
	wells_mgr = new WellsManager (const_cast <Wells*> (ve->wells ()));

    // pass arguments to the underlaying simulator; this is where
    // the smart pointer wrapper will actually create the underlaying
    // simulator instance
    sim->init (param,
               ve->grid (),
               ve->props (),
               rock_comp_props,
               *wells_mgr,
               ve->src (),
               ve->bcs (),
               linsolver,
               ve->gravity ());
}

Event&
VertEqWrapperBase::timestep_completed () {
	return *timestep_callbacks;
}

void
VertEqWrapperBase::resetSyncFlag () {
	this->syncDone = false;
}

VertEqWrapperBase::~VertEqWrapperBase () {
	delete wells_mgr;
	delete ve;
}

SimulatorReport
VertEqWrapperBase::run(
		SimulatorTimer& timer,
		TwophaseState& state,
		WellState& well_state) {

	// the state that is passed is for fine-scale; we need to create
	// a coarse state that matches the grid, for the simulator
	VertEqState upscaled_state (*ve, state);

	// setup the "current" state so that we can downscale this on
	// demand for any callbacks in the sync() method
	this->fineState = &state;
	this->coarseState = &upscaled_state;

	// make the state "active", so that it push its changes to the
	// ve model whenever an update is completed and its state is stable
	sim->timestep_completed ()
	    .add <VertEqState, &VertEqState::notify> (upscaled_state);

	// add everyone that has registered at us to be notified by the
	// inner simulator as well (on our behalf); this daisy chains the
	// list of callbacks
	sim->timestep_completed ()
	    .add <EventSource, &EventSource::signal> (*timestep_callbacks);

	// after the other callbacks have been signaled, then reset
	// the sync flag so that the next timestep it will have to be
	// done again (this depends on the ordering of the callbacks)
	sim->timestep_completed ()
	    .add <VertEqWrapperBase, &VertEqWrapperBase::resetSyncFlag> (*this);

	// we "reuse" the well state for the three-dimensional grid;
	// it is a value object which is created once based on the
	// list of wells. the internal well manager used here contains
	// the same wells at the same indices as the original, so the
	// state should work with the clone, too.

	// forward the call to the underlaying simulator
	SimulatorReport report = sim->run(timer, upscaled_state, well_state);

	// clear the state pointers after the simulation has ended; then
	// there is no "current" state anymore (but the fine state object
	// that were pointed to, which we were passed as an argument, is
	// of course still valid
	this->fineState = 0;
	this->coarseState = 0;

	return report;
}

void
VertEqWrapperBase::sync () {
	// if there is no "current" state, then we have been called from
	// outside of the callback
	if (!fineState) {
		throw OPM_EXC ("sync() called from outside callback!");
	}

	// downscale from the local variable in run, to the argument passed
	// to it, based on these two pointers to them, once, and then mark
	// that we don't need to do that for the rest of this timestep
	if (!syncDone) {
		ve->downscale (*coarseState, *fineState);
		syncDone = true;
	}
}

} /* namespace Opm */
