// Copyright (c) 2017-2019 Michael C. Heiber
// This source file is part of the Excimontec project, which is subject to the MIT License.
// For more information, see the LICENSE file that accompanies this software.
// The Excimontec project can be found on Github at https://github.com/MikeHeiber/Excimontec

#include "OSC_Sim.h"

using namespace std;
using namespace KMC_Lattice;

namespace Excimontec {

	OSC_Sim::OSC_Sim() {}

	OSC_Sim::~OSC_Sim() {}

	bool OSC_Sim::init(const Parameters& params_in, const int id) {
		// Reset error status
		Error_found = false;
		// Check parameters for errors
		if (!params_in.checkParameters()) {
			Error_found = true;
			cout << id << ": Error with input parameters." << endl;
			setErrorMessage("Error with the input parameters.");
			return false;
		}
		bool success;
		// Set parameters of Simulation base class
		Simulation::init(params_in, id);
		// Initialize parameters object
		params = params_in;
		// Initialize derived parameters
		if (params.Enable_ToF_test) {
			Transient_start = params.ToF_transient_start;
			Transient_end = params.ToF_transient_end;
			Transient_pnts_per_decade = params.ToF_pnts_per_decade;
		}
		if (params.Enable_dynamics_test) {
			Transient_start = params.Dynamics_transient_start;
			Transient_end = params.Dynamics_transient_end;
			Transient_pnts_per_decade = params.Dynamics_pnts_per_decade;
		}
		// Initialize Sites
		Site_OSC site;
		sites.assign(lattice.getNumSites(), site);
		// Initialize Film Architecture
		success = initializeArchitecture();
		if (!success) {
			Error_found = true;
			cout << id << ": Error initializing the film architecture." << endl;
			setErrorMessage("Error initializing the film architecture.");
			return false;
		}
		// Assign energies to each site in the sites vector
		reassignSiteEnergies();
		// Initialize Coulomb interactions lookup table
		AvgDielectric = (params.Dielectric_donor + params.Dielectric_acceptor) / 2;
		Image_interaction_prefactor = (Elementary_charge / (16 * Pi*AvgDielectric*Vacuum_permittivity))*1e9;
		double Unit_size = lattice.getUnitSize();
		int range = (int)ceil(intpow(params.Coulomb_cutoff / lattice.getUnitSize(), 2));
		Coulomb_table.assign(range + 1, 0);
		for (int i = 1, imax = (int)Coulomb_table.size(); i < imax; i++) {
			Coulomb_table[i] = ((Coulomb_constant*Elementary_charge) / AvgDielectric) / (1e-9*Unit_size*sqrt((double)i));
			if (params.Enable_gaussian_polaron_delocalization) {
				Coulomb_table[i] *= erf((Unit_size*sqrt((double)i)) / (params.Polaron_delocalization_length*sqrt(2)));
			}
		}
		Coulomb_range = (int)ceil((params.Coulomb_cutoff / lattice.getUnitSize())*(params.Coulomb_cutoff / lattice.getUnitSize()));
		// Initialize electrical potential vector
		E_potential.assign(lattice.getHeight(), 0);
		for (int i = 0; i < lattice.getHeight(); i++) {
			E_potential[i] = (params.Internal_potential*lattice.getHeight() / (lattice.getHeight() + 1)) - (params.Internal_potential / (lattice.getHeight() + 1))*i;
		}
		// Initialize event calculation data
		exciton_event_calc_vars = ExcitonEventCalcVars(this);
		polaron_event_calc_vars = PolaronEventCalcVars(this);
		// Initialize exciton creation event
		R_exciton_generation_donor = ((params.Exciton_generation_rate_donor*N_donor_sites*1e-7*lattice.getUnitSize())*1e-7*lattice.getUnitSize())*1e-7*lattice.getUnitSize();
		R_exciton_generation_acceptor = ((params.Exciton_generation_rate_acceptor*N_acceptor_sites*1e-7*lattice.getUnitSize())*1e-7*lattice.getUnitSize())*1e-7*lattice.getUnitSize();
		if (params.Enable_exciton_diffusion_test || params.Enable_IQE_test) {
			isLightOn = true;
			Exciton::Creation exciton_creation_event(this);
			exciton_creation_event.calculateRateConstant(R_exciton_generation_donor + R_exciton_generation_acceptor);
			exciton_creation_event.calculateExecutionTime(R_exciton_generation_donor + R_exciton_generation_acceptor);
			exciton_creation_events.assign(1, exciton_creation_event);
			exciton_creation_it = addEvent(&exciton_creation_events.front());
		}
		else if (params.Enable_dynamics_test) {
			isLightOn = false;
			// Initialize parameters
			N_initial_excitons = (int)ceil(params.Dynamics_initial_exciton_conc*lattice.getVolume());
			// Initialize data structures
			Transient_step_size = 1.0 / (double)Transient_pnts_per_decade;
			int num_steps = (int)floor((log10(Transient_end) - log10(Transient_start)) / Transient_step_size) + 1;
			transient_times.assign(num_steps, 0);
			for (int i = 0; i < (int)transient_times.size(); i++) {
				transient_times[i] = pow(10, log10(Transient_start) + i * Transient_step_size);
			}
			transient_singlet_counts.assign(num_steps, 0);
			transient_triplet_counts.assign(num_steps, 0);
			transient_electron_counts.assign(num_steps, 0);
			transient_hole_counts.assign(num_steps, 0);
			transient_exciton_msdv.assign(num_steps, 0);
			transient_electron_msdv.assign(num_steps, 0);
			transient_hole_msdv.assign(num_steps, 0);
			transient_exciton_energies.assign(num_steps, 0);
			transient_electron_energies.assign(num_steps, 0);
			transient_hole_energies.assign(num_steps, 0);
			// Create initial test excitons
			generateDynamicsExcitons();
		}
		else if (params.Enable_ToF_test) {
			isLightOn = false;
			// Initialize data structures
			Transient_step_size = 1.0 / (double)Transient_pnts_per_decade;
			int num_steps = (int)floor((log10(Transient_end) - log10(Transient_start)) / Transient_step_size) + 1;
			transient_times.assign(num_steps, 0);
			for (int i = 0; i < (int)transient_times.size(); i++) {
				transient_times[i] = pow(10, log10(Transient_start) + i * Transient_step_size);
			}
			transient_velocities.assign(num_steps, 0);
			if (!params.ToF_polaron_type) {
				transient_electron_energies.assign(num_steps, 0);
				transient_electron_counts.assign(num_steps, 0);
			}
			else {
				transient_hole_energies.assign(num_steps, 0);
				transient_hole_counts.assign(num_steps, 0);
			}
			electron_extraction_data.assign(lattice.getLength()*lattice.getWidth(), 0);
			hole_extraction_data.assign(lattice.getLength()*lattice.getWidth(), 0);
			// Create initial test polarons
			generateToFPolarons();
		}
		else if (params.Enable_steady_transport_test) {
			isLightOn = false;
			// Create test polarons
			generateSteadyPolarons();
		}
		if (params.Enable_IQE_test) {
			electron_extraction_data.assign(lattice.getLength()*lattice.getWidth(), 0);
			hole_extraction_data.assign(lattice.getLength()*lattice.getWidth(), 0);
		}
		if (Error_found) {
			return false;
		}
		else {
			return true;
		}
	}

	void OSC_Sim::calculateAllEvents() {
		auto object_its = getAllObjectPtrs();
		calculateObjectListEvents(object_its);
	}

	double OSC_Sim::calculateCoulomb(const std::list<Polaron>::const_iterator polaron_it, const KMC_Lattice::Coords& coords) const {
		double Energy = 0;
		double distance;
		int distance_sq_lat;
		bool charge = polaron_it->getCharge();
		// Loop through electrons
		for (auto const &item : electrons) {
			if (!charge && item.getTag() == polaron_it->getTag()) {
				continue;
			}
			distance_sq_lat = lattice.calculateLatticeDistanceSquared(coords, item.getCoords());
			if (!(distance_sq_lat > Coulomb_range)) {
				if (!charge) {
					Energy += Coulomb_table[distance_sq_lat];
				}
				else {
					Energy -= Coulomb_table[distance_sq_lat];
				}
			}
		}
		// Loop through holes
		for (auto const &item : holes) {
			if (charge && item.getTag() == polaron_it->getTag()) {
				continue;
			}
			distance_sq_lat = lattice.calculateLatticeDistanceSquared(coords, item.getCoords());
			if (!(distance_sq_lat > Coulomb_range)) {
				if (charge) {
					Energy += Coulomb_table[distance_sq_lat];
				}
				else {
					Energy -= Coulomb_table[distance_sq_lat];
				}
			}
		}
		// Add electrode image charge interactions
		if (!lattice.isZPeriodic() && !params.Enable_ToF_test) {
			distance = lattice.getUnitSize()*((double)(lattice.getHeight() - coords.z) - 0.5);
			if (!((distance - 0.0001) > params.Coulomb_cutoff)) {
				Energy -= Image_interaction_prefactor / distance;
			}
			distance = lattice.getUnitSize()*((double)(coords.z + 1) - 0.5);
			if (!((distance - 0.0001) > params.Coulomb_cutoff)) {
				Energy -= Image_interaction_prefactor / distance;
			}
		}
		return Energy;
	}

	double OSC_Sim::calculateCoulomb(const bool charge, const KMC_Lattice::Coords& coords) const {
		double Energy = 0;
		double distance;
		int distance_sq_lat;
		// Loop through electrons
		for (auto const &item : electrons) {
			distance_sq_lat = lattice.calculateLatticeDistanceSquared(coords, item.getCoords());
			if (!(distance_sq_lat > Coulomb_range)) {
				if (!charge) {
					Energy += Coulomb_table[distance_sq_lat];
				}
				else {
					Energy -= Coulomb_table[distance_sq_lat];
				}
			}
		}
		// Loop through holes
		for (auto const &item : holes) {
			distance_sq_lat = lattice.calculateLatticeDistanceSquared(coords, item.getCoords());
			if (!(distance_sq_lat > Coulomb_range)) {
				if (charge) {
					Energy += Coulomb_table[distance_sq_lat];
				}
				else {
					Energy -= Coulomb_table[distance_sq_lat];
				}
			}
		}
		// Add electrode image charge interactions
		if (!lattice.isZPeriodic()) {
			distance = lattice.getUnitSize()*((double)(lattice.getHeight() - coords.z) - 0.5);
			if (!((distance - 0.0001) > params.Coulomb_cutoff)) {
				Energy -= Image_interaction_prefactor / distance;
			}
			distance = lattice.getUnitSize()*((double)(coords.z + 1) - 0.5);
			if (!((distance - 0.0001) > params.Coulomb_cutoff)) {
				Energy -= Image_interaction_prefactor / distance;
			}
		}
		return Energy;
	}

	void OSC_Sim::calculateDOSCorrelation() {
		DOS_correlation_data.clear();
		double cutoff_radius = 1.0;
		calculateDOSCorrelation(cutoff_radius);
		while (DOS_correlation_data.back().second > 0.01) {
			cutoff_radius += 1.0;
			calculateDOSCorrelation(cutoff_radius);
		}
	}

	void OSC_Sim::calculateDOSCorrelation(const double cutoff_radius) {
		int size_old = (int)DOS_correlation_data.size();
		int range = (int)ceil(cutoff_radius / lattice.getUnitSize());
		int size_new = (int)ceil(2 * cutoff_radius / lattice.getUnitSize()) + 1;
		vector<double> sum_total(size_new, 0.0);
		vector<int> count_total(size_new, 0);
		vector<double> energies(sites.size());
		for (int n = 0, nmax = (int)sites.size(); n < nmax; n++) {
			Coords coords = lattice.getSiteCoords(n);
			energies[n] = getSiteEnergy(coords);
			for (int i = -range; i <= range; i++) {
				for (int j = -range; j <= range; j++) {
					for (int k = -range; k <= range; k++) {
						if (!lattice.checkMoveValidity(coords, i, j, k)) {
							continue;
						}
						int bin = (int)round(2.0 * sqrt(i * i + j * j + k * k));
						// Calculation is skipped for bin values that have already been calculated during previous calls
						if (bin < (size_old - 1)) {
							continue;
						}
						Coords dest_coords;
						lattice.calculateDestinationCoords(coords, i, j, k, dest_coords);
						if (bin < size_new) {
							sum_total[bin] += getSiteEnergy(coords)*getSiteEnergy(dest_coords);
							count_total[bin]++;
						}
					}
				}
			}
		}
		double stdev = vector_stdev(energies);
		DOS_correlation_data.resize(size_new);
		DOS_correlation_data[0].first = 0.0;
		DOS_correlation_data[0].second = 1.0;
		DOS_correlation_data[1].first = lattice.getUnitSize()*0.5;
		DOS_correlation_data[1].second = 1.0;
		for (int m = 2; m < size_new; m++) {
			if (m < size_old) {
				continue;
			}
			if (count_total[m] > 0) {
				DOS_correlation_data[m].first = lattice.getUnitSize()*m / 2.0;
				DOS_correlation_data[m].second = sum_total[m] / ((count_total[m] - 1)*stdev*stdev);
			}
		}
	}

	vector<double> OSC_Sim::calculateMobilityData(const vector<double>& transit_times) const {
		vector<double> mobilities = transit_times;
		for (int i = 0; i < (int)mobilities.size(); i++) {
			mobilities[i] = (1e-7*lattice.getUnitSize()*lattice.getHeight()) / (fabs(params.Internal_potential)*transit_times[i]);
			mobilities[i] *= 1e-7*lattice.getUnitSize()*lattice.getHeight();
		}
		return mobilities;
	}

	vector<pair<double, double>> OSC_Sim::calculateTransitTimeHist(const vector<double>& data, const int counts) const {
		double step_size = 1.0 / (double)Transient_pnts_per_decade;
		vector<pair<double, double>> dist(transient_times.size(), { 0.0,0.0 });
		for (auto const &item : data) {
			for (int j = 0; j < (int)transient_times.size(); j++) {
				if (item > pow(10, log10(transient_times[j]) - 0.5*step_size) && item < pow(10, log10(transient_times[j]) + 0.5*step_size)) {
					dist[j].second += 1.0;
				}
			}
		}
		for (int j = 0; j < (int)dist.size(); j++) {
			dist[j].first = transient_times[j];
			dist[j].second /= (double)counts;
		}
		return dist;
	}

	Coords OSC_Sim::calculateRandomExcitonCreationCoords() {
		uniform_real_distribution<double> dist(0.0, R_exciton_generation_donor + R_exciton_generation_acceptor);
		double num = dist(generator);
		short type_target;
		if (num < R_exciton_generation_donor) {
			type_target = 1;
		}
		else {
			type_target = 2;
		}
		// Faster method of choosing a random site.  This becomes slow when the lattice has high occupancy.
		// Use faster method when lattice is less than 50% occupied.
		int N_tries = 0;
		while ((N_excitons + N_electrons + N_holes) < (0.5*lattice.getLength()*lattice.getWidth()*lattice.getHeight()) && N_tries < 10) {
			auto dest_coords = lattice.generateRandomCoords();
			if (!lattice.isOccupied(dest_coords) && getSiteType(dest_coords) == type_target) {
				return dest_coords;
			}
			N_tries++;
		}
		// Method of choosing one of the empty sites.  This is slowish because it must loop through all sites first
		// Get vector of possible creation sites
		vector<long int> indices;
		indices.reserve(lattice.getLength()*lattice.getWidth()*lattice.getHeight());
		for (int x = 0; x < lattice.getLength(); x++) {
			for (int y = 0; y < lattice.getWidth(); y++) {
				for (int z = 0; z < lattice.getHeight(); z++) {
					Coords dest_coords(x, y, z);
					if (!lattice.isOccupied(dest_coords) && getSiteType(dest_coords) == type_target) {
						indices.push_back(lattice.getSiteIndex(dest_coords));
					}
				}
			}
		}
		if ((int)indices.size() == 0) {
			cout << getId() << ": Error! An empty site for exciton creation could not be found." << endl;
			setErrorMessage("An empty site for exciton creation could not be found.");
			Error_found = true;
			return Coords(-1, -1, -1);
		}
		uniform_int_distribution<long int> distn(0, (long int)indices.size());
		return lattice.getSiteCoords(indices[distn(generator)]);
	}

	void OSC_Sim::calculateExcitonEvents(Exciton* exciton_ptr) {
		const auto exciton_it = getExcitonIt(exciton_ptr);
		const Coords object_coords = exciton_it->getCoords();
		if (isLoggingEnabled()) {
			*Logfile << "Calculating events for exciton " << exciton_it->getTag() << " at site " << object_coords.x << "," << object_coords.y << "," << object_coords.z << "." << endl;
		}
		Coords dest_coords;
		double E_delta, Coulomb_final;
		double rate = 0;
		vector<Event*> possible_events;
		// Exciton hopping, dissociation, and annihilation events
		for (int i = -exciton_event_calc_vars.range, imax = exciton_event_calc_vars.range; i <= imax; i++) {
			for (int j = -exciton_event_calc_vars.range, jmax = exciton_event_calc_vars.range; j <= jmax; j++) {
				for (int k = -exciton_event_calc_vars.range, kmax = exciton_event_calc_vars.range; k <= kmax; k++) {
					int index = (i + exciton_event_calc_vars.range)*exciton_event_calc_vars.dim*exciton_event_calc_vars.dim + (j + exciton_event_calc_vars.range)*exciton_event_calc_vars.dim + (k + exciton_event_calc_vars.range);
					if (!exciton_event_calc_vars.isInDissRange[index] && !exciton_event_calc_vars.isInFRETRange[index]) {
						continue;
					}
					if (!lattice.checkMoveValidity(object_coords, i, j, k)) {
						continue;
					}
					lattice.calculateDestinationCoords(object_coords, i, j, k, dest_coords);
					// Annihilation events
					if (lattice.isOccupied(dest_coords)) {
						if (exciton_event_calc_vars.isInFRETRange[index]) {
							auto object_target_ptr = sites[lattice.getSiteIndex(dest_coords)].getObjectPtr();
							// Exciton-Exciton annihilation
							if (object_target_ptr->getObjectType().compare(Exciton::object_type) == 0) {
								// Skip disallowed triplet-singlet annihilation
								if (!exciton_it->getSpin() && getExcitonIt(object_target_ptr)->getSpin()) {
									continue;
								}
								exciton_event_calc_vars.ee_annihilations_temp[index].setObjectPtr(exciton_ptr);
								exciton_event_calc_vars.ee_annihilations_temp[index].setDestCoords(dest_coords);
								exciton_event_calc_vars.ee_annihilations_temp[index].setObjectTargetPtr(object_target_ptr);
								// Exciton is starting from a donor site
								if (getSiteType(object_coords) == (short)1) {
									// Triplet Dexter mechanism
									if (!exciton_it->getSpin() && !params.Enable_FRET_triplet_annihilation) {
										exciton_event_calc_vars.ee_annihilations_temp[index].calculateRateConstant(params.R_exciton_exciton_annihilation_donor, params.Triplet_localization_donor, exciton_event_calc_vars.distances[index]);
									}
									// FRET mechanism
									else {
										exciton_event_calc_vars.ee_annihilations_temp[index].calculateRateConstant(params.R_exciton_exciton_annihilation_donor, exciton_event_calc_vars.distances[index]);
									}
								}
								// Exciton is starting from an acceptor site
								else {
									// Triplet Dexter mechanism
									if (!exciton_it->getSpin() && !params.Enable_FRET_triplet_annihilation) {
										exciton_event_calc_vars.ee_annihilations_temp[index].calculateRateConstant(params.R_exciton_exciton_annihilation_acceptor, params.Triplet_localization_acceptor, exciton_event_calc_vars.distances[index]);
									}
									// FRET mechanism
									else {
										exciton_event_calc_vars.ee_annihilations_temp[index].calculateRateConstant(params.R_exciton_exciton_annihilation_acceptor, exciton_event_calc_vars.distances[index]);
									}
								}
								// Save the calculated exciton-exciton annihilation event as a possible event
								possible_events.push_back(&exciton_event_calc_vars.ee_annihilations_temp[index]);
							}
							// Exciton-Polaron annihilation
							else if (sites[lattice.getSiteIndex(dest_coords)].getObjectPtr()->getObjectType().compare(Polaron::object_type) == 0) {
								exciton_event_calc_vars.ep_annihilations_temp[index].setObjectPtr(exciton_ptr);
								exciton_event_calc_vars.ep_annihilations_temp[index].setDestCoords(dest_coords);
								exciton_event_calc_vars.ep_annihilations_temp[index].setObjectTargetPtr(object_target_ptr);
								// Exciton is starting from a donor site
								if (getSiteType(object_coords) == (short)1) {
									// Triplet Dexter mechanism
									if (!exciton_it->getSpin() && !params.Enable_FRET_triplet_annihilation) {
										exciton_event_calc_vars.ep_annihilations_temp[index].calculateRateConstant(params.R_exciton_polaron_annihilation_donor, params.Triplet_localization_donor, exciton_event_calc_vars.distances[index]);
									}
									// FRET mechanism
									else {
										exciton_event_calc_vars.ep_annihilations_temp[index].calculateRateConstant(params.R_exciton_polaron_annihilation_donor, exciton_event_calc_vars.distances[index]);
									}
								}
								// Exciton is starting from an acceptor site
								else {
									// Triplet Dexter mechanism
									if (!exciton_it->getSpin() && !params.Enable_FRET_triplet_annihilation) {
										exciton_event_calc_vars.ep_annihilations_temp[index].calculateRateConstant(params.R_exciton_polaron_annihilation_acceptor, params.Triplet_localization_acceptor, exciton_event_calc_vars.distances[index]);
									}
									// FRET mechanism
									else {
										exciton_event_calc_vars.ep_annihilations_temp[index].calculateRateConstant(params.R_exciton_polaron_annihilation_acceptor, exciton_event_calc_vars.distances[index]);
									}
								}
								// Save the calculated exciton-polaron annihilation event as a possible event
								possible_events.push_back(&exciton_event_calc_vars.ep_annihilations_temp[index]);
							}
						}
					}
					// Dissociation and Hop events
					else {
						// Dissociation event
						if (getSiteType(object_coords) != getSiteType(dest_coords) && exciton_event_calc_vars.isInDissRange[index]) {
							exciton_event_calc_vars.dissociations_temp[index].setObjectPtr(exciton_ptr);
							exciton_event_calc_vars.dissociations_temp[index].setDestCoords(dest_coords);
							// Exciton is starting from a donor site
							if (getSiteType(object_coords) == (short)1) {
								Coulomb_final = calculateCoulomb(true, object_coords) + calculateCoulomb(false, dest_coords) - Coulomb_table[i*i + j * j + k * k];
								E_delta = (getSiteEnergy(dest_coords) - getSiteEnergy(object_coords)) - (params.Lumo_acceptor - params.Lumo_donor) + (Coulomb_final + params.E_exciton_binding_donor) + (E_potential[dest_coords.z] - E_potential[object_coords.z]);
								// Singlet
								if (exciton_ptr->getSpin()) {
									if (params.Enable_miller_abrahams) {
										exciton_event_calc_vars.dissociations_temp[index].calculateRateConstant(params.R_exciton_dissociation_donor, params.Singlet_localization_donor, exciton_event_calc_vars.distances[index], E_delta);
									}
									else {
										exciton_event_calc_vars.dissociations_temp[index].calculateRateConstant(params.R_exciton_dissociation_donor, params.Singlet_localization_donor, exciton_event_calc_vars.distances[index], E_delta, params.Reorganization_donor);
									}
								}
								// Triplet
								else {
									// Increase E_delta by the singlet-triplet energy splitting if the exciton is a triplet
									E_delta += params.E_exciton_ST_donor;
									if (params.Enable_miller_abrahams) {
										exciton_event_calc_vars.dissociations_temp[index].calculateRateConstant(params.R_exciton_dissociation_donor, params.Triplet_localization_donor, exciton_event_calc_vars.distances[index], E_delta);
									}
									else {
										exciton_event_calc_vars.dissociations_temp[index].calculateRateConstant(params.R_exciton_dissociation_donor, params.Triplet_localization_donor, exciton_event_calc_vars.distances[index], E_delta, params.Reorganization_donor);
									}
								}
							}
							// Exciton is starting from an acceptor site
							else {
								Coulomb_final = calculateCoulomb(false, object_coords) + calculateCoulomb(true, dest_coords) - Coulomb_table[i*i + j * j + k * k];
								E_delta = (getSiteEnergy(dest_coords) - getSiteEnergy(object_coords)) + (params.Homo_donor - params.Homo_acceptor) + (Coulomb_final + params.E_exciton_binding_donor) - (E_potential[dest_coords.z] - E_potential[object_coords.z]);
								// Singlet
								if (exciton_ptr->getSpin()) {
									if (params.Enable_miller_abrahams) {
										exciton_event_calc_vars.dissociations_temp[index].calculateRateConstant(params.R_exciton_dissociation_acceptor, params.Singlet_localization_acceptor, exciton_event_calc_vars.distances[index], E_delta);
									}
									else {
										exciton_event_calc_vars.dissociations_temp[index].calculateRateConstant(params.R_exciton_dissociation_acceptor, params.Singlet_localization_acceptor, exciton_event_calc_vars.distances[index], E_delta, params.Reorganization_acceptor);
									}
								}
								// Triplet
								else {
									// Increase E_delta by the singlet-triplet energy splitting if the exciton is a triplet
									E_delta += params.E_exciton_ST_acceptor;
									if (params.Enable_miller_abrahams) {
										exciton_event_calc_vars.dissociations_temp[index].calculateRateConstant(params.R_exciton_dissociation_acceptor, params.Triplet_localization_acceptor, exciton_event_calc_vars.distances[index], E_delta);
									}
									else {
										exciton_event_calc_vars.dissociations_temp[index].calculateRateConstant(params.R_exciton_dissociation_acceptor, params.Triplet_localization_acceptor, exciton_event_calc_vars.distances[index], E_delta, params.Reorganization_acceptor);
									}
								}
							}
							// Save the calculated exciton dissociation event as a possible event
							possible_events.push_back(&exciton_event_calc_vars.dissociations_temp[index]);
						}
						// Hop event
						if (exciton_event_calc_vars.isInFRETRange[index]) {
							exciton_event_calc_vars.hops_temp[index].setObjectPtr(exciton_ptr);
							exciton_event_calc_vars.hops_temp[index].setDestCoords(dest_coords);
							E_delta = (getSiteEnergy(dest_coords) - getSiteEnergy(object_coords));
							// Singlet FRET hopping
							if (exciton_it->getSpin()) {
								if (getSiteType(object_coords) == (short)1) {
									// donor-to-acceptor energy modification
									if (getSiteType(dest_coords) == (short)2) {
										E_delta += (params.Homo_acceptor - params.Lumo_acceptor - params.E_exciton_binding_acceptor) - (params.Homo_donor - params.Lumo_donor - params.E_exciton_binding_donor);
									}
									exciton_event_calc_vars.hops_temp[index].calculateRateConstant(params.R_singlet_hopping_donor, exciton_event_calc_vars.distances[index], E_delta);
								}
								else {
									// acceptor-to-donor energy modification
									if (getSiteType(dest_coords) == (short)1) {
										E_delta += (params.Homo_donor - params.Lumo_donor - params.E_exciton_binding_donor) - (params.Homo_acceptor - params.Lumo_acceptor - params.E_exciton_binding_acceptor);
									}
									exciton_event_calc_vars.hops_temp[index].calculateRateConstant(params.R_singlet_hopping_acceptor, exciton_event_calc_vars.distances[index], E_delta);
								}
							}
							// Dexter hopping is only donor-to-donor and acceptor-to-acceptor
							else {
								if (getSiteType(object_coords) == (short)1) {
									exciton_event_calc_vars.hops_temp[index].calculateRateConstant(params.R_triplet_hopping_donor, params.Triplet_localization_donor, exciton_event_calc_vars.distances[index], E_delta);
								}
								else {
									exciton_event_calc_vars.hops_temp[index].calculateRateConstant(params.R_triplet_hopping_donor, params.Triplet_localization_acceptor, exciton_event_calc_vars.distances[index], E_delta);
								}
							}
							// Save the calculated exciton hop event as a possible event
							possible_events.push_back(&exciton_event_calc_vars.hops_temp[index]);
						}
					}
				}
			}
		}
		// Exciton Recombination
		auto recombination_event_it = find_if(exciton_recombination_events.begin(), exciton_recombination_events.end(), [exciton_ptr](Exciton::Recombination& a) { return a.getObjectPtr() == exciton_ptr; });
		if (exciton_it->getSpin()) {
			if (getSiteType(object_coords) == (short)1) {
				rate = 1.0 / params.Singlet_lifetime_donor;
			}
			else if (getSiteType(object_coords) == (short)2) {
				rate = 1.0 / params.Singlet_lifetime_acceptor;
			}
		}
		else {
			if (getSiteType(object_coords) == (short)1) {
				rate = 1.0 / params.Triplet_lifetime_donor;
			}
			else if (getSiteType(object_coords) == (short)2) {
				rate = 1.0 / params.Triplet_lifetime_acceptor;
			}
		}
		recombination_event_it->calculateRateConstant(rate);
		// Save the calculated exciton recombination event as a possible event
		possible_events.push_back(&(*recombination_event_it));
		// Exciton Intersystem Crossing
		auto intersystem_crossing_event_it = find_if(exciton_intersystem_crossing_events.begin(), exciton_intersystem_crossing_events.end(), [exciton_ptr](Exciton::Intersystem_Crossing& a) { return a.getObjectPtr() == exciton_ptr; });
		// ISC
		if (exciton_it->getSpin()) {
			if (getSiteType(object_coords) == (short)1) {
				intersystem_crossing_event_it->calculateRateConstant(params.R_exciton_isc_donor, 0.0);
			}
			else if (getSiteType(object_coords) == (short)2) {
				intersystem_crossing_event_it->calculateRateConstant(params.R_exciton_isc_acceptor, 0.0);
			}
		}
		// RISC
		else {
			if (getSiteType(object_coords) == (short)1) {
				intersystem_crossing_event_it->calculateRateConstant(params.R_exciton_risc_donor, params.E_exciton_ST_donor);
			}
			else if (getSiteType(object_coords) == (short)2) {
				intersystem_crossing_event_it->calculateRateConstant(params.R_exciton_risc_acceptor, params.E_exciton_ST_acceptor);
			}
		}
		// Save the calculated exciton ISC/RISC event as a possible event
		possible_events.push_back(&(*intersystem_crossing_event_it));
		// Check for no valid events
		if (possible_events.size() == 0) {
			setObjectEvent(exciton_ptr, nullptr);
			cout << getId() << ": Error! No valid exciton events could be calculated." << endl;
			setErrorMessage("No valid exciton events could be calculated.");
			Error_found = true;
			return;
		}
		// Determine which event will be selected
		Event* event_ptr_target = determinePathway(possible_events);
		// Check that the execution time is valid
		if (event_ptr_target->getExecutionTime() < getTime()) {
			setObjectEvent(exciton_ptr, nullptr);
			cout << getId() << ": Error! The fastest exciton event execution time is less than the current simulation time." << endl;
			setErrorMessage(" The fastest exciton event execution time is less than the current simulation time.");
			Error_found = true;
			return;
		}
		// Copy the chosen temp event to the appropriate main event list and set the target event pointer to the corresponding event from the main list
		string event_type = event_ptr_target->getEventType();
		if (event_type.compare(Exciton::Hop::event_type) == 0) {
			auto hop_list_it = exciton_hop_events.begin();
			std::advance(hop_list_it, std::distance(excitons.begin(), exciton_it));
			*hop_list_it = *static_cast<Exciton::Hop*>(event_ptr_target);
			event_ptr_target = &(*hop_list_it);
		}
		else if (event_type.compare(Exciton::Dissociation::event_type) == 0) {
			auto dissociation_list_it = exciton_dissociation_events.begin();
			std::advance(dissociation_list_it, std::distance(excitons.begin(), exciton_it));
			*dissociation_list_it = *static_cast<Exciton::Dissociation*>(event_ptr_target);
			event_ptr_target = &(*dissociation_list_it);
		}
		else if (event_type.compare(Exciton::Exciton_Annihilation::event_type) == 0) {
			auto exciton_exciton_annihilation_list_it = exciton_exciton_annihilation_events.begin();
			std::advance(exciton_exciton_annihilation_list_it, std::distance(excitons.begin(), exciton_it));
			*exciton_exciton_annihilation_list_it = *static_cast<Exciton::Exciton_Annihilation*>(event_ptr_target);
			event_ptr_target = &(*exciton_exciton_annihilation_list_it);
		}
		else if (event_type.compare(Exciton::Polaron_Annihilation::event_type) == 0) {
			auto exciton_polaron_annihilation_list_it = exciton_polaron_annihilation_events.begin();
			std::advance(exciton_polaron_annihilation_list_it, std::distance(excitons.begin(), exciton_it));
			*exciton_polaron_annihilation_list_it = *static_cast<Exciton::Polaron_Annihilation*>(event_ptr_target);
			event_ptr_target = &(*exciton_polaron_annihilation_list_it);
		}
		// Set the chosen event
		setObjectEvent(exciton_ptr, event_ptr_target);
	}

	void OSC_Sim::calculateObjectListEvents(const vector<Object*>& object_ptr_vec) {
		if (isLoggingEnabled()) {
			*Logfile << "Calculating events for " << object_ptr_vec.size() << " objects:" << endl;
		}
		for (auto &item : object_ptr_vec) {
			// If object is exciton
			if (item->getObjectType().compare(Exciton::object_type) == 0) {
				calculateExcitonEvents(static_cast<Exciton*>(item));
			}
			// If object is polaron
			else if (item->getObjectType().compare(Polaron::object_type) == 0) {
				calculatePolaronEvents(static_cast<Polaron*>(item));
			}
		}
	}

	void OSC_Sim::calculatePolaronEvents(Polaron* polaron_ptr) {
		const auto polaron_it = getPolaronIt(polaron_ptr);
		const Coords object_coords = polaron_it->getCoords();
		if (isLoggingEnabled()) {
			if (!polaron_it->getCharge()) {
				*Logfile << "Calculating events for electron " << polaron_it->getTag() << " at site " << object_coords.x << "," << object_coords.y << "," << object_coords.z << "." << endl;
			}
			else {
				*Logfile << "Calculating events for hole " << polaron_it->getTag() << " at site " << object_coords.x << "," << object_coords.y << "," << object_coords.z << "." << endl;
			}
		}
		if (params.Enable_phase_restriction && !polaron_it->getCharge() && getSiteType(object_coords) == (short)1) {
			cout << "Error! Electron is on a donor site and should not be with phase restriction enabled." << endl;
			setErrorMessage("Electron is on a donor site and should not be with phase restriction enabled.");
			Error_found = true;
			return;
		}
		if (params.Enable_phase_restriction && polaron_it->getCharge() && getSiteType(object_coords) == (short)2) {
			cout << "Error! Hole is on an acceptor site and should not be with phase restriction enabled." << endl;
			setErrorMessage("Hole is on an acceptor site and should not be with phase restriction enabled.");
			Error_found = true;
			return;
		}
		Coords dest_coords;
		int index;
		double E_site_i = getSiteEnergy(object_coords);
		double Coulomb_i = calculateCoulomb(polaron_it, object_coords);
		vector<Event*> possible_events;
		// Calculate Polaron hopping and recombination events
		for (int i = -polaron_event_calc_vars.range, imax = polaron_event_calc_vars.range; i <= imax; i++) {
			for (int j = -polaron_event_calc_vars.range, jmax = polaron_event_calc_vars.range; j <= jmax; j++) {
				for (int k = -polaron_event_calc_vars.range, kmax = polaron_event_calc_vars.range; k <= kmax; k++) {
					index = (i + polaron_event_calc_vars.range)*polaron_event_calc_vars.dim*polaron_event_calc_vars.dim + (j + polaron_event_calc_vars.range)*polaron_event_calc_vars.dim + (k + polaron_event_calc_vars.range);
					if (!polaron_event_calc_vars.isInRange[index]) {
						continue;
					}
					if (!lattice.checkMoveValidity(object_coords, i, j, k)) {
						continue;
					}
					lattice.calculateDestinationCoords(object_coords, i, j, k, dest_coords);
					// Recombination events
					// If destination site is occupied by a hole Polaron and the main Polaron is an electron, check for a possible recombination event
					if (lattice.isOccupied(dest_coords) && !polaron_it->getCharge() && siteContainsHole(dest_coords)) {
						if (getSiteType(object_coords) == (short)1) {
							polaron_event_calc_vars.recombinations_temp[index].calculateRateConstant(params.R_polaron_recombination, params.Polaron_localization_donor, polaron_event_calc_vars.distances[index], 0);
						}
						else if (getSiteType(object_coords) == (short)2) {
							polaron_event_calc_vars.recombinations_temp[index].calculateRateConstant(params.R_polaron_recombination, params.Polaron_localization_acceptor, polaron_event_calc_vars.distances[index], 0);
						}
						polaron_event_calc_vars.recombinations_temp[index].setObjectPtr(polaron_ptr);
						polaron_event_calc_vars.recombinations_temp[index].setDestCoords(dest_coords);
						polaron_event_calc_vars.recombinations_temp[index].setObjectTargetPtr((*lattice.getSiteIt(dest_coords))->getObjectPtr());
						possible_events.push_back(&polaron_event_calc_vars.recombinations_temp[index]);
					}
					// Hop events
					// If destination site is unoccupied and either phase restriction is disabled or the starting site and destination sites have the same type, check for a possible hop event
					if (!lattice.isOccupied(dest_coords) && (!params.Enable_phase_restriction || getSiteType(object_coords) == getSiteType(dest_coords))) {
						polaron_event_calc_vars.E_deltas[index] = (getSiteEnergy(dest_coords) - E_site_i);
						polaron_event_calc_vars.E_deltas[index] += (calculateCoulomb(polaron_it, dest_coords) - Coulomb_i);
						double E_potential_change = (E_potential[dest_coords.z] - E_potential[object_coords.z]);
						if (lattice.calculateDZ(object_coords, dest_coords) < 0) {
							E_potential_change -= params.Internal_potential;
						}
						if (lattice.calculateDZ(object_coords, dest_coords) > 0) {
							E_potential_change += params.Internal_potential;
						}
						if (!polaron_it->getCharge()) {
							polaron_event_calc_vars.E_deltas[index] += E_potential_change;
						}
						else {
							polaron_event_calc_vars.E_deltas[index] -= E_potential_change;
						}
						if (getSiteType(object_coords) == (short)1) {
							if (getSiteType(dest_coords) == (short)2) {
								if (!polaron_it->getCharge()) {
									polaron_event_calc_vars.E_deltas[index] -= (params.Lumo_acceptor - params.Lumo_donor);
								}
								else {
									polaron_event_calc_vars.E_deltas[index] -= (params.Homo_acceptor - params.Homo_donor);
								}
							}
							if (params.Enable_miller_abrahams) {
								polaron_event_calc_vars.hops_temp[index].calculateRateConstant(params.R_polaron_hopping_donor, params.Polaron_localization_donor, polaron_event_calc_vars.distances[index], polaron_event_calc_vars.E_deltas[index]);
							}
							else {
								polaron_event_calc_vars.hops_temp[index].calculateRateConstant(params.R_polaron_hopping_donor, params.Polaron_localization_donor, polaron_event_calc_vars.distances[index], polaron_event_calc_vars.E_deltas[index], params.Reorganization_donor);
							}
						}
						else if (getSiteType(object_coords) == (short)2) {
							if (getSiteType(dest_coords) == (short)1) {
								if (!polaron_it->getCharge()) {
									polaron_event_calc_vars.E_deltas[index] -= (params.Lumo_donor - params.Lumo_acceptor);
								}
								else {
									polaron_event_calc_vars.E_deltas[index] -= (params.Homo_donor - params.Homo_acceptor);
								}
							}
							if (params.Enable_miller_abrahams) {
								polaron_event_calc_vars.hops_temp[index].calculateRateConstant(params.R_polaron_hopping_acceptor, params.Polaron_localization_acceptor, polaron_event_calc_vars.distances[index], polaron_event_calc_vars.E_deltas[index]);
							}
							else {
								polaron_event_calc_vars.hops_temp[index].calculateRateConstant(params.R_polaron_hopping_acceptor, params.Polaron_localization_acceptor, polaron_event_calc_vars.distances[index], polaron_event_calc_vars.E_deltas[index], params.Reorganization_acceptor);
							}
						}
						polaron_event_calc_vars.hops_temp[index].setObjectPtr(polaron_ptr);
						polaron_event_calc_vars.hops_temp[index].setDestCoords(dest_coords);
						polaron_event_calc_vars.hops_temp[index].setObjectTargetPtr(nullptr);
						possible_events.push_back(&polaron_event_calc_vars.hops_temp[index]);
					}
				}
			}
		}
		// Calculate possible polaron extraction event
		// Electrons are extracted at the bottom of the lattice (z=-1)
		// Holes are extracted at the top of the lattice (z=Height)
		if ((params.Enable_dynamics_test && params.Enable_dynamics_extraction) || (!params.Enable_dynamics_test && !params.Enable_steady_transport_test)) {
			bool Extraction_valid = false;
			list<Polaron::Extraction>::iterator extraction_event_it;
			double distance;
			// If electron, charge is false
			if (!polaron_it->getCharge()) {
				distance = lattice.getUnitSize()*((double)(object_coords.z + 1) - 0.5);
				if (!((distance - 0.0001) > params.Polaron_hopping_cutoff)) {
					extraction_event_it = electron_extraction_events.begin();
					std::advance(extraction_event_it, std::distance(electrons.begin(), polaron_it));
					Extraction_valid = true;
				}
			}
			// If hole, charge is true
			else {
				distance = lattice.getUnitSize()*((double)(lattice.getHeight() - object_coords.z) - 0.5);
				if (!((distance - 0.0001) > params.Polaron_hopping_cutoff)) {
					extraction_event_it = hole_extraction_events.begin();
					std::advance(extraction_event_it, std::distance(holes.begin(), polaron_it));
					Extraction_valid = true;
				}
			}
			if (Extraction_valid) {
				if (getSiteType(object_coords) == (short)1) {
					extraction_event_it->calculateRateConstant(params.R_polaron_hopping_donor, distance, params.Polaron_localization_donor, 0);
				}
				else if (getSiteType(object_coords) == (short)2) {
					extraction_event_it->calculateRateConstant(params.R_polaron_hopping_acceptor, distance, params.Polaron_localization_acceptor, 0);
				}
				possible_events.push_back(&(*extraction_event_it));
			}
		}
		// If there are no possible events, set the polaron event ptr to nullptr
		if (possible_events.size() == 0) {
			setObjectEvent(polaron_ptr, nullptr);
			return;
		}
		// Determine the next event
		Event* event_ptr_target = determinePathway(possible_events);
		// Check that the execution time is valid
		if (event_ptr_target->getExecutionTime() < getTime()) {
			setObjectEvent(polaron_ptr, nullptr);
			cout << getId() << ": Error! The fastest polaron event execution time is less than the current simulation time." << endl;
			setErrorMessage(" The fastest polaron event execution time is less than the current simulation time.");
			Error_found = true;
			return;
		}
		// Copy the chosen temp event to the appropriate main event list and set the target event pointer to the corresponding event from the main list
		string event_type = event_ptr_target->getEventType();
		if (event_type.compare(Polaron::Hop::event_type) == 0) {
			list<Polaron::Hop>::iterator hop_list_it;
			// If electron, charge is false
			if (!polaron_it->getCharge()) {
				hop_list_it = electron_hop_events.begin();
				std::advance(hop_list_it, std::distance(electrons.begin(), polaron_it));
			}
			// If hole, charge is true
			else {
				hop_list_it = hole_hop_events.begin();
				std::advance(hop_list_it, std::distance(holes.begin(), polaron_it));
			}
			*hop_list_it = *static_cast<Polaron::Hop*>(event_ptr_target);
			event_ptr_target = &(*hop_list_it);
		}
		else if (event_type.compare(Polaron::Recombination::event_type) == 0) {
			list<Polaron::Recombination>::iterator recombination_list_it;
			// If electron, charge is false
			if (!polaron_it->getCharge()) {
				recombination_list_it = polaron_recombination_events.begin();
				std::advance(recombination_list_it, std::distance(electrons.begin(), polaron_it));
			}
			// If hole, charge is true
			else {
				setObjectEvent(polaron_ptr, nullptr);
				cout << getId() << ": Error! Only electrons can initiate polaron recombination." << endl;
				setErrorMessage("Error calculating polaron events. Only electrons can initiate polaron recombination.");
				Error_found = true;
				return;
			}
			*recombination_list_it = *static_cast<Polaron::Recombination*>(event_ptr_target);
			event_ptr_target = &(*recombination_list_it);
		}
		// Set the chosen event
		setObjectEvent(polaron_ptr, event_ptr_target);
	}

	bool OSC_Sim::checkFinished() const {
		if (Error_found) {
			cout << getId() << ": An error has been detected and the simulation will now end." << endl;
			return true;
		}
		if (params.Enable_exciton_diffusion_test) {
			return ((N_singlet_excitons_recombined + N_triplet_excitons_recombined) == params.N_tests);
		}
		if (params.Enable_dynamics_test) {
			return (N_excitons == 0 && N_electrons == 0 && N_holes == 0 && N_excitons_created >= params.N_tests);
		}
		if (params.Enable_ToF_test) {
			return ((N_electrons == 0 && N_electrons_created >= params.N_tests) || (N_holes == 0 && N_holes_created >= params.N_tests));
		}
		if (params.Enable_IQE_test) {
			if (N_excitons_created == params.N_tests && N_excitons == 0 && N_electrons == 0 && N_holes == 0) {
				return true;
			}
			if (N_excitons_created == params.N_tests && getTime() > params.IQE_time_cutoff) {
				return true;
			}
			return false;
		}
		if (params.Enable_steady_transport_test) {
			return (N_events_executed == (params.N_equilibration_events + params.N_tests));
		}
		cout << getId() << ": Error checking simulation finish conditions.  The simulation will now end." << endl;
		return true;
	}

	void OSC_Sim::createCorrelatedDOS(const double correlation_length) {
		double stdev, percent_diff;
		double scale_factor = 1;
		if (params.Enable_gaussian_kernel) {
			scale_factor = -0.07*exp((correlation_length - 1) / -0.21) - 0.09*exp((correlation_length - 1) / -0.9);
		}
		if (params.Enable_power_kernel && params.Power_kernel_exponent == -1) {
			scale_factor = 0.7 + 3.1*pow(correlation_length, -1.55);
		}
		if (params.Enable_power_kernel && params.Power_kernel_exponent == -2) {
			scale_factor = -0.4 + 2.2*pow(correlation_length, -0.74);
			scale_factor = pow(scale_factor, 2);
		}
		// Save original site energies
		vector<float> original_energies((int)sites.size());
		for (int n = 0; n < (int)sites.size(); n++) {
			original_energies[n] = sites[n].getEnergy();
		}
		vector<float> new_energies((int)sites.size());
		int range = 2;
		while (1) {
			// Create and calculates distances and range check vectors
			int dim = 2 * range + 1;
			int vec_size = dim * dim*dim;
			vector<float> distances(vec_size, 0.0f);
			vector<bool> isInRange(vec_size, false);
			vector<int> distance_indices(vec_size, 0);
			for (int i = -range; i <= range; i++) {
				for (int j = -range; j <= range; j++) {
					for (int k = -range; k <= range; k++) {
						int index = (i + range)*dim*dim + (j + range)*dim + (k + range);
						distance_indices[index] = i * i + j * j + k * k;
						distances[index] = (float)lattice.getUnitSize()*sqrt((float)(i*i + j * j + k * k));
						if (i == 0 && j == 0 && k == 0) {
							distances[index] = -1.0f;
						}
						else if (distance_indices[index] < range*range) {
							isInRange[index] = true;
						}
					}
				}
			}
			// Impart correlation
			vector<float> isAble(vec_size, 0.0f);
			vector<float> energies_temp(vec_size, 0.0f);
			vector<int> counts(range*range + 1, 0);
			for (int n = 0, nmax = (int)sites.size(); n < nmax; n++) {
				isAble.assign(vec_size, 0.0f);
				energies_temp.assign(vec_size, 0.0f);
				Coords coords = lattice.getSiteCoords(n);
				// Get nearby site energies and determine if able
				for (int i = -range; i <= range; i++) {
					for (int j = -range; j <= range; j++) {
						for (int k = -range; k <= range; k++) {
							if (!lattice.checkMoveValidity(coords, i, j, k)) {
								continue;
							}
							Coords dest_coords;
							lattice.calculateDestinationCoords(coords, i, j, k, dest_coords);
							int index = (i + range)*dim*dim + (j + range)*dim + (k + range);
							if (isInRange[index]) {
								energies_temp[index] = getSiteEnergy(dest_coords);
								isAble[index] = 1.0f;
							}
						}
					}
				}
				if (params.Enable_gaussian_kernel) {
					for (int m = 0; m < vec_size; m++) {
						energies_temp[m] = isAble[m] * energies_temp[m] * exp((float)scale_factor * distances[m] * distances[m]);
					}
				}
				if (params.Enable_power_kernel && params.Power_kernel_exponent == -1) {
					for (int m = 0; m < vec_size; m++) {
						energies_temp[m] = isAble[m] * energies_temp[m] / ((float)scale_factor * distances[m]);
					}
				}
				if (params.Enable_power_kernel && params.Power_kernel_exponent == -2) {
					for (int m = 0; m < vec_size; m++) {
						energies_temp[m] = isAble[m] * energies_temp[m] / ((float)scale_factor * distances[m] * distances[m]);
					}
				}
				// Normalize energies by site count
				counts.assign(range*range + 1, 0);
				for (int m = 0; m < vec_size; m++) {
					if (isAble[m] > 0.1f && distance_indices[m] < (int)counts.size()) {
						counts[distance_indices[m]] ++;
					}
				}
				for (int m = 0; m < vec_size; m++) {
					if (isAble[m] > 0.1f && distance_indices[m] < (int)counts.size()) {
						energies_temp[m] /= counts[distance_indices[m]];
					}
				}
				new_energies[n] = accumulate(energies_temp.begin(), energies_temp.end(), getSiteEnergy(coords));
			}
			// Normalize energies to reach desired disorder
			stdev = vector_stdev(new_energies);
			percent_diff = (stdev - params.Energy_stdev_donor) / params.Energy_stdev_donor;
			double norm_factor = 1 + percent_diff;
			for (auto &item : new_energies) {
				item /= (float)norm_factor;
			}
			// Assign new energies to the sites
			for (int n = 0; n < (int)sites.size(); n++) {
				sites[n].setEnergy(new_energies[n]);
			}
			// Calculate the correlation function
			calculateDOSCorrelation();
			// Check if finished
			if ((int)DOS_correlation_data.size() - 1 < 2 * range) {
				break;
			}
			// If not finished
			else {
				// Reassign original site energies
				for (int n = 0; n < (int)sites.size(); n++) {
					sites[n].setEnergy(original_energies[n]);
				}
				// Increment range and repeat the calculation
				range += 2;
			}
		}
	}

	void OSC_Sim::createElectron(const Coords& coords) {
		// Check that coords are valid
		try {
			lattice.getSiteIndex(coords);
		}
		catch (out_of_range exception) {
			cout << "Error! Electron cannot be generated because the input coordinates are invalid." << endl;
			setErrorMessage("Electron cannot be generated because the input coordinates are invalid.");
			Error_found = true;
			return;
		}
		// Check that the site type is valid
		if (params.Enable_phase_restriction && getSiteType(coords) == 1) {
			cout << "Error! Electron cannot be generated on a donor site." << endl;
			setErrorMessage("Electron cannot be generated on a donor site.");
			Error_found = true;
			return;
		}
		generateElectron(coords, 0);
	}

	void OSC_Sim::createExciton(const bool spin) {
		auto coords = calculateRandomExcitonCreationCoords();
		if (coords.x == -1 && coords.y == -1 && coords.z == -1) {
			return;
		}
		generateExciton(coords, spin, 0);
	}

	void OSC_Sim::createExciton(const KMC_Lattice::Coords& coords, const bool spin) {
		// Check that coords are valid
		try {
			lattice.getSiteIndex(coords);
		}
		catch (out_of_range exception) {
			cout << "Error! Exciton cannot be generated because the input coordinates are invalid." << endl;
			setErrorMessage("Exciton cannot be generated because the input coordinates are invalid.");
			Error_found = true;
			return;
		}
		if (lattice.isOccupied(coords)) {
			cout << "Error! Exciton cannot be generated because the input coordinates are occupied." << endl;
			setErrorMessage("Exciton cannot be generated because the input coordinates are occupied.");
			Error_found = true;
			return;
		}
		generateExciton(coords, spin, 0);
	}

	void OSC_Sim::createHole(const Coords& coords) {
		// Check that coords are valid
		try {
			lattice.getSiteIndex(coords);
		}
		catch (out_of_range exception) {
			cout << "Error! Hole cannot be generated because the input coordinates are invalid." << endl;
			setErrorMessage("Hole cannot be generated because the input coordinates are invalid.");
			Error_found = true;
			return;
		}
		// Check that the site type is valid
		if (params.Enable_phase_restriction && getSiteType(coords) == 2) {
			cout << "Error! Hole cannot be generated on an acceptor site." << endl;
			setErrorMessage("Hole cannot be generated on an acceptor site.");
			Error_found = true;
			return;
		}
		generateHole(coords, 0);
	}

	bool OSC_Sim::createImportedMorphology() {
		string file_info;
		string line;
		stringstream ss;
		Coords coords;
		int length, width, height;
		bool isV3 = false;
		bool isV4 = false;
		bool isCompressed = false;
		// Open morphology file
		ifstream morphology_file(params.Morphology_filename.c_str(), ifstream::in);
		// Check if morphology file exists and is accessible
		if (!morphology_file.good()) {
			cout << getId() << ": Error opening morphology file for importing." << endl;
			setErrorMessage("Morphology file could not be opened for importing.");
			Error_found = true;
			morphology_file.close();
			return false;
		}
		// Get input morphology file information from header line
		getline(morphology_file, line);
		file_info = line;
		// Analyze file header line
		if (file_info.substr(0, 9).compare("Ising_OPV") == 0) {
			// extract version string
			string version_str = file_info;
			version_str.erase(0, version_str.find('v') + 1);
			version_str = version_str.substr(0, version_str.find(' '));
			Version file_version(version_str);
			// check if morphology file version is greater than or equal to the minimum version
			Version min_version("3.2");
			if (file_version < min_version) {
				cout << getId() << ": Error! Morphology file format not recognized. Only morphologies created using Ising_OPV v3.2 and v4.0 or greater are currently supported." << endl;
				setErrorMessage("Morphology file format not recognized. Only morphologies created using Ising_OPV v3.2 and v4.0 or greater are currently supported.");
				Error_found = true;
				morphology_file.close();
				return false;
			}
			if (file_version == min_version) {
				isV3 = true;
			}
			else if (file_version >= Version("4.0.0-beta.1")) {
				isV4 = true;
			}
			// Check if file is in compressed format or not
			isCompressed = (file_info.find("uncompressed") == string::npos);
		}
		else {
			cout << getId() << ": Error! Morphology file format not recognized. Only morphologies created using Ising_OPV v3.2 and v4.0 or greater are currently supported." << endl;
			setErrorMessage("Morphology file format not recognized. Only morphologies created using Ising_OPV v3.2 and v4.0 or greater are currently supported.");
			Error_found = true;
			morphology_file.close();
			return false;
		}
		// Parse file info
		getline(morphology_file, line);
		length = atoi(line.c_str());
		getline(morphology_file, line);
		width = atoi(line.c_str());
		getline(morphology_file, line);
		height = atoi(line.c_str());
		if (lattice.getLength() != length || lattice.getWidth() != width || lattice.getHeight() != height) {
			cout << getId() << ": Error! Morphology lattice dimensions do not match the lattice dimensions defined in the parameter file." << endl;
			setErrorMessage("Morphology lattice dimensions do not match the lattice dimensions defined in the parameter file.");
			Error_found = true;
			morphology_file.close();
			return false;
		}
		if (isV3) {
			// Skip 3 lines (domain size1, domain size2, blend ratio)
			getline(morphology_file, line);
			getline(morphology_file, line);
			getline(morphology_file, line);
		}
		else if (isV4) {
			// skip boundary conditions
			getline(morphology_file, line);
			getline(morphology_file, line);
			getline(morphology_file, line);
			// number of site types
			getline(morphology_file, line);
			int N_types = atoi(line.c_str());
			// skip domain size and mix fraction lines
			for (int i = 0; i < 2 * N_types; i++) {
				getline(morphology_file, line);
			}
		}
		// Begin parsing morphology site data
		if (!isCompressed) {
			while (getline(morphology_file, line)) {
				stringstream linestream(line);
				string item;
				vector<int> values;
				values.reserve(4);
				while (getline(linestream, item, ',')) {
					values.push_back(atoi(item.c_str()));
				}
				coords.setXYZ(values[0], values[1], values[2]);
				sites[lattice.getSiteIndex(coords)].setType((char)values[3]);
				if (values[3] == 1) {
					N_donor_sites++;
				}
				else if (values[3] == 2) {
					N_acceptor_sites++;
				}
			}
		}
		else {
			int site_count = 0;
			short type = 0;
			for (int x = 0; x < lattice.getLength(); x++) {
				for (int y = 0; y < lattice.getWidth(); y++) {
					for (int z = 0; z < lattice.getHeight(); z++) {
						if (site_count == 0) {
							getline(morphology_file, line);
							if (!(morphology_file).good()) {
								cout << "Error parsing file.  End of file reached before expected." << endl;
								setErrorMessage("Error parsing imported morphology file.  End of file reached before expected.");
								Error_found = true;
								morphology_file.close();
								return false;
							}
							type = (short)atoi(line.substr(0, 1).c_str());
							site_count = atoi(line.substr(1).c_str());
						}
						coords.setXYZ(x, y, z);
						sites[lattice.getSiteIndex(coords)].setType(type);
						if (type == (short)1) {
							N_donor_sites++;
						}
						else if (type == (short)2) {
							N_acceptor_sites++;
						}
						site_count--;
					}
				}
			}
		}
		// Close the morphology file
		morphology_file.close();
		// Check for unassigned sites
		for (auto const &item : sites) {
			if (item.getType() == (short)0) {
				cout << getId() << ": Error! Unassigned site found after morphology import. Check the morphology file for errors." << endl;
				setErrorMessage("Unassigned site found after morphology import. Check the morphology file for errors.");
				Error_found = true;
				return false;
			}
		}
		return true;
	}

	void OSC_Sim::deleteObject(Object* object_ptr) {
		if (object_ptr->getObjectType().compare(Exciton::object_type) == 0) {
			auto exciton_it = getExcitonIt(object_ptr);
			// Remove the object from Simulation
			removeObject(object_ptr);
			// Locate corresponding recombination event
			auto recombination_list_it = exciton_recombination_events.begin();
			std::advance(recombination_list_it, std::distance(excitons.begin(), exciton_it));
			// Locate corresponding hop event
			auto hop_list_it = exciton_hop_events.begin();
			std::advance(hop_list_it, std::distance(excitons.begin(), exciton_it));
			// Locate corresponding dissociation event
			auto dissociation_list_it = exciton_dissociation_events.begin();
			std::advance(dissociation_list_it, std::distance(excitons.begin(), exciton_it));
			// Locate corresponding exciton-exciton annihilation event
			auto exciton_exciton_annihilation_list_it = exciton_exciton_annihilation_events.begin();
			std::advance(exciton_exciton_annihilation_list_it, std::distance(excitons.begin(), exciton_it));
			// Locate corresponding exciton-polaron annihilation event
			auto exciton_polaron_annihilation_list_it = exciton_polaron_annihilation_events.begin();
			std::advance(exciton_polaron_annihilation_list_it, std::distance(excitons.begin(), exciton_it));
			// Locate corresponding exciton intersystem crossing event
			auto intersystem_crossing_list_it = exciton_intersystem_crossing_events.begin();
			std::advance(intersystem_crossing_list_it, std::distance(excitons.begin(), exciton_it));
			// Delete exciton
			excitons.erase(exciton_it);
			// Delete exciton recombination event
			exciton_recombination_events.erase(recombination_list_it);
			// Delete exciton hop event
			exciton_hop_events.erase(hop_list_it);
			// Delete exciton dissociation event
			exciton_dissociation_events.erase(dissociation_list_it);
			// Delete exciton-exciton annihilation event
			exciton_exciton_annihilation_events.erase(exciton_exciton_annihilation_list_it);
			// Delete exciton-polaron annihilation event
			exciton_polaron_annihilation_events.erase(exciton_polaron_annihilation_list_it);
			// Delete exciton intersystem crossing event
			exciton_intersystem_crossing_events.erase(intersystem_crossing_list_it);
		}
		else if (object_ptr->getObjectType().compare(Polaron::object_type) == 0) {
			auto polaron_it = getPolaronIt(object_ptr);
			// Remove the object from Simulation
			removeObject(object_ptr);
			// Electron
			if (!(polaron_it->getCharge())) {
				// Locate corresponding recombination event
				auto recombination_list_it = polaron_recombination_events.begin();
				std::advance(recombination_list_it, std::distance(electrons.begin(), polaron_it));
				// Locate corresponding hop event
				auto hop_list_it = electron_hop_events.begin();
				std::advance(hop_list_it, std::distance(electrons.begin(), polaron_it));
				// Locate corresponding extraction event
				auto extraction_list_it = electron_extraction_events.begin();
				std::advance(extraction_list_it, std::distance(electrons.begin(), polaron_it));
				// Delete electron
				electrons.erase(polaron_it);
				// Delete polaron recombination event
				polaron_recombination_events.erase(recombination_list_it);
				// Delete electron hop event
				electron_hop_events.erase(hop_list_it);
				// Delete electron extraction event
				electron_extraction_events.erase(extraction_list_it);
			}
			// Hole
			else {
				// Locate corresponding hop event
				auto hop_list_it = hole_hop_events.begin();
				std::advance(hop_list_it, std::distance(holes.begin(), polaron_it));
				// Locate corresponding extraction event
				auto extraction_list_it = hole_extraction_events.begin();
				std::advance(extraction_list_it, std::distance(holes.begin(), polaron_it));
				// Delete hole
				holes.erase(polaron_it);
				// Delete hole hop event
				hole_hop_events.erase(hop_list_it);
				// Delete hole extraction event
				hole_extraction_events.erase(extraction_list_it);
			}
		}
	}

	bool OSC_Sim::executeExcitonCreation() {
		// Create new exciton and determine its coordinates
		Coords coords_new = generateExciton();
		// Update event list
		auto recalc_objects = findRecalcObjects(coords_new, coords_new);
		calculateObjectListEvents(recalc_objects);
		// Calculate next exciton creation event
		exciton_creation_events.front().calculateRateConstant(R_exciton_generation_donor + R_exciton_generation_acceptor);
		exciton_creation_events.front().calculateExecutionTime(R_exciton_generation_donor + R_exciton_generation_acceptor);
		return true;
	}

	bool OSC_Sim::executeExcitonDissociation(const list<Event*>::const_iterator event_it) {
		// Get event info
		Coords coords_initial = (((*event_it)->getObjectPtr()))->getCoords();
		Coords coords_dest = (*event_it)->getDestCoords();
		bool spin_state = (getExcitonIt((*event_it)->getObjectPtr()))->getSpin();
		// Delete exciton and its events
		deleteObject((*event_it)->getObjectPtr());
		// Generate new electron and hole
		int tag = (N_electrons_created > N_holes_created) ? (N_electrons_created + 1) : (N_holes_created + 1);
		if (getSiteType(coords_dest) == (short)2) {
			generateHole(coords_initial, tag);
			generateElectron(coords_dest, tag);
		}
		else {
			generateElectron(coords_initial, tag);
			generateHole(coords_dest, tag);
		}
		// Update counters
		if (spin_state) {
			N_singlets--;
			N_singlet_excitons_dissociated++;
		}
		else {
			N_triplets--;
			N_triplet_excitons_dissociated++;
		}
		N_excitons--;

		// Update event list
		auto recalc_objects = findRecalcObjects(coords_initial, coords_dest);
		calculateObjectListEvents(recalc_objects);
		return true;
	}

	bool OSC_Sim::executeExcitonExcitonAnnihilation(const list<Event*>::const_iterator event_it) {
		// Get event info
		auto exciton_it = getExcitonIt((*event_it)->getObjectPtr());
		int exciton_tag = exciton_it->getTag();
		bool spin_state = exciton_it->getSpin();
		int target_tag = ((*event_it)->getObjectTargetPtr())->getTag();
		bool spin_state_target = (getExcitonIt((*event_it)->getObjectTargetPtr()))->getSpin();
		Coords coords_initial = exciton_it->getCoords();
		Coords coords_dest = (*event_it)->getDestCoords();
		// Triplet-triplet annihilation
		if (!spin_state && !spin_state_target) {
			// Target triplet exciton becomes a singlet exciton
			if (rand01() > 0.75) {
				getExcitonIt((*event_it)->getObjectTargetPtr())->flipSpin();
				N_triplets--;
				N_singlets++;
			}
			N_triplet_triplet_annihilations++;
		}
		// Singlet-singlet or singlet-triplet annihilation
		// singlet-singlet
		if (spin_state && spin_state_target) {
			N_singlet_singlet_annihilations++;
		}
		// singlet-triplet
		else if (spin_state && !spin_state_target) {
			N_singlet_triplet_annihilations++;
		}
		// delete exciton and its events
		removeExciton(exciton_it);
		// Log event
		if (isLoggingEnabled()) {
			*Logfile << "Exciton " << exciton_tag << " annihilated at site " << coords_initial.x << "," << coords_initial.y << "," << coords_initial.z;
			*Logfile << " with exciton " << target_tag << " at " << coords_dest.x << "," << coords_dest.y << "," << coords_dest.z << "." << endl;
		}
		// Update event list
		auto recalc_objects = findRecalcObjects(coords_initial, coords_dest);
		calculateObjectListEvents(recalc_objects);
		return true;
	}

	bool OSC_Sim::executeExcitonPolaronAnnihilation(const list<Event*>::const_iterator event_it) {
		// Get event info
		auto object_ptr = (*event_it)->getObjectPtr();
		int exciton_tag = object_ptr->getTag();
		int target_tag = ((*event_it)->getObjectTargetPtr())->getTag();
		bool spin_state = (getExcitonIt((*event_it)->getObjectPtr()))->getSpin();
		Coords coords_initial = object_ptr->getCoords();
		Coords coords_dest = (*event_it)->getDestCoords();
		// delete exciton and its events
		deleteObject((*event_it)->getObjectPtr());
		// Update exciton counters
		N_excitons--;
		if (spin_state) {
			N_singlet_polaron_annihilations++;
			N_singlets--;
		}
		else {
			N_triplet_polaron_annihilations++;
			N_triplets--;
		}
		// Log event
		if (isLoggingEnabled()) {
			*Logfile << "Exciton " << exciton_tag << " annihilated at site " << coords_initial.x << "," << coords_initial.y << "," << coords_initial.z;
			*Logfile << " with polaron " << target_tag << " at " << coords_dest.x << "," << coords_dest.y << "," << coords_dest.z << "." << endl;
		}
		// Update event list
		auto recalc_objects = findRecalcObjects(coords_initial, coords_dest);
		calculateObjectListEvents(recalc_objects);
		return true;
	}

	bool OSC_Sim::executeExcitonHop(const list<Event*>::const_iterator event_it) {
		if (lattice.isOccupied((*event_it)->getDestCoords())) {
			cout << getId() << ": Error! Exciton hop cannot be executed. Destination site " << (*event_it)->getDestCoords().x << "," << (*event_it)->getDestCoords().y << "," << (*event_it)->getDestCoords().z << " is already occupied." << endl;
			outputStatus();
			setErrorMessage("Exciton hop cannot be executed. Destination site is already occupied.");
			Error_found = true;
			return false;
		}
		else {
			if (isLoggingEnabled()) {
				*Logfile << "Exciton " << ((*event_it)->getObjectPtr())->getTag() << " hopping to site " << (*event_it)->getDestCoords().x << "," << (*event_it)->getDestCoords().y << "," << (*event_it)->getDestCoords().z << "." << endl;
			}
			// Log information about the hop when the exciton diffusion test is enabled
			if (params.Enable_exciton_diffusion_test) {
				exciton_hop_distances.push_back(lattice.calculateLatticeDistanceSquared(((*event_it)->getObjectPtr())->getCoords(), (*event_it)->getDestCoords()));
			}
			return executeObjectHop(event_it);
		}
	}

	bool OSC_Sim::executeExcitonIntersystemCrossing(const list<Event*>::const_iterator event_it) {
		// Get event info
		int exciton_tag = ((*event_it)->getObjectPtr())->getTag();
		Coords coords_initial = ((*event_it)->getObjectPtr())->getCoords();
		auto exciton_it = getExcitonIt((*event_it)->getObjectPtr());
		bool spin_i = exciton_it->getSpin();
		// Execute spin flip
		exciton_it->flipSpin();
		// Update exciton counters
		if (spin_i) {
			N_exciton_intersystem_crossings++;
			N_singlets--;
			N_triplets++;
		}
		else {
			N_exciton_reverse_intersystem_crossings++;
			N_triplets--;
			N_singlets++;
		}
		// Log event
		if (isLoggingEnabled()) {
			if (spin_i) {
				*Logfile << "Singlet exciton " << exciton_tag << " at site " << coords_initial.x << "," << coords_initial.y << "," << coords_initial.z << " has become a triplet exciton." << endl;
			}
			else {
				*Logfile << "Triplet exciton " << exciton_tag << " at site " << coords_initial.x << "," << coords_initial.y << "," << coords_initial.z << " has become a singlet exciton." << endl;
			}
		}
		// Update event list
		auto recalc_objects = findRecalcObjects(coords_initial, coords_initial);
		calculateObjectListEvents(recalc_objects);
		return true;
	}

	bool OSC_Sim::executeExcitonRecombination(const list<Event*>::const_iterator event_it) {
		// Get event info
		auto exciton_it = getExcitonIt((*event_it)->getObjectPtr());
		int exciton_tag = exciton_it->getTag();
		Coords coords_initial = exciton_it->getCoords();
		bool spin_state = exciton_it->getSpin();
		// delete exciton and its events
		removeExciton(exciton_it);
		// Update exciton counters
		if (spin_state) {
			N_singlet_excitons_recombined++;
		}
		else {
			N_triplet_excitons_recombined++;
		}
		// Log event
		if (isLoggingEnabled()) {
			*Logfile << "Exciton " << exciton_tag << " recombined at site " << coords_initial.x << "," << coords_initial.y << "," << coords_initial.z << "." << endl;
		}
		// Update event list
		auto recalc_objects = findRecalcObjects(coords_initial, coords_initial);
		calculateObjectListEvents(recalc_objects);
		return true;
	}

	bool OSC_Sim::executeNextEvent() {
		// Check or when to stop generating excitons in the IQE test before executing the next event
		if (params.Enable_IQE_test) {
			if (isLightOn && N_excitons_created == params.N_tests) {
				removeEvent(&exciton_creation_events.front());
				isLightOn = false;
			}
		}
		// Perform Transients test analysis
		if (params.Enable_dynamics_test || params.Enable_ToF_test) {
			updateTransientData();
			// If none of the excitons or polarons can move or the cycle has reached the transient cutoff
			if (getN_events() == 0 || (getTime() - Transient_creation_time) > Transient_end) {
				// Remove any remaining excitons and polarons
				while ((int)excitons.size() > 0) {
					if (excitons.front().getSpin()) {
						N_singlets--;
					}
					else {
						N_triplets--;
					}
					N_excitons--;
					deleteObject(&excitons.front());
				}
				while ((int)electrons.size() > 0) {
					N_electrons--;
					deleteObject(&electrons.front());
				}
				while ((int)holes.size() > 0) {
					N_holes--;
					deleteObject(&holes.front());
				}
			}
			// Check if new excitons or polarons need to be created
			if (N_excitons == 0 && N_holes == 0 && N_electrons == 0 && !checkFinished()) {
				if (params.Enable_ToF_test) {
					generateToFPolarons();
				}
				if (params.Enable_dynamics_test) {
					generateDynamicsExcitons();
				}
			}
			if (checkFinished()) {
				return true;
			}
		}
		// Perform steady transport test analysis
		if (params.Enable_steady_transport_test) {
			updateSteadyData();
		}
		// Choose the next event
		auto event_it = chooseNextEvent();
		// Check for errors
		if (*event_it == nullptr) {
			cout << getId() << ": Error! The simulation has no events to execute." << endl;
			setErrorMessage("The simulation has no events to execute.");
			Error_found = true;
			return false;
		}
		if ((*event_it)->getExecutionTime() < getTime()) {
			cout << getId() << ": Error! The chosen event execution time is less than the current simulation time." << endl;
			setErrorMessage(" The chosen event execution time is less than the current simulation time.");
			Error_found = true;
			return false;
		}
		string event_type = (*event_it)->getEventType();
		if (isLoggingEnabled()) {
			*Logfile << "Event " << N_events_executed << ": Executing " << event_type << " event" << endl;
		}
		previous_event_type = event_type;
		previous_event_time = getTime();
		N_events_executed++;
		// Update simulation time
		setTime((*event_it)->getExecutionTime());
		// Execute the chosen event
		if (event_type.compare(Exciton::Creation::event_type) == 0) {
			return executeExcitonCreation();
		}
		else if (event_type.compare(Exciton::Hop::event_type) == 0) {
			return executeExcitonHop(event_it);
		}
		else if (event_type.compare(Exciton::Recombination::event_type) == 0) {
			return executeExcitonRecombination(event_it);
		}
		else if (event_type.compare(Exciton::Dissociation::event_type) == 0) {
			return executeExcitonDissociation(event_it);
		}
		else if (event_type.compare(Exciton::Exciton_Annihilation::event_type) == 0) {
			return executeExcitonExcitonAnnihilation(event_it);
		}
		else if (event_type.compare(Exciton::Polaron_Annihilation::event_type) == 0) {
			return executeExcitonPolaronAnnihilation(event_it);
		}
		else if (event_type.compare(Exciton::Intersystem_Crossing::event_type) == 0) {
			return executeExcitonIntersystemCrossing(event_it);
		}
		else if (event_type.compare(Polaron::Hop::event_type) == 0) {
			return executePolaronHop(event_it);
		}
		else if (event_type.compare(Polaron::Recombination::event_type) == 0) {
			return executePolaronRecombination(event_it);
		}
		else if (event_type.compare(Polaron::Extraction::event_type) == 0) {
			return executePolaronExtraction(event_it);
		}
		else {
			//error
			cout << getId() << ": Error! Valid event not found when calling executeNextEvent." << endl;
			setErrorMessage("Valid event not found when calling executeNextEvent.");
			Error_found = true;
			return false;
		}
	}

	bool OSC_Sim::executeObjectHop(const list<Event*>::const_iterator event_it) {
		// Get event info
		auto object_ptr = (*event_it)->getObjectPtr();
		Coords coords_initial = object_ptr->getCoords();
		Coords coords_dest = (*event_it)->getDestCoords();
		// Move the object in the Simulation
		moveObject((*event_it)->getObjectPtr(), coords_dest);
		// Update event list
		auto recalc_objects = findRecalcObjects(coords_initial, coords_dest);
		calculateObjectListEvents(recalc_objects);
		return true;
	}

	bool OSC_Sim::executePolaronExtraction(const list<Event*>::const_iterator event_it) {
		// Get event info
		auto polaron_it = getPolaronIt((*event_it)->getObjectPtr());
		bool charge = polaron_it->getCharge();
		int polaron_tag = ((*event_it)->getObjectPtr())->getTag();
		Coords coords_initial = ((*event_it)->getObjectPtr())->getCoords();
		// Save transit time and extraction location info
		if (params.Enable_ToF_test) {
			transit_times.push_back(getTime() - ((*event_it)->getObjectPtr())->getCreationTime());
		}
		if (params.Enable_ToF_test || params.Enable_IQE_test) {
			if (!charge) {
				electron_extraction_data[lattice.getWidth()*coords_initial.x + coords_initial.y]++;
			}
			else {
				hole_extraction_data[lattice.getWidth()*coords_initial.x + coords_initial.y]++;
			}
		}
		// Delete polaron and its events
		deleteObject((*event_it)->getObjectPtr());
		// Update polaron counters
		if (!charge) {
			N_electrons_collected++;
			N_electrons--;
		}
		else {
			N_holes_collected++;
			N_holes--;
		}
		// Log event
		if (isLoggingEnabled()) {
			if (!charge) {
				*Logfile << "Electron " << polaron_tag << " was extracted from site " << coords_initial.x << "," << coords_initial.y << "," << coords_initial.z << "." << endl;
			}
			else {
				*Logfile << "Hole " << polaron_tag << " was extracted from site " << coords_initial.x << "," << coords_initial.y << "," << coords_initial.z << "." << endl;
			}
		}
		auto recalc_objects = findRecalcObjects(coords_initial, coords_initial);
		calculateObjectListEvents(recalc_objects);
		return true;
	}

	bool OSC_Sim::executePolaronHop(const list<Event*>::const_iterator event_it) {
		if (lattice.isOccupied((*event_it)->getDestCoords())) {
			cout << getId() << ": Error! Polaron hop cannot be executed. Destination site " << (*event_it)->getDestCoords().x << "," << (*event_it)->getDestCoords().y << "," << (*event_it)->getDestCoords().z << " is already occupied." << endl;
			outputStatus();
			setErrorMessage("Polaron hop cannot be executed. Destination site is already occupied.");
			Error_found = true;
			return false;
		}
		else {
			auto polaron_it = getPolaronIt((*event_it)->getObjectPtr());
			auto object_coords = polaron_it->getCoords();
			auto dest_coords = (*event_it)->getDestCoords();
			// Log event
			if (isLoggingEnabled()) {
				if (!polaron_it->getCharge()) {
					*Logfile << "Electron " << polaron_it->getTag() << " hopping to site " << dest_coords.x << "," << dest_coords.y << "," << dest_coords.z << "." << endl;
				}
				else {
					*Logfile << "Hole " << polaron_it->getTag() << " hopping to site " << dest_coords.x << "," << dest_coords.y << "," << dest_coords.z << "." << endl;
				}
			}
			// Record data for the steady transport test
			if (params.Enable_steady_transport_test && N_events_executed > params.N_equilibration_events) {
				int displacement = object_coords.z - dest_coords.z;
				// Skip transport energy calculation when the z-direction displacement is zero
				if (displacement == 0) {
					// Do nothing
				}
				else {
					// Correct for hops across the periodic boundaries
					if (2 * displacement > lattice.getHeight()) {
						displacement -= lattice.getHeight();
					}
					else if (2 * displacement < -lattice.getHeight()) {
						displacement += lattice.getHeight();
					}
					// Get initial site energy
					double energy_i;
					if (getSiteType(polaron_it->getCoords()) == 1) {
						energy_i = params.Homo_donor + getSiteEnergy(object_coords);
					}
					else {
						energy_i = params.Homo_acceptor + getSiteEnergy(object_coords);
					}
					// Get final site energy
					double energy_f;
					if (getSiteType((*event_it)->getDestCoords()) == 1) {
						energy_f = params.Homo_donor + getSiteEnergy(dest_coords);
					}
					else {
						energy_f = params.Homo_acceptor + getSiteEnergy(dest_coords);
					}
					// Add to the transport energy weighted sum without Coulomb energy
					Transport_energy_weighted_sum += ((energy_i + energy_f) / 2.0) * displacement;
					// Add to the transport energy weighted sum with Coulomb energy
					energy_i += calculateCoulomb(polaron_it, object_coords);
					energy_f += calculateCoulomb(polaron_it, dest_coords);
					Transport_energy_weighted_sum_Coulomb += ((energy_i + energy_f) / 2.0) * displacement;
					// Add displacement to sum of weights
					Transport_energy_sum_of_weights += displacement;
				}
			}
			return executeObjectHop(event_it);
		}
	}

	bool OSC_Sim::executePolaronRecombination(const list<Event*>::const_iterator event_it) {
		// Get event info
		auto object_ptr = (*event_it)->getObjectPtr();
		int polaron_tag = object_ptr->getTag();
		int target_tag = ((*event_it)->getObjectTargetPtr())->getTag();
		Coords coords_initial = object_ptr->getCoords();
		Coords coords_dest = (*event_it)->getDestCoords();
		// Delete polarons and their events
		deleteObject((*event_it)->getObjectTargetPtr());
		deleteObject(object_ptr);
		// Update polaron counters
		N_electrons_recombined++;
		N_holes_recombined++;
		N_electrons--;
		N_holes--;
		if (polaron_tag == target_tag) {
			N_geminate_recombinations++;
		}
		else {
			N_bimolecular_recombinations++;
		}
		// Log event
		if (isLoggingEnabled()) {
			*Logfile << "Electron " << polaron_tag << " at site " << coords_initial.x << "," << coords_initial.y << "," << coords_initial.z << " recombined with hole " << target_tag << " at site " << coords_dest.x << "," << coords_dest.y << "," << coords_dest.z << "." << endl;
		}
		// Update event list
		auto recalc_objects = findRecalcObjects(coords_initial, coords_dest);
		calculateObjectListEvents(recalc_objects);
		return true;
	}

	void OSC_Sim::exportEnergies(std::string filename) {
		ofstream outfile(filename);
		outfile << lattice.getLength() << endl;
		outfile << lattice.getWidth() << endl;
		outfile << lattice.getHeight() << endl;
		for (int x = 0; x < lattice.getLength(); x++) {
			for (int y = 0; y < lattice.getWidth(); y++) {
				for (int z = 0; z < lattice.getHeight(); z++) {
					outfile << getSiteEnergy(Coords(x, y, z)) << "\n";
				}
			}
		}
		outfile.close();
	}

	void OSC_Sim::exportEnergies(std::string filename, bool charge) {
		ofstream outfile(filename);
		outfile << lattice.getLength() << endl;
		outfile << lattice.getWidth() << endl;
		outfile << lattice.getHeight() << endl;
		for (int x = 0; x < lattice.getLength(); x++) {
			for (int y = 0; y < lattice.getWidth(); y++) {
				for (int z = 0; z < lattice.getHeight(); z++) {
					if (getSiteType(Coords(x, y, z)) == 1) {
						// hole site energies
						if (charge) {
							outfile << params.Homo_donor + getSiteEnergy(Coords(x, y, z)) << "\n";
						}
						// electron site energies
						else {
							outfile << params.Lumo_donor + getSiteEnergy(Coords(x, y, z)) << "\n";
						}
					}
					else {
						// hole site energies
						if (charge) {
							outfile << params.Homo_acceptor + getSiteEnergy(Coords(x, y, z)) << "\n";
						}
						// electron site energies
						else {
							outfile << params.Lumo_acceptor + getSiteEnergy(Coords(x, y, z)) << "\n";
						}
					}
				}
			}
		}
		outfile.close();
	}

	Coords OSC_Sim::generateExciton() {
		// Determine coords
		Coords coords = calculateRandomExcitonCreationCoords();
		generateExciton(coords, true, 0);
		return coords;
	}

	void OSC_Sim::generateExciton(const KMC_Lattice::Coords& coords, const bool spin, int tag) {
		if (tag == 0) {
			tag = N_excitons_created + 1;
		}
		// Create the new exciton and add it to the simulation
		Exciton exciton_new(getTime(), tag, coords, spin);
		excitons.push_back(exciton_new);
		Object* object_ptr = &excitons.back();
		addObject(object_ptr);
		// Add placeholder events to the corresponding lists
		Simulation* sim_ptr = this;
		Exciton::Hop hop_event(sim_ptr);
		exciton_hop_events.push_back(hop_event);
		Exciton::Recombination recombination_event(sim_ptr);
		recombination_event.setObjectPtr(object_ptr);
		exciton_recombination_events.push_back(recombination_event);
		Exciton::Dissociation dissociation_event(sim_ptr);
		exciton_dissociation_events.push_back(dissociation_event);
		Exciton::Exciton_Annihilation exciton_exciton_annihilation_event(sim_ptr);
		exciton_exciton_annihilation_events.push_back(exciton_exciton_annihilation_event);
		Exciton::Polaron_Annihilation exciton_polaron_annihilation_event(sim_ptr);
		exciton_polaron_annihilation_events.push_back(exciton_polaron_annihilation_event);
		Exciton::Intersystem_Crossing intersystem_crossing_event(sim_ptr);
		intersystem_crossing_event.setObjectPtr(object_ptr);
		exciton_intersystem_crossing_events.push_back(intersystem_crossing_event);
		// Update exciton counters
		if (getSiteType(coords) == (short)1) {
			N_excitons_created_donor++;
		}
		else {
			N_excitons_created_acceptor++;
		}
		N_excitons_created++;
		N_excitons++;
		if (spin) {
			N_singlets++;
		}
		else {
			N_triplets++;
		}
		// Log event
		if (isLoggingEnabled()) {
			*Logfile << "Created exciton " << exciton_new.getTag() << " at site " << coords.x << "," << coords.y << "," << coords.z << "." << endl;
		}
	}

	void OSC_Sim::generateElectron(const Coords& coords, int tag = 0) {
		if (tag == 0) {
			tag = N_electrons_created + 1;
		}
		// Create the new electron and add it to the simulation
		Polaron electron_new(getTime(), tag, coords, false);
		electrons.push_back(electron_new);
		Object* object_ptr = &electrons.back();
		addObject(object_ptr);
		// Add placeholder events to the corresponding lists
		Simulation* sim_ptr = this;
		Polaron::Hop hop_event(sim_ptr);
		electron_hop_events.push_back(hop_event);
		Polaron::Recombination recombination_event(sim_ptr);
		polaron_recombination_events.push_back(recombination_event);
		Polaron::Extraction extraction_event(sim_ptr);
		extraction_event.setObjectPtr(object_ptr);
		electron_extraction_events.push_back(extraction_event);
		// Update exciton counters
		N_electrons_created++;
		N_electrons++;
		// Log event
		if (isLoggingEnabled()) {
			*Logfile << "Created electron " << electron_new.getTag() << " at site " << coords.x << "," << coords.y << "," << coords.z << "." << endl;
		}
		// Update transient data
		if (params.Enable_dynamics_test) {
			transient_electron_tags.push_back(electrons.back().getTag());
			transient_electron_energies_prev.push_back(0);
		}
	}

	void OSC_Sim::generateHole(const Coords& coords, int tag = 0) {
		if (tag == 0) {
			tag = N_holes_created + 1;
		}
		// Create the new hole and add it to the simulation
		Polaron hole_new(getTime(), tag, coords, true);
		holes.push_back(hole_new);
		Object* object_ptr = &holes.back();
		addObject(object_ptr);
		// Add placeholder events to the corresponding lists
		Simulation* sim_ptr = this;
		Polaron::Hop hop_event(sim_ptr);
		hole_hop_events.push_back(hop_event);
		Polaron::Extraction extraction_event(sim_ptr);
		extraction_event.setObjectPtr(object_ptr);
		hole_extraction_events.push_back(extraction_event);
		// Update exciton counters
		N_holes_created++;
		N_holes++;
		// Log event
		if (isLoggingEnabled()) {
			*Logfile << "Created hole " << hole_new.getTag() << " at site " << coords.x << "," << coords.y << "," << coords.z << "." << endl;
		}
		// Update transient data
		if (params.Enable_dynamics_test) {
			transient_hole_tags.push_back(holes.back().getTag());
			transient_hole_energies_prev.push_back(0);
		}
	}

	void OSC_Sim::generateDynamicsExcitons() {
		// Reassign site energies
		if (N_excitons_created > 0) {
			reassignSiteEnergies();
		}
		// Initialize transient data vectors
		transient_exciton_tags.assign(N_initial_excitons, -1);
		transient_exciton_energies_prev.assign(N_initial_excitons, 0);
		transient_electron_tags.clear();
		transient_electron_energies_prev.clear();
		transient_hole_tags.clear();
		transient_hole_energies_prev.clear();
		N_transient_cycles++;
		int num = 0;
		if (N_transient_cycles % 10 == 0 || N_transient_cycles == 1) {
			cout << getId() << ": Dynamics transient cycle " << N_transient_cycles << ": Generating " << N_initial_excitons << " initial excitons." << endl;
		}
		while (num < N_initial_excitons) {
			generateExciton();
			transient_exciton_tags[num] = excitons.back().getTag();
			transient_exciton_energies_prev[num] = getSiteEnergy(excitons.back().getCoords());
			num++;
		}
		Transient_creation_time = getTime();
		Transient_index_prev = -1;
		Transient_singlet_counts_prev = N_singlets;
		Transient_triplet_counts_prev = N_triplets;
		Transient_electron_counts_prev = N_electrons;
		Transient_hole_counts_prev = N_holes;
		calculateAllEvents();
	}

	void OSC_Sim::generateSteadyPolarons() {
		// Check to make sure there are enough possible sites
		int N_polarons = round_int(params.Steady_carrier_density*lattice.getVolume());
		cout << getId() << ": Creating " << N_polarons << " polarons for the steady charge transport simulation..." << endl;
		if (params.Enable_phase_restriction && N_donor_sites < N_polarons) {
			cout << "Error! " << N_polarons << " donor sites were not available to place the initial steady transport test hole polarons." << endl;
			setErrorMessage("Steady transport test hole polarons could not be created.");
			Error_found = true;
			return;
		}
		// If there is no disorder, generate the hole polarons on random sites
		// Randomly select from the total possible sites
		if (!params.Enable_gaussian_dos && !params.Enable_exponential_dos) {
			// Construct a vector of coordinates for all possible sites
			vector<Coords> coords_vec;
			if (params.Enable_phase_restriction) {
				coords_vec.reserve(N_donor_sites);
			}
			else {
				coords_vec.reserve(sites.size());
			}
			for (long int i = 0; i < (long int)sites.size(); i++) {
				auto coords = lattice.getSiteCoords(i);
				if (params.Enable_phase_restriction && getSiteType(coords) == 2) {
					continue;
				}
				coords_vec.push_back(coords);
			}
			shuffle(coords_vec.begin(), coords_vec.end(), generator);
			// Resize the vector to only keep the appropriate number of sites
			coords_vec.resize(N_polarons);
			for (auto& item : coords_vec) {
				generateHole(item);
			}
		}
		// If there is disorder, create polarons by filling up the DOS
		else {
			// Construct a vector of coordinates for all possible sites
			vector<Coords> site_data;
			if (params.Enable_phase_restriction) {
				site_data.reserve(N_donor_sites);
			}
			else {
				site_data.reserve(sites.size());
			}
			for (long int i = 0; i < (long int)sites.size(); i++) {
				auto coords = lattice.getSiteCoords(i);
				if (params.Enable_phase_restriction && getSiteType(coords) == 2) {
					continue;
				}
				site_data.push_back(coords);
			}
			// Sort subrange of coords vect based on the site energy at the coords
			partial_sort(site_data.begin(), site_data.begin() + N_polarons, site_data.end(), [this](const Coords& a, const Coords& b) -> bool {
				if (getSiteType(a) == getSiteType(b)) {
					return (getSiteEnergy(a) < getSiteEnergy(b));
				}
				else if (getSiteType(a) == 1) {
					return ((params.Homo_donor + getSiteEnergy(a)) < (params.Homo_acceptor + getSiteEnergy(b)));
				}
				else {
					return ((params.Homo_acceptor + getSiteEnergy(a)) < (params.Homo_donor + getSiteEnergy(b)));
				}
			});
			// Resize vector to only keep the lowest energy sites
			site_data.resize(N_polarons);
			// Generate the polarons in the selected sites
			for (auto const &item : site_data) {
				generateHole(item);
			}
		}
		// Calculate events for all polarons created
		calculateAllEvents();
	}

	void OSC_Sim::generateToFPolarons() {
		Coords coords;
		// Reassign site energies
		if (N_electrons_collected > 0 || N_holes_collected > 0) {
			reassignSiteEnergies();
		}
		// Create electrons at the top plane of the lattice
		if (!params.ToF_polaron_type) {
			coords.z = lattice.getHeight() - 1;
		}
		// Create holes at the bottom plane of the lattice
		else {
			coords.z = 0;
		}
		// Initialize transient data vectors
		if (!params.ToF_polaron_type) {
			transient_electron_tags.assign(params.ToF_initial_polarons, -1);
			transient_electron_energies_prev.assign(params.ToF_initial_polarons, 0);
			Transient_electron_counts_prev = params.ToF_initial_polarons;
		}
		else {
			transient_hole_tags.assign(params.ToF_initial_polarons, -1);
			transient_hole_energies_prev.assign(params.ToF_initial_polarons, 0);
			Transient_hole_counts_prev = params.ToF_initial_polarons;
		}
		ToF_positions_prev.assign(params.ToF_initial_polarons, coords.z);
		Transient_creation_time = getTime();
		Transient_index_prev = -1;
		N_transient_cycles++;
		if (N_transient_cycles % 10 == 0) {
			cout << getId() << ": ToF transient cycle " << N_transient_cycles << ": Generating " << params.ToF_initial_polarons << " initial polarons." << endl;
		}
		int num = 0;
		// Determine unique coords for each new polaron
		vector<Coords> coords_vect(0);
		// Construct vector of coordinates for all possible sites
		for (int x = 0; x < lattice.getLength(); x++) {
			for (int y = 0; y < lattice.getWidth(); y++) {
				coords.x = x;
				coords.y = y;
				// If phase restriction is enabled, electrons cannot be created on donor sites
				if (params.Enable_phase_restriction && !params.ToF_polaron_type && getSiteType(coords) == (short)1) {
					continue;
				}
				// If phase restriction is enabled, holes cannot be created on acceptor sites
				if (params.Enable_phase_restriction && params.ToF_polaron_type && getSiteType(coords) == (short)2) {
					continue;
				}
				coords_vect.push_back(coords);
			}
		}
		// Check to make sure there are enough possible sites
		if ((int)coords_vect.size() < params.ToF_initial_polarons) {
			cout << "Error! " << params.ToF_initial_polarons << " sites were not available to place the initial ToF polarons." << endl;
			setErrorMessage("Initial ToF polarons could not be created.");
			Error_found = true;
			return;
		}
		// Randomly select from the total possible sites
		if (params.Enable_ToF_random_placement) {
			shuffle(coords_vect.begin(), coords_vect.end(), generator);
			// Resize the vector to only keep the appropriate number of sites
			coords_vect.resize(params.ToF_initial_polarons);
		}
		// Only select the lowest energy sites
		else if (params.Enable_ToF_energy_placement) {
			// Sort vector by site energy from closest to the target energy to furthest
			sort(coords_vect.begin(), coords_vect.end(), [this](const Coords& a, const Coords& b) -> bool {
				return fabs(getSiteEnergy(a) - params.ToF_placement_energy) < fabs(getSiteEnergy(b) - params.ToF_placement_energy);
			});
			// Resize vector to only keep the lowest energy sites
			coords_vect.resize(params.ToF_initial_polarons);
		}
		// Generate the polarons in the selected sites
		num = 0;
		for (auto const &item : coords_vect) {
			if (!params.ToF_polaron_type) {
				generateElectron(item);
				transient_electron_tags[num] = electrons.back().getTag();
				transient_electron_energies_prev[num] = getSiteEnergy(item);
			}
			else {
				generateHole(item);
				transient_hole_tags[num] = holes.back().getTag();
				transient_hole_energies_prev[num] = getSiteEnergy(item);
			}
			num++;
		}
		calculateAllEvents();
	}

	vector<pair<double, double>> OSC_Sim::getDOSCorrelationData() const {
		return DOS_correlation_data;
	}

	vector<double> OSC_Sim::getDynamicsExcitonMSDV() const {
		return transient_exciton_msdv;
	}

	vector<double> OSC_Sim::getDynamicsElectronMSDV() const {
		return transient_electron_msdv;
	}

	vector<double> OSC_Sim::getDynamicsHoleMSDV() const {
		return transient_hole_msdv;
	}

	vector<double> OSC_Sim::getDynamicsExcitonEnergies() const {
		return transient_exciton_energies;
	}

	vector<double> OSC_Sim::getDynamicsElectronEnergies() const {
		return transient_electron_energies;
	}

	vector<double> OSC_Sim::getDynamicsHoleEnergies() const {
		return transient_hole_energies;
	}

	vector<int> OSC_Sim::getDynamicsTransientSinglets() const {
		return transient_singlet_counts;
	}

	vector<int> OSC_Sim::getDynamicsTransientTriplets() const {
		return transient_triplet_counts;
	}

	vector<int> OSC_Sim::getDynamicsTransientElectrons() const {
		return transient_electron_counts;
	}

	vector<int> OSC_Sim::getDynamicsTransientHoles() const {
		return transient_hole_counts;
	}

	vector<double> OSC_Sim::getDynamicsTransientTimes() const {
		return transient_times;
	}

	vector<double> OSC_Sim::getExcitonDiffusionData() const {
		return exciton_diffusion_distances;
	}

	vector<int> OSC_Sim::getExcitonHopLengthData() const {
		return exciton_hop_distances;
	}

	vector<double> OSC_Sim::getExcitonLifetimeData() const {
		return exciton_lifetimes;
	}

	list<Exciton>::iterator OSC_Sim::getExcitonIt(const Object* object_ptr) {
		auto it = find_if(excitons.begin(), excitons.end(), [object_ptr](Exciton& a) {return (a.getTag() == object_ptr->getTag()); });
		if (it == excitons.end()) {
			cout << "Error! Exciton iterator could not be located." << endl;
			setErrorMessage("Exciton iterator could not be located.");
			Error_found = true;
		}
		return it;
	}

	double OSC_Sim::getInternalField() const {
		return params.Internal_potential / (1e-7*lattice.getHeight()*lattice.getUnitSize());
	}

	int OSC_Sim::getN_bimolecular_recombinations() const {
		return N_bimolecular_recombinations;
	}

	int OSC_Sim::getN_electrons_collected() const {
		return N_electrons_collected;
	}

	int OSC_Sim::getN_electrons_created() const {
		return N_electrons_created;
	}

	int OSC_Sim::getN_electrons_recombined() const {
		return N_electrons_recombined;
	}

	long int OSC_Sim::getN_events_executed() const {
		return N_events_executed;
	}

	int OSC_Sim::getN_excitons_created() const {
		return N_excitons_created;
	}

	int OSC_Sim::getN_excitons_created(const short site_type) const {
		if (site_type == (short)1) {
			return N_excitons_created_donor;
		}
		else {
			return N_excitons_created_acceptor;
		}
	}

	int OSC_Sim::getN_singlet_excitons_dissociated() const {
		return N_singlet_excitons_dissociated;
	}

	int OSC_Sim::getN_triplet_excitons_dissociated() const {
		return N_triplet_excitons_dissociated;
	}

	int OSC_Sim::getN_singlet_excitons_recombined() const {
		return N_singlet_excitons_recombined;
	}

	int OSC_Sim::getN_triplet_excitons_recombined() const {
		return N_triplet_excitons_recombined;
	}

	int OSC_Sim::getN_singlet_singlet_annihilations() const {
		return N_singlet_singlet_annihilations;
	}

	int OSC_Sim::getN_singlet_triplet_annihilations() const {
		return N_singlet_triplet_annihilations;
	}

	int OSC_Sim::getN_triplet_triplet_annihilations() const {
		return N_triplet_triplet_annihilations;
	}

	int OSC_Sim::getN_singlet_polaron_annihilations() const {
		return N_singlet_polaron_annihilations;
	}

	int OSC_Sim::getN_triplet_polaron_annihilations() const {
		return N_triplet_polaron_annihilations;
	}

	int OSC_Sim::getN_geminate_recombinations() const {
		return N_geminate_recombinations;
	}

	int OSC_Sim::getN_holes_collected() const {
		return N_holes_collected;
	}

	int OSC_Sim::getN_holes_created() const {
		return N_holes_created;
	}

	int OSC_Sim::getN_holes_recombined() const {
		return N_holes_recombined;
	}

	int OSC_Sim::getN_transient_cycles() const {
		return N_transient_cycles;
	}

	list<Polaron>::iterator OSC_Sim::getPolaronIt(const Object* object_ptr) {
		list<Polaron>::iterator it;
		if (object_ptr->getObjectType().compare(Polaron::object_type) == 0) {
			// electrons
			if (!(static_cast<const Polaron*>(object_ptr)->getCharge())) {
				it = find_if(electrons.begin(), electrons.end(), [object_ptr](Polaron& a) {
					return (a.getTag() == object_ptr->getTag());
				});
				if (it == electrons.end()) {
					cout << "Error! Polaron iterator could not be located." << endl;
					setErrorMessage("Polaron iterator could not be located.");
					Error_found = true;
				}
			}
			// holes
			else {
				it = find_if(holes.begin(), holes.end(), [object_ptr](Polaron& a) {
					return (a.getTag() == object_ptr->getTag());
				});
				if (it == holes.end()) {
					cout << "Error! Polaron iterator could not be located." << endl;
					setErrorMessage("Polaron iterator could not be located.");
					Error_found = true;
				}
			}
		}
		return it;
	}

	std::string OSC_Sim::getPreviousEventType() const {
		return previous_event_type;
	}

	vector<float> OSC_Sim::getSiteEnergies(const short site_type) const {
		vector<float> energies;
		for (int i = 0; i < lattice.getNumSites(); i++) {
			if (sites[i].getType() == site_type) {
				energies.push_back(sites[i].getEnergy());
			}
		}
		return energies;
	}

	float OSC_Sim::getSiteEnergy(const Coords& coords) {
		// Check that coords are valid
		try {
			return sites[lattice.getSiteIndex(coords)].getEnergy();
		}
		catch (out_of_range exception) {
			cout << "Error! Site energy cannot be retrieved because the input coordinates are invalid." << endl;
			setErrorMessage("Site energy cannot be retrieved because the input coordinates are invalid.");
			Error_found = true;
			return NAN;
		}
	}

	short OSC_Sim::getSiteType(const Coords& coords) {
		// Check that coords are valid
		try {
			return sites[lattice.getSiteIndex(coords)].getType();
		}
		catch (out_of_range exception) {
			cout << "Error! Site type cannot be retrieved because the input coordinates are invalid." << endl;
			setErrorMessage("Site type cannot be retrieved because the input coordinates are invalid.");
			Error_found = true;
			return -1;
		}
	}

	vector<string> OSC_Sim::getChargeExtractionMap(const bool charge) const {
		vector<string> output_data(lattice.getLength()*lattice.getWidth() + 1);
		int x, y;
		output_data[0] = "X-Position,Y-Position,Extraction Probability";
		if (!charge) {
			for (int i = 0; i < (int)electron_extraction_data.size(); i++) {
				x = i / lattice.getWidth();
				y = i % lattice.getWidth();
				if (N_electrons_collected > 0) {
					output_data[i + 1] = to_string(x) + "," + to_string(y) + "," + to_string((double)electron_extraction_data[i] / (double)(N_electrons_collected));
				}
				else {
					output_data[i + 1] = to_string(x) + "," + to_string(y) + ",0";
				}
			}
		}
		else {
			for (int i = 0; i < (int)hole_extraction_data.size(); i++) {
				x = i / lattice.getWidth();
				y = i % lattice.getWidth();
				if (N_holes_collected > 0) {
					output_data[i + 1] = to_string(x) + "," + to_string(y) + "," + to_string((double)hole_extraction_data[i] / (double)(N_holes_collected));
				}
				else {
					output_data[i + 1] = to_string(x) + "," + to_string(y) + ",0";
				}
			}
		}
		return output_data;
	}

	double OSC_Sim::getSteadyCurrentDensity() const {
		double average_displacement = 0.0;
		for (auto const &item : holes) {
			average_displacement += item.calculateDisplacement(3);
		}
		average_displacement *= (lattice.getUnitSize()*1e-7 / holes.size());
		return 1000 * Elementary_charge*(abs(average_displacement) / (getTime() - Steady_equilibration_time)) * (N_holes / getVolume());
	}

	vector<std::pair<double, double>> OSC_Sim::getSteadyDOOS() const {
		auto hist = steady_DOOS;
		// Normalize histogram counts to produce density
		for (auto& item : hist) {
			item.second /= Steady_DOOS_sampling_counter * lattice.getVolume()*DOS_bin_size;
		}
		return hist;
	}

	vector<std::pair<double, double>> OSC_Sim::getSteadyDOOS_Coulomb() const {
		auto hist = steady_DOOS_Coulomb;
		// Normalize histogram counts to produce density
		for (auto& item : hist) {
			item.second /= Steady_DOOS_sampling_counter * lattice.getVolume()*DOS_bin_size;
		}
		return hist;
	}

	vector<std::pair<double, double>> OSC_Sim::getSteadyDOS() {
		steady_DOS.clear();
		for (long int i = 0; i < lattice.getNumSites(); i++) {
			double energy;
			auto site_coords = lattice.getSiteCoords(i);
			if (lattice.isOccupied(site_coords)) {
				if (getSiteType(site_coords) == 1) {
					energy = params.Homo_donor + getSiteEnergy(site_coords);
				}
				else {
					energy = params.Homo_acceptor + getSiteEnergy(site_coords);
				}
			}
			else {
				if (getSiteType(site_coords) == 1) {
					energy = params.Homo_donor + getSiteEnergy(site_coords);
				}
				else {
					energy = params.Homo_acceptor + getSiteEnergy(site_coords);
				}
			}
			updateSteadyDOS(steady_DOS, energy);
		}
		auto hist = steady_DOS;
		// Normalize histogram counts to produce density
		for (auto& item : hist) {
			item.second /= lattice.getVolume()*DOS_bin_size;
		}
		return hist;
	}

	vector<std::pair<double, double>> OSC_Sim::getSteadyDOS_Coulomb() const {
		auto hist = steady_DOS_Coulomb;
		// Normalize histogram counts to produce density
		for (auto& item : hist) {
			item.second /= Steady_DOS_sampling_counter * lattice.getVolume()*DOS_bin_size;
		}
		return hist;
	}

	double OSC_Sim::getSteadyEquilibrationEnergy() const {
		if ((int)holes.size() > 0) {
			return Steady_equilibration_energy_sum / (double)(holes.size()*((params.N_tests / Steady_hops_per_DOOS_sample) + 1));
		}
		else {
			return NAN;
		}
	}

	double OSC_Sim::getSteadyEquilibrationEnergy_Coulomb() const {
		if ((int)holes.size() > 0) {
			return Steady_equilibration_energy_sum_Coulomb / (double)(holes.size()*((params.N_tests / Steady_hops_per_DOOS_sample) + 1));
		}
		else {
			return NAN;
		}
	}

	double OSC_Sim::getSteadyMobility() const {
		double average_displacement = 0.0;
		for (auto const &item : holes) {
			average_displacement += item.calculateDisplacement(3);
		}
		average_displacement *= (lattice.getUnitSize()*1e-7 / holes.size());
		return abs(average_displacement) / ((getTime() - Steady_equilibration_time) * abs(getInternalField()));
	}

	double OSC_Sim::getSteadyTransportEnergy() const {
		if (abs(Transport_energy_sum_of_weights) > 0) {
			return (Transport_energy_weighted_sum / Transport_energy_sum_of_weights);
		}
		else {
			return NAN;
		}
	}

	double OSC_Sim::getSteadyTransportEnergy_Coulomb() const {
		if (abs(Transport_energy_sum_of_weights) > 0) {
			return (Transport_energy_weighted_sum_Coulomb / Transport_energy_sum_of_weights);
		}
		else {
			return NAN;
		}
	}

	vector<int> OSC_Sim::getToFTransientCounts() const {
		if (!params.ToF_polaron_type) {
			return transient_electron_counts;
		}
		else {
			return transient_hole_counts;
		}
	}

	vector<double> OSC_Sim::getToFTransientEnergies() const {
		if (!params.ToF_polaron_type) {
			return transient_electron_energies;
		}
		else {
			return transient_hole_energies;
		}
	}

	vector<double> OSC_Sim::getToFTransientTimes() const {
		return transient_times;
	}

	vector<double> OSC_Sim::getToFTransientVelocities() const {
		return transient_velocities;
	}

	vector<double> OSC_Sim::getTransitTimeData() const {
		return transit_times;
	}

	bool OSC_Sim::initializeArchitecture() {
		bool success;
		N_donor_sites = 0;
		N_acceptor_sites = 0;
		if (params.Enable_neat) {
			N_donor_sites = lattice.getNumSites();
			N_acceptor_sites = 0;
			for (auto &item : sites) {
				item.setType(1);
			}
		}
		else if (params.Enable_bilayer) {
			Coords coords;
			for (int x = 0; x < lattice.getLength(); x++) {
				for (int y = 0; y < lattice.getWidth(); y++) {
					for (int z = 0; z < lattice.getHeight(); z++) {
						coords.setXYZ(x, y, z);
						if (z < params.Thickness_acceptor) {
							sites[lattice.getSiteIndex(coords)].setType(2);
							N_acceptor_sites++;
						}
						else {
							sites[lattice.getSiteIndex(coords)].setType(1);
							N_donor_sites++;
						}
					}
				}
			}
		}
		else if (params.Enable_random_blend) {
			vector<short> site_types;
			site_types.assign(lattice.getNumSites(), 1);
			for (int i = 0; i < (int)lattice.getNumSites()*params.Acceptor_conc; i++) {
				site_types[i] = 2;
				N_acceptor_sites++;
			}
			N_donor_sites = lattice.getNumSites() - N_acceptor_sites;
			shuffle(site_types.begin(), site_types.end(), generator);
			for (int i = 0; i < (int)sites.size(); i++) {
				sites[i].setType(site_types[i]);
			}
		}
		else if (params.Enable_import_morphology) {
			success = createImportedMorphology();
			if (!success) {
				return false;
			}
		}
		// Send the site pointers to the Lattice object
		vector<Site*> site_ptrs((int)sites.size());
		for (int i = 0; i < (int)sites.size(); i++) {
			site_ptrs[i] = &sites[i];
		}
		lattice.setSitePointers(site_ptrs);
		return true;
	}

	void OSC_Sim::outputStatus() {
		if (params.Enable_ToF_test) {
			cout << getId() << ": Time = " << getTime() << " seconds.\n";
			if (!params.ToF_polaron_type) {
				cout << getId() << ": " << N_electrons_collected << " out of " << N_electrons_created << " electrons have been collected and " << N_events_executed << " events have been executed.\n";
				cout << getId() << ": There are currently " << N_electrons << " electrons in the lattice:\n";
				for (auto const &item : electrons) {
					cout << getId() << ": Electron " << item.getTag() << " is at " << item.getCoords().x << "," << item.getCoords().y << "," << item.getCoords().z << ".\n";
				}
			}
			else {
				cout << getId() << ": " << N_holes_collected << " out of " << N_holes_created << " holes have been collected and " << N_events_executed << " events have been executed.\n";
				cout << getId() << ": There are currently " << N_holes << " holes in the lattice:\n";
				for (auto const &item : holes) {
					cout << getId() << ": Hole " << item.getTag() << " is at " << item.getCoords().x << "," << item.getCoords().y << "," << item.getCoords().z << ".\n";
				}
			}
		}
		if (params.Enable_exciton_diffusion_test) {
			cout << getId() << ": Time = " << getTime() << " seconds.\n";
			cout << getId() << ": " << N_excitons_created << " excitons have been created and " << N_events_executed << " events have been executed.\n";
			cout << getId() << ": There are currently " << N_excitons << " excitons in the lattice:\n";
			for (auto const &item : excitons) {
				cout << getId() << ": Exciton " << item.getTag() << " is at " << item.getCoords().x << "," << item.getCoords().y << "," << item.getCoords().z << ".\n";
			}
		}
		if (params.Enable_IQE_test || params.Enable_dynamics_test) {
			cout << getId() << ": Time = " << getTime() << " seconds.\n";
			cout << getId() << ": " << N_excitons_created << " excitons have been created and " << N_events_executed << " events have been executed.\n";
			cout << getId() << ": There are currently " << N_excitons << " excitons in the lattice:\n";
			for (auto const &item : excitons) {
				cout << getId() << ": Exciton " << item.getTag() << " is at " << item.getCoords().x << "," << item.getCoords().y << "," << item.getCoords().z << ".\n";
			}
			cout << getId() << ": There are currently " << N_electrons << " electrons in the lattice:\n";
			for (auto const &item : electrons) {
				cout << getId() << ": Electron " << item.getTag() << " is at " << item.getCoords().x << "," << item.getCoords().y << "," << item.getCoords().z << ".\n";
			}
			cout << getId() << ": There are currently " << N_holes << " holes in the lattice:\n";
			for (auto const &item : holes) {
				cout << getId() << ": Hole " << item.getTag() << " is at " << item.getCoords().x << "," << item.getCoords().y << "," << item.getCoords().z << ".\n";
			}
		}
		if (params.Enable_steady_transport_test) {
			if (N_events_executed <= params.N_equilibration_events) {
				cout << getId() << ": Time = " << getTime() << " seconds.\n";
				cout << getId() << ": " << N_events_executed << " of " << params.N_equilibration_events << " equilibration events have been executed.\n";
			}
			else if (N_events_executed > params.N_equilibration_events) {
				cout << getId() << ": Time = " << getTime() << " seconds.\n";
				cout << getId() << ": " << (N_events_executed - params.N_equilibration_events) << " of " << params.N_tests << " test events have been executed.\n";
			}
		}
		cout.flush();
	}

	void OSC_Sim::reassignSiteEnergies() {
		vector<float> site_energies_donor;
		vector<float> site_energies_acceptor;
		if (params.Enable_gaussian_dos) {
			site_energies_donor.assign(N_donor_sites, 0);
			site_energies_acceptor.assign(N_acceptor_sites, 0);
			createGaussianDOSVector(site_energies_donor, 0, params.Energy_stdev_donor, generator);
			createGaussianDOSVector(site_energies_acceptor, 0, params.Energy_stdev_acceptor, generator);
		}
		else if (params.Enable_exponential_dos) {
			site_energies_donor.assign(N_donor_sites, 0);
			site_energies_acceptor.assign(N_acceptor_sites, 0);
			createExponentialDOSVector(site_energies_donor, 0, params.Energy_urbach_donor, generator);
			createExponentialDOSVector(site_energies_acceptor, 0, params.Energy_urbach_acceptor, generator);
		}
		int donor_count = 0;
		int acceptor_count = 0;
		for (auto& site : sites) {
			if (params.Enable_gaussian_dos || params.Enable_exponential_dos) {
				if (site.getType() == (short)1) {
					site.setEnergy(*(site_energies_donor.begin() + donor_count));
					donor_count++;
				}
				else if (site.getType() == (short)2) {
					site.setEnergy(*(site_energies_acceptor.begin() + acceptor_count));
					acceptor_count++;
				}
				else {
					cout << getId() << ": Error! Undefined site type detected while assigning site energies." << endl;
					setErrorMessage("Undefined site type detected while assigning site energies.");
					Error_found = true;
					return;
				}
			}
			else {
				if (site.getType() == (short)1) {
					site.setEnergy(0.0);
				}
				else if (site.getType() == (short)2) {
					site.setEnergy(0.0);
				}
				else {
					cout << getId() << ": Error! Undefined site type detected while assigning site energies." << endl;
					setErrorMessage("Undefined site type detected while assigning site energies.");
					Error_found = true;
					return;
				}
			}
		}
		if (params.Enable_correlated_disorder) {
			createCorrelatedDOS(params.Disorder_correlation_length);
		}
		if (params.Enable_interfacial_energy_shift) {
			for (int n = 0; n < lattice.getNumSites(); n++) {
				Coords coords_i = lattice.getSiteCoords(n);
				int counts_first = 0;
				int counts_second = 0;
				int counts_third = 0;
				for (int i = -1; i <= 1; i++) {
					for (int j = -1; j <= 1; j++) {
						for (int k = -1; k <= 1; k++) {
							if (lattice.checkMoveValidity(coords_i, i, j, k)) {
								Coords coords_f;
								lattice.calculateDestinationCoords(coords_i, i, j, k, coords_f);
								if (getSiteType(coords_i) != getSiteType(coords_f)) {
									switch (abs(i) + abs(j) + abs(k)) {
									case 1:
										counts_first++;
										break;
									case 2:
										counts_second++;
										break;
									case 3:
										counts_third++;
										break;
									default:
										break;
									}
								}
							}
						}
					}
				}
				if (counts_first > 0 || counts_second > 0 || counts_third > 0) {
					float energy_new = 0;
					if (sites[n].getType() == (short)1) {
						if (!params.Enable_gaussian_dos && !params.Enable_exponential_dos) {
							energy_new = (counts_first * (float)params.Energy_shift_donor) + (counts_second * (float)params.Energy_shift_donor / sqrt(2.0f)) + (counts_third * (float)params.Energy_shift_donor / sqrt(3.0f));
							sites[n].setEnergy(energy_new);
						}
						else {
							energy_new = sites[n].getEnergy() + (counts_first * (float)params.Energy_shift_donor) + (counts_second * (float)params.Energy_shift_donor / sqrt(2.0f)) + (counts_third * (float)params.Energy_shift_donor / sqrt(3.0f));
							sites[n].setEnergy(energy_new);
						}

					}
					else if (sites[n].getType() == (short)2) {
						if (!params.Enable_gaussian_dos && !params.Enable_exponential_dos) {
							energy_new = (counts_first * (float)params.Energy_shift_acceptor) + (counts_second * (float)params.Energy_shift_acceptor / sqrt(2.0f)) + (counts_third * (float)params.Energy_shift_acceptor / sqrt(3.0f));
							sites[n].setEnergy(energy_new);
						}
						else {
							energy_new = sites[n].getEnergy() + (counts_first * (float)params.Energy_shift_acceptor) + (counts_second * (float)params.Energy_shift_acceptor / sqrt(2.0f)) + (counts_third * (float)params.Energy_shift_acceptor / sqrt(3.0f));
							sites[n].setEnergy(energy_new);
						}
					}
					else {
						cout << getId() << ": Error! Undefined site type detected while assigning site energies." << endl;
						setErrorMessage("Undefined site type detected while assigning site energies.");
						Error_found = true;
						return;
					}
				}
			}
		}
		if (params.Enable_import_energies) {
			ifstream infile(params.Energies_import_filename);
			// Check if energies file exists and is accessible
			if (!infile.good()) {
				cout << getId() << ": Error opening site energies file for importing." << endl;
				setErrorMessage("Site energies file could not be opened for importing.");
				Error_found = true;
				infile.close();
				return;
			}
			// Copy all fine lines into a string vector
			string line;
			vector<string> lines;
			while (getline(infile, line)) {
				lines.push_back(line);
			}
			// Check that the file contains at least 3 lines for the lattice dimensions
			int length = -1;
			int width = -1;
			int height = -1;
			if (lines.size() > 3) {
				// Read in the lattice dimensions
				length = stoi(lines[0]);
				width = stoi(lines[1]);
				height = stoi(lines[2]);
			}
			// Check that valid lattice dimensions were read from the file
			if (length <= 0 || width <= 0 || height <= 0) {
				cout << getId() << ": Error importing the site energies, lattice dimensions imported from file are not valid." << endl;
				setErrorMessage("Error importing the site energies, lattice dimensions imported from file are not valid.");
				Error_found = true;
				return;
			}
			// Check that the lattice dimensions in the file match the size of the lattice
			if (length != lattice.getLength() || width != lattice.getWidth() || height != lattice.getHeight()) {
				cout << getId() << ": Error importing the site energies, dimensions in file do not match the lattice dimensions." << endl;
				setErrorMessage("Error importing the site energies, dimensions in file do not match the lattice dimensions.");
				Error_found = true;
				return;
			}
			// Check that the number of read lines corresponds to the correct number of lattice sites
			if ((int)lines.size() != (length*width*height + 3)) {
				cout << getId() << ": Error importing the site energies, the number of energies does not equal the number of sites." << endl;
				setErrorMessage("Error importing the site energies file,  the number of energies does not equal the number of sites.");
				Error_found = true;
				return;
			}
			int i = 3;
			for (int x = 0; x < length; x++) {
				for (int y = 0; y < width; y++) {
					for (int z = 0; z < height; z++) {
						float energy = stof(lines[i]);
						long int index = lattice.getSiteIndex(Coords(x, y, z));
						if (sites[index].getType() == (short)1) {
							sites[index].setEnergy(energy);
						}
						else if (sites[index].getType() == (short)2) {
							sites[index].setEnergy(energy);
						}
						else {
							cout << getId() << ": Error! Undefined site type detected while assigning site energies." << endl;
							setErrorMessage("Undefined site type detected while assigning site energies.");
							Error_found = true;
							return;
						}
						i++;
					}
				}
			}

		}
	}

	void OSC_Sim::removeExciton(list<Exciton>::iterator exciton_it) {
		// Output diffusion distance
		if (params.Enable_exciton_diffusion_test) {
			exciton_diffusion_distances.push_back(lattice.getUnitSize()*exciton_it->calculateDisplacement());
			exciton_lifetimes.push_back(getTime() - exciton_it->getCreationTime());
		}
		// Update exciton counters
		N_excitons--;
		if (exciton_it->getSpin()) {
			N_singlets--;
		}
		else {
			N_triplets--;
		}
		// Delete exciton
		deleteObject(&(*exciton_it));
	}

	bool OSC_Sim::siteContainsHole(const Coords& coords) {
		if (lattice.isOccupied(coords)) {
			auto object_ptr = (*lattice.getSiteIt(coords))->getObjectPtr();
			if (object_ptr->getObjectType().compare(Polaron::object_type) == 0) {
				return static_cast<Polaron*>(object_ptr)->getCharge();
			}
		}
		return false;
	}

	void OSC_Sim::updateSteadyData() {
		// Check if equilibration step is complete
		if (N_events_executed == params.N_equilibration_events) {
			// Mark time
			Steady_equilibration_time = getTime();
			// Reset displacement of polarons
			for (auto &item : holes) {
				item.resetInitialCoords(item.getCoords());
			}
			// Reset transport energy data
			Transport_energy_weighted_sum = 0.0;
			Transport_energy_weighted_sum_Coulomb = 0.0;
			Transport_energy_sum_of_weights = 0.0;
			// Output status
			cout << getId() << ": Equilibration phase complete." << endl;
		}
		if (N_events_executed >= params.N_equilibration_events) {
			// Sample the density of occupied states
			if ((N_events_executed - params.N_equilibration_events) % Steady_hops_per_DOOS_sample == 0) {
				for (const auto& item : holes) {
					double energy;
					double energy_C;
					auto site_coords = item.getCoords();
					auto it = getPolaronIt((*lattice.getSiteIt(site_coords))->getObjectPtr());
					if (getSiteType(site_coords) == 1) {
						energy = params.Homo_donor + getSiteEnergy(site_coords);
						energy_C = params.Homo_donor + getSiteEnergy(site_coords) + calculateCoulomb(it, site_coords);
					}
					else {
						energy = params.Homo_acceptor + getSiteEnergy(site_coords);
						energy_C = params.Homo_acceptor + getSiteEnergy(site_coords) + calculateCoulomb(it, site_coords);
					}
					updateSteadyDOS(steady_DOOS, energy);
					updateSteadyDOS(steady_DOOS_Coulomb, energy_C);
					Steady_equilibration_energy_sum += energy;
					Steady_equilibration_energy_sum_Coulomb += energy_C;
				}
				Steady_DOOS_sampling_counter++;
			}
			// Sample the density of states
			if ((N_events_executed - params.N_equilibration_events) % Steady_hops_per_DOS_sample == 0) {
				for (long int i = 0; i < lattice.getNumSites(); i++) {
					double energy_C;
					auto site_coords = lattice.getSiteCoords(i);
					if (lattice.isOccupied(site_coords)) {
						auto it = getPolaronIt((*lattice.getSiteIt(site_coords))->getObjectPtr());
						if (getSiteType(site_coords) == 1) {
							energy_C = params.Homo_donor + getSiteEnergy(site_coords) + calculateCoulomb(it, site_coords);
						}
						else {
							energy_C = params.Homo_acceptor + getSiteEnergy(site_coords) + calculateCoulomb(it, site_coords);
						}
					}
					else {
						if (getSiteType(site_coords) == 1) {
							energy_C = params.Homo_donor + getSiteEnergy(site_coords) + calculateCoulomb(true, site_coords);
						}
						else {
							energy_C = params.Homo_acceptor + getSiteEnergy(site_coords) + calculateCoulomb(true, site_coords);
						}
					}
					updateSteadyDOS(steady_DOS_Coulomb, energy_C);
				}
				Steady_DOS_sampling_counter++;
			}
		}
	}

	void OSC_Sim::updateSteadyDOS(vector<pair<double, double>>& density_of_states, double state_energy) {
		// Check if the density of states is empty
		if ((int)density_of_states.size() == 0) {
			// Round input state_energy to nearest bin value
			double new_bin = round_int(state_energy / DOS_bin_size)*DOS_bin_size;
			density_of_states.push_back(make_pair(new_bin, 1.0));
			return;
		}
		int new_bin_int = round_int(state_energy / DOS_bin_size);
		// Determine current range of bins
		int smallest_bin_int = round_int(density_of_states[0].first / DOS_bin_size);
		int largest_bin_int = round_int(density_of_states.back().first / DOS_bin_size);
		if (new_bin_int < smallest_bin_int) {
			// Insert as many bins as needed before the current bin to extend the range of the histogram to include the new state_energy
			vector<pair<double, double>> temp;
			for (int i = new_bin_int; i < smallest_bin_int; i++) {
				temp.push_back(make_pair(i*DOS_bin_size, 0.0));
			}
			// Add histogram count for the new state energy
			temp[0].second += 1.0;
			// Insert temp vector into the main density of states vector
			density_of_states.insert(density_of_states.begin(), temp.begin(), temp.end());
			return;
		}
		if (new_bin_int > largest_bin_int) {
			// Insert as many bins as needed after the current bin to extend the range of the histogram to include the new state_energy
			vector<pair<double, double>> temp;
			for (int i = largest_bin_int + 1; i <= new_bin_int; i++) {
				temp.push_back(make_pair(i*DOS_bin_size, 0.0));
			}
			// Add histogram count for the new state energy
			temp.back().second += 1.0;
			// Insert temp vector into the main density of states vector
			density_of_states.insert(density_of_states.end(), temp.begin(), temp.end());
			return;
		}
		// Calculate start of data range
		double min_val = smallest_bin_int * DOS_bin_size - 0.5*DOS_bin_size;
		// Add the state_energy to histogram
		int index = (int)floor((state_energy - min_val) / DOS_bin_size);
		density_of_states[index].second += 1.0;
		return;
	}

	void OSC_Sim::updateTransientData() {
		// ToF_positions_prev is a vector that stores the z-position of each charge carrier at the previous time interval
		// Transient_xxxx_energies_prev is a vector that stores the energies of each object at the previous time interval
		if (params.Enable_ToF_test) {
			// Check if enough time has passed since the previous time interval
			if ((getTime() - Transient_creation_time) > transient_times[Transient_index_prev + 1]) {
				int index = (int)floor((log10(getTime() - Transient_creation_time) - log10(Transient_start)) / Transient_step_size);
				if (index >= (int)transient_times.size()) {
					return;
				}
				// Update info for any previous timesteps that have elapsed already but were not accounted for
				while (index != 0 && Transient_index_prev < index - 1 && (Transient_index_prev + 1) < (int)transient_times.size()) {
					// electrons
					if (!params.ToF_polaron_type) {
						transient_electron_counts[Transient_index_prev + 1] += Transient_electron_counts_prev;
						for (auto const &item : electrons) {
							int electron_index = (int)distance(transient_electron_tags.begin(), find(transient_electron_tags.begin(), transient_electron_tags.end(), item.getTag()));
							// transient_velocities[index_prev+1] += 0;
							transient_electron_energies[Transient_index_prev + 1] += transient_electron_energies_prev[electron_index];
						}
					}
					// holes
					else {
						transient_hole_counts[Transient_index_prev + 1] += Transient_hole_counts_prev;
						for (auto const &item : holes) {
							int hole_index = (int)distance(transient_hole_tags.begin(), find(transient_hole_tags.begin(), transient_hole_tags.end(), item.getTag()));
							// transient_velocities[index_prev+1] += 0;
							transient_hole_energies[Transient_index_prev + 1] += transient_hole_energies_prev[hole_index];
						}
					}
					Transient_index_prev++;
				}
				// Update info for the current timestep
				// electrons
				if (!params.ToF_polaron_type) {
					transient_electron_counts[index] += N_electrons;
					Transient_electron_counts_prev = N_electrons;
					for (auto const &item : electrons) {
						// Get electron site energy and position for previous timestep
						int electron_index = (int)distance(transient_electron_tags.begin(), find(transient_electron_tags.begin(), transient_electron_tags.end(), item.getTag()));
						transient_velocities[index] += abs(1e-7*lattice.getUnitSize()*(item.getCoords().z - ToF_positions_prev[electron_index])) / ((getTime() - Transient_creation_time) - transient_times[Transient_index_prev]);
						transient_electron_energies[index] += getSiteEnergy(item.getCoords());
						transient_electron_energies_prev[electron_index] = getSiteEnergy(item.getCoords());
						ToF_positions_prev[electron_index] = item.getCoords().z;
					}
				}
				// holes
				else {
					transient_hole_counts[index] += N_holes;
					Transient_hole_counts_prev = N_holes;
					for (auto const &item : holes) {
						// Get hole site energy and position for previous timestep
						int hole_index = (int)distance(transient_hole_tags.begin(), find(transient_hole_tags.begin(), transient_hole_tags.end(), item.getTag()));
						transient_velocities[index] += abs(1e-7*lattice.getUnitSize()*(item.getCoords().z - ToF_positions_prev[hole_index])) / ((getTime() - Transient_creation_time) - transient_times[Transient_index_prev]);
						transient_hole_energies[index] += getSiteEnergy(item.getCoords());
						transient_hole_energies_prev[hole_index] = getSiteEnergy(item.getCoords());
						ToF_positions_prev[hole_index] = item.getCoords().z;
					}
				}
				Transient_index_prev = index;
			}
		}
		else if (params.Enable_dynamics_test) {
			// Calculate data for next time step if enough time has elapsed
			if ((getTime() - Transient_creation_time) > transient_times[Transient_index_prev + 1]) {
				int index = (int)floor((log10(getTime() - Transient_creation_time) - log10(Transient_start)) / Transient_step_size);
				if (index >= (int)transient_times.size()) {
					return;
				}
				// Update info for any previous timesteps that have elapsed already but were not accounted for
				while (index != 0 && Transient_index_prev < index - 1 && Transient_index_prev + 1 < (int)transient_times.size()) {
					transient_singlet_counts[Transient_index_prev + 1] += Transient_singlet_counts_prev;
					transient_triplet_counts[Transient_index_prev + 1] += Transient_triplet_counts_prev;
					transient_electron_counts[Transient_index_prev + 1] += Transient_electron_counts_prev;
					transient_hole_counts[Transient_index_prev + 1] += Transient_hole_counts_prev;
					for (auto const &item : excitons) {
						int exciton_index = (int)distance(transient_exciton_tags.begin(), find(transient_exciton_tags.begin(), transient_exciton_tags.end(), item.getTag()));
						transient_exciton_energies[Transient_index_prev + 1] += transient_exciton_energies_prev[exciton_index];
					}
					for (auto const &item : electrons) {
						int electron_index = (int)distance(transient_electron_tags.begin(), find(transient_electron_tags.begin(), transient_electron_tags.end(), item.getTag()));
						transient_electron_energies[Transient_index_prev + 1] += transient_electron_energies_prev[electron_index];
					}
					for (auto const &item : holes) {
						int hole_index = (int)distance(transient_hole_tags.begin(), find(transient_hole_tags.begin(), transient_hole_tags.end(), item.getTag()));
						transient_hole_energies[Transient_index_prev + 1] += transient_hole_energies_prev[hole_index];
					}
					Transient_index_prev++;
				}
				// Update info for the current timestep
				transient_singlet_counts[index] += N_singlets;
				transient_triplet_counts[index] += N_triplets;
				transient_electron_counts[index] += N_electrons;
				transient_hole_counts[index] += N_holes;
				Transient_singlet_counts_prev = N_singlets;
				Transient_triplet_counts_prev = N_triplets;
				Transient_electron_counts_prev = N_electrons;
				Transient_hole_counts_prev = N_holes;
				for (auto &item : excitons) {
					// Get polaron site energy and position for previous timestep
					int exciton_index = (int)distance(transient_exciton_tags.begin(), find(transient_exciton_tags.begin(), transient_exciton_tags.end(), item.getTag()));
					transient_exciton_msdv[index] += intpow(1e-7*lattice.getUnitSize()*item.calculateDisplacement(), 2) / ((getTime() - Transient_creation_time) - transient_times[Transient_index_prev]);
					item.resetInitialCoords(item.getCoords());
					transient_exciton_energies[index] += getSiteEnergy(item.getCoords());
					transient_exciton_energies_prev[exciton_index] = getSiteEnergy(item.getCoords());
				}
				for (auto &item : electrons) {
					// Get polaron site energy and position for previous timestep
					int electron_index = (int)distance(transient_electron_tags.begin(), find(transient_electron_tags.begin(), transient_electron_tags.end(), item.getTag()));
					transient_electron_msdv[index] += intpow(1e-7*lattice.getUnitSize()*item.calculateDisplacement(), 2) / ((getTime() - Transient_creation_time) - transient_times[Transient_index_prev]);
					item.resetInitialCoords(item.getCoords());
					transient_electron_energies[index] += getSiteEnergy(item.getCoords());
					transient_electron_energies_prev[electron_index] = getSiteEnergy(item.getCoords());
				}
				for (auto &item : holes) {
					// Get polaron site energy and position for previous timestep
					int hole_index = (int)distance(transient_hole_tags.begin(), find(transient_hole_tags.begin(), transient_hole_tags.end(), item.getTag()));
					transient_hole_msdv[index] += intpow(1e-7*lattice.getUnitSize()*item.calculateDisplacement(), 2) / ((getTime() - Transient_creation_time) - transient_times[Transient_index_prev]);
					item.resetInitialCoords(item.getCoords());
					transient_hole_energies[index] += getSiteEnergy(item.getCoords());
					transient_hole_energies_prev[hole_index] = getSiteEnergy(item.getCoords());
				}
				Transient_index_prev = index;
			}
		}

	}
}
