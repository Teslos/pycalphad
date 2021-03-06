/*=============================================================================
 Copyright (c) 2012-2014 Richard Otis
 
 Distributed under the Boost Software License, Version 1.0. (See accompanying
 file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 =============================================================================*/

// Declarations for global minimization of a thermodynamic potential

#ifndef INCLUDED_GLOBAL_MINIMIZATION
#define INCLUDED_GLOBAL_MINIMIZATION

#include "libgibbs/include/compositionset.hpp"
#include "libgibbs/include/models.hpp"
#include "libgibbs/include/optimizer/utils/ezd_minimization.hpp"
#include "libgibbs/include/optimizer/utils/hull_mapping.hpp"
#include "libgibbs/include/optimizer/utils/convex_hull.hpp"
#include "libgibbs/include/utils/for_each_pair.hpp"
#include "libgibbs/include/utils/site_fraction_convert.hpp"
#include "libtdb/include/logging.hpp"
#include <boost/assert.hpp>
#include <boost/noncopyable.hpp>
#include <boost/concept_check.hpp>
#include <functional>
#include <list>
#include <limits>
#include <set>

namespace Optimizer {

/* GlobalMinimizer performs global minimization of the specified
 * thermodynamic potential. Energy manifolds are calculated for
 * all phases in the global composition space and each phase's
 * internal degrees of freedom. Constraints can be added incrementally
 * to identify the equilibrium tie hyperplane and fix a position in it.
 */
template <
typename FacetType,
typename CoordinateType = double, 
typename EnergyType = CoordinateType
>
class GlobalMinimizer {
public:
    typedef details::ConvexHullMap<CoordinateType,EnergyType> HullMapType;
    typedef FacetType HullFacetType;
protected:
    HullMapType hull_map;
    std::vector<FacetType> candidate_facets;
    mutable logger class_log;
    double critical_edge_length; // minimum length of a tie line
    std::size_t initial_subdivisions_per_axis; // initial discretization to find spinodals
    std::size_t refinement_subdivisions_per_axis; // during mesh refinement
    std::size_t max_search_depth; // maximum recursive depth
    bool discard_unstable; // when sampling points, discard unstable ones before refinement
public:
    typedef typename HullMapType::PointType PointType;
    typedef typename HullMapType::GlobalPointType GlobalPointType;
    
    GlobalMinimizer() {
        critical_edge_length = 0.05;
        initial_subdivisions_per_axis = 20;
        refinement_subdivisions_per_axis = 2;
        max_search_depth = 5;
        discard_unstable = true;
    }

    virtual std::vector<PointType> point_sample(
        CompositionSet const& cmp,
        sublattice_set const& sublset,
        evalconditions const& conditions
        ) {
        BOOST_ASSERT(initial_subdivisions_per_axis>0);
        // Use adaptive simplex subdivision to sample the space
        return details::AdaptiveSimplexSample(cmp, sublset, conditions, initial_subdivisions_per_axis, refinement_subdivisions_per_axis, discard_unstable);
    };
    virtual std::vector<PointType> internal_hull(
        CompositionSet const& cmp,
        std::vector<PointType> const& points,
        std::set<std::size_t> const& dependent_dimensions,
        evalconditions const& conditions
    ) {
        BOOST_ASSERT(critical_edge_length>0);
        // Create a callback function for energy calculation for this phase
        auto calculate_energy = [&cmp,&conditions] (const PointType& point) {
            return cmp.evaluate_objective(conditions,cmp.get_variable_map(),const_cast<EnergyType*>(&point[0]));
        };
        // Use qhull to get the hull
        return details::internal_lower_convex_hull( points, 
                                                    dependent_dimensions, 
                                                    critical_edge_length, 
                                                    calculate_energy 
                                                  );
    };
    virtual std::vector<FacetType> global_hull(
        std::vector<PointType> const& points,
        std::map<std::string,CompositionSet> const& phase_list,
        evalconditions const& conditions
    ) {
        BOOST_ASSERT(critical_edge_length>0);
        // Calculate the "true energy" of the midpoint of two points, based on their IDs
        // If the phases are distinct, the "true energy" is infinite (indicates true line)
        auto calculate_global_midpoint_energy = [this,&conditions,&phase_list] 
        (const std::size_t point1_id, const std::size_t point2_id) 
        { 
            BOOST_ASSERT ( point1_id < hull_map.size() );
            BOOST_ASSERT ( point2_id < hull_map.size() );
            if ( point1_id == point2_id) return hull_map[point1_id].energy;
            if (hull_map[point1_id].phase_name != hull_map[point2_id].phase_name) {
                // Can't calculate a "true energy" if the tie points are different phases
                return std::numeric_limits<EnergyType>::max();
            }
            // Return the energy of the average of the internal degrees of freedom
            else {
                PointType midpoint ( hull_map[point1_id].internal_coordinates );
                PointType point2 ( hull_map[point2_id].internal_coordinates );
                auto current_comp_set = phase_list.find ( hull_map[point1_id].phase_name );
                std::transform ( 
                point2.begin(), 
                                point2.end(),
                                midpoint.begin(),
                                midpoint.begin(),
                                std::plus<EnergyType>()
                ); // sum points together
                for (auto &coord : midpoint) coord /= 2; // divide by two
                auto calculate_energy = [&] (const PointType& point) {
                    return current_comp_set->second.evaluate_objective(conditions,current_comp_set->second.get_variable_map(),const_cast<EnergyType*>(&point[0]));
                };
                return  calculate_energy ( midpoint ); 
            }
        };
        // Use qhull to get the global hull
        return details::global_lower_convex_hull( points, 
                                                  critical_edge_length, 
                                                  calculate_global_midpoint_energy
                                                );
    };
    /* GlobalMinimizer works by taking the phase information for the system and a
     * list of functors that implement point sampling and convex hull calculation.
     * Once GlobalMinimizer is constructed, the user can filter against the calculated grid.
     */
    virtual void run ( 
        std::map<std::string,CompositionSet> const &phase_list,
        sublattice_set const &sublset,
        evalconditions const& conditions
    ) 
    {
        BOOST_LOG_NAMED_SCOPE ( "GlobalMinimizer::run" );
        BOOST_LOG_CHANNEL_SEV ( class_log, "optimizer", debug ) << "enter";
        std::vector<PointType> temporary_hull_storage;

        BOOST_ASSERT(critical_edge_length>0);
        BOOST_ASSERT(initial_subdivisions_per_axis>0);
        BOOST_ASSERT(refinement_subdivisions_per_axis>0);
        BOOST_ASSERT(max_search_depth>=0);
        
        for ( auto comp_set = phase_list.begin(); comp_set != phase_list.end(); ++comp_set ) {
            std::set<std::size_t> dependent_dimensions;
            std::size_t current_dependent_dimension = 0;
            // Create a callback function for energy calculation for this phase
            auto calculate_energy = [&comp_set,&conditions] (const PointType& point) {
                return comp_set->second.evaluate_objective(conditions,comp_set->second.get_variable_map(),const_cast<EnergyType*>(&point[0]));
            };
            // Determine the indices of the dependent dimensions
            boost::multi_index::index<sublattice_set,phase_subl>::type::iterator ic0,ic1;
            int sublindex = 0;
            ic0 = boost::multi_index::get<phase_subl> ( sublset ).lower_bound ( boost::make_tuple ( comp_set->first, sublindex ) );
            ic1 = boost::multi_index::get<phase_subl> ( sublset ).upper_bound ( boost::make_tuple ( comp_set->first, sublindex ) );
            
            while ( ic0 != ic1 ) {
                const std::size_t number_of_species = std::distance ( ic0,ic1 );
                if ( number_of_species > 0 ) {
                    // Last component is dependent dimension
                    current_dependent_dimension += (number_of_species-1);
                    dependent_dimensions.insert(current_dependent_dimension);
                    ++current_dependent_dimension;
                }
                // Next sublattice
                ++sublindex;
                ic0 = boost::multi_index::get<phase_subl> ( sublset ).lower_bound ( boost::make_tuple ( comp_set->first, sublindex ) );
                ic1 = boost::multi_index::get<phase_subl> ( sublset ).upper_bound ( boost::make_tuple ( comp_set->first, sublindex ) );
            }
            // Sample the composition space of this phase
            auto phase_points = this->point_sample ( comp_set->second, sublset, conditions );
            // Calculate the phase's internal convex hull and store the result
            auto phase_hull_points = this->internal_hull ( comp_set->second, phase_points, dependent_dimensions, conditions );
            // TODO: Apply phase-specific constraints to internal dof and globally
            // Add all points from this phase's convex hull to our internal hull map
            for ( auto point : phase_hull_points ) {
                // All points added to the hull_map could possibly be on the global hull
                auto global_point = convert_site_fractions_to_mole_fractions ( comp_set->first, sublset, point );
                PointType ordered_global_point;  
                ordered_global_point.reserve ( global_point.size()+1 );
                for ( auto pt : global_point ) ordered_global_point.push_back ( pt.second );
                double energy = calculate_energy ( point );
                hull_map.insert_point ( 
                comp_set->first, 
                energy, 
                point,
                global_point
                );
                ordered_global_point.push_back ( energy );
                temporary_hull_storage.push_back ( std::move ( ordered_global_point ) );
            }
        }
        // TODO: Add points and set options related to activity constraints here
        // Determine the facets on the global convex hull of all phase's energy landscapes
        candidate_facets = this->global_hull ( temporary_hull_storage, phase_list, conditions );
        BOOST_LOG_SEV ( class_log, debug ) << "candidate_facets.size() = " << candidate_facets.size();
        // Mark all hull entries that are on the global hull
        for ( auto facet : candidate_facets ) {
            for ( auto point : facet.vertices ) {
                const std::size_t point_id = point;
                // point_id is on the global hull
                hull_map.set_global_hull_status ( point_id, true);
            }
        }
    }
    
    typename HullMapType::HullEntryContainerType get_hull_entries() const {
        return hull_map.get_all_points();
    }
    std::vector<FacetType> get_facets() const {
        return candidate_facets;
    }
    
    std::vector<typename HullMapType::HullEntryType> find_tie_points ( 
        evalconditions const& conditions
        ) {
        BOOST_LOG_NAMED_SCOPE ( "GlobalMinimizer::find_tie_points" );
        const double critical_edge_length = 0.05;
        // Filter candidate facets based on user-specified constraints
        std::set<std::size_t> candidate_ids; // ensures returned points are unique
        std::vector<FacetType> pre_candidate_facets; // check all facets before adding to candidates
        std::vector<typename HullMapType::HullEntryType> candidates;
        BOOST_LOG_SEV ( class_log, debug ) << "candidate_facets.size() = " << candidate_facets.size();
        for ( auto facet : candidate_facets ) {
            std::stringstream logbuf;
            logbuf << "Checking facet ";
            logbuf << "[";
            for ( auto point : facet.vertices ) {
                const std::size_t point_id = point;
                auto point_entry = hull_map [ point_id ];
                logbuf << "(";
                for ( auto coord : point_entry.global_coordinates ) {
                    logbuf << coord.first << ":" << coord.second << ",";
                }
                logbuf << ")";
            }
            logbuf << "]";
            BOOST_LOG_SEV ( class_log, debug ) << logbuf.str();
            bool failed_conditions = false;
            
            // Determine if the user-specified point is inside this facet
            boost::numeric::ublas::vector<CoordinateType> trial_point ( conditions.xfrac.size()+1 );
            for ( auto coord = conditions.xfrac.begin(); coord !=  conditions.xfrac.end(); ++coord) {
                trial_point [ std::distance(conditions.xfrac.begin(),coord) ] = coord->second;
            }
            trial_point [ conditions.xfrac.size() ] = 1;
            BOOST_LOG_SEV ( class_log, debug ) << "trial_point: " << trial_point;
            BOOST_LOG_SEV ( class_log, debug ) << "facet.basis_matrix: " << facet.basis_matrix;
            auto trial_vector = boost::numeric::ublas::prod ( facet.basis_matrix, trial_point );
            BOOST_LOG_SEV ( class_log, debug ) << "trial_vector: " << trial_vector;
            for ( auto coord : trial_vector ) {
                if ( coord < 0 ) {
                    failed_conditions = true;
                    break;
                }
            }
            std::cout << "POINT DEBUGGING" << std::endl;
            for ( auto hull_entry : hull_map.get_all_points() ) {
                for ( auto coord : hull_entry.global_coordinates ) {
                    std::cout << coord.second << " ";
                }
                std::cout  << std::endl;
            }
            std::cout << "FACET DEBUGGING" << std::endl;
            for ( auto facet : candidate_facets ) {
                for ( auto vertex : facet.vertices ) {
                    std::cout << vertex << " ";
                }
                std::cout << std::endl;
            }
            
            if ( !failed_conditions ) {
                // This is a pre-candidate facet!
                // It's possible we will have more than one due to edge/corner cases
                // We can select some method of choosing between them
                pre_candidate_facets.push_back ( facet );
                std::stringstream logbuf;
                logbuf << "Candidate facet ";
                for ( auto point : facet.vertices ) {
                    const std::size_t point_id = point;
                    auto point_entry = hull_map [ point_id ];
                    logbuf << "[";
                    for ( auto coord : point_entry.internal_coordinates ) {
                        logbuf << coord << ",";
                    }
                    logbuf << "]";
                    logbuf << "{";
                        for ( auto coord : point_entry.global_coordinates ) {
                            logbuf << coord.first << ":" << coord.second << ",";
                        }
                        logbuf << "}";
                }
                BOOST_LOG_SEV ( class_log, debug ) << logbuf.str();
            }
        }
        
        // Sort candidate_facets by area; choose candidate facet with smallest area
        // If two candidate facets have the same area, we just choose the first one
        auto facet_area_comparator = [] (const FacetType &a,const FacetType &b) { return a.area<b.area; };
        std::sort ( pre_candidate_facets.begin(), pre_candidate_facets.end() , facet_area_comparator );
        
        if ( pre_candidate_facets.size() == 0 ) return candidates; // No candidate facets; return empty-handed
        
        auto final_facet = pre_candidate_facets.begin();

        // final_facet satisfies all the conditions; return its tie points
        
        for_each_pair (final_facet->vertices.begin(), final_facet->vertices.end(), 
            [this,&candidate_ids,critical_edge_length](
                decltype( final_facet->vertices.begin() ) point1, 
                                                        decltype( final_facet->vertices.begin() ) point2
            ) { 
                const std::size_t point1_id = *point1;
                const std::size_t point2_id = *point2;
                auto point1_entry = hull_map [ point1_id ];
                auto point2_entry = hull_map [ point2_id ];
                if ( point1_entry.phase_name != point2_entry.phase_name ) {
                    // phases differ; definitely a tie line
                    BOOST_LOG_SEV ( class_log, debug ) << "Adding tie points " << point1_id << "(" << point1_entry.phase_name << ") and " << point2_id << "(" << point2_entry.phase_name << ")";
                    candidate_ids.insert ( point1_id );
                    candidate_ids.insert ( point2_id );
                }
                else {
                    // phases are the same -- does a tie line span a miscibility gap?
                    // use internal coordinates to check
                    CoordinateType distance = 0;
                    auto difference = point2_entry.internal_coordinates;
                    auto diff_iter = difference.begin();
                    
                    for ( auto coord = point1_entry.internal_coordinates.begin(); coord != point1_entry.internal_coordinates.end(); ++coord) {
                        *(diff_iter++) -= *coord;
                    }
                    for (auto coord : difference) { distance += std::pow(coord,2); }
                    
                    distance = sqrt ( distance );
                       
                    if (distance > critical_edge_length) {
                        // the tie line is sufficiently long
                        BOOST_LOG_SEV ( class_log, debug ) << "Adding tie points " << point1_id << " and " << point2_id << "(distance " << distance << " satisfies critical_edge_length " << critical_edge_length;
                        candidate_ids.insert ( point1_id );
                        candidate_ids.insert ( point2_id );
                    }
                }
            });
        
        // If two tie points come from the same phase and are very close together,
        // one should be discarded; this is the merge step
        for ( auto point1 = candidate_ids.begin(); point1 != candidate_ids.end(); ++point1 ) {
            for ( auto point2 = point1; ++point2 != candidate_ids.end(); /**/ ) {
                const std::size_t point1_id = *point1;
                const std::size_t point2_id = *point2;
                auto point1_entry = hull_map [ point1_id ];
                auto point2_entry = hull_map [ point2_id ];
                // don't merge points from different phases
                if ( point1_entry.phase_name != point2_entry.phase_name ) { continue; }
                CoordinateType distance = 0;
                auto difference = point2_entry.internal_coordinates;
                auto diff_iter = difference.begin();
                
                for ( auto coord = point1_entry.internal_coordinates.begin(); coord != point1_entry.internal_coordinates.end(); ++coord) {
                    *(diff_iter++) -= *coord;
                }
                for (auto coord : difference) { distance += std::pow(coord,2); }
                
                distance = sqrt ( distance );
                
                if (distance <= critical_edge_length) {
                    // this tie line is not real; remove one of the points (arbitrarily, point2)
                    BOOST_LOG_SEV ( class_log, debug ) << "Removing tie point " << point2_id;
                    candidate_ids.erase ( point2_id );
                    // reset pairwise check after one ID is erased
                    point1 = candidate_ids.begin();
                    point2 = ++( candidate_ids.begin() );
                }
            }
        }
        
        // If there are no candidate IDs yet, no tie lines were found
        // We must be in a single phase region; just add the first vertex
        // from the "tie plane"
        if (candidate_ids.size() == 0) {
            BOOST_LOG_SEV ( class_log, debug ) << "Adding single-phase point " << *( final_facet->vertices.begin() );
            candidate_ids.insert ( *( final_facet->vertices.begin() ) );
        }
        
        // Dereference point IDs to hull entries
        for (auto point_id : candidate_ids) {
            candidates.push_back ( hull_map [ point_id ] );
        }
        return std::move( candidates );
    }
};

} //namespace Optimizer


#endif