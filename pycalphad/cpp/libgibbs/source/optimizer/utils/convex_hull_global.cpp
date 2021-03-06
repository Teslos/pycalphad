/*=============================================================================
 Copyright (c) 2012-2014 Richard Otis
 
 Distributed under the Boost Software License, Version 1.0. (See accompanying
 file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 =============================================================================*/

// Calculate global convex hull using Qhull / libqhullcpp

#include "libgibbs/include/libgibbs_pch.hpp"
#include "libgibbs/include/optimizer/utils/convex_hull.hpp"
#include "libgibbs/include/optimizer/utils/simplicial_facet.hpp"
#include "libgibbs/include/utils/invert_matrix.hpp"
#include <libqhullcpp/RboxPoints.h>
#include <libqhullcpp/QhullError.h>
#include <libqhullcpp/QhullQh.h>
#include <libqhullcpp/QhullFacet.h>
#include <libqhullcpp/QhullFacetList.h>
#include <libqhullcpp/QhullHyperplane.h>
#include <libqhullcpp/QhullLinkedList.h>
#include <libqhullcpp/QhullPoint.h>
#include <libqhullcpp/QhullVertex.h>
#include <libqhullcpp/QhullVertexSet.h>
#include <libqhullcpp/Qhull.h>
#include <boost/assert.hpp>
#include <map>
#include <set>
#include <string>
#include <sstream>
#include <algorithm>
#include <functional>
#include <cmath>

using orgQhull::RboxPoints;
using orgQhull::Qhull;
using orgQhull::QhullError;
using orgQhull::QhullFacet;
using orgQhull::QhullFacetList;
using orgQhull::QhullPoint;
using orgQhull::QhullQh;
using orgQhull::RboxPoints;
using orgQhull::QhullVertex;
using orgQhull::QhullVertexSet;

namespace Optimizer { namespace details {
// Modified QuickHull algorithm using d-dimensional Beneath-Beyond
// Reference: N. Perevoshchikova, et al., 2012, Computational Materials Science.
// "A convex hull algorithm for a grid minimization of Gibbs energy as initial step 
//    in equilibrium calculations in two-phase multicomponent alloys"
std::vector<SimplicialFacet<double>> global_lower_convex_hull (
    const std::vector<std::vector<double>> &points,
    const double critical_edge_length,
    const std::function<double(const std::size_t, const std::size_t)> calculate_midpoint_energy
) {
    BOOST_ASSERT(points.size() > 0);
    BOOST_ASSERT(critical_edge_length > 0);
    const double coplanarity_allowance = 0.001; // max energy difference (%/100) to still be on tie plane
    std::vector<SimplicialFacet<double>> candidates;
    const std::size_t point_dimension = points.begin()->size();
    const std::size_t point_count = points.size();
    std::set<std::size_t> candidate_point_ids; // vertices of tie hyperplanes
    RboxPoints point_buffer;
    point_buffer.setDimension ( point_dimension );
    point_buffer.reserveCoordinates ( point_count );
    std::string Qhullcommand = "Qt ";
    
    if (point_count == 1) { // Special case: No composition dependence
        SimplicialFacet<double> new_facet;
        new_facet.vertices = std::vector<std::size_t>();
        new_facet.normal = std::vector<double>();
        new_facet.vertices.push_back ( 0 );
        new_facet.normal.push_back ( 0 );
        new_facet.area = 0;
        candidates.push_back ( new_facet );
        return candidates;
    }
    // TODO: Handle degenerate case when point_count <= point_dimension
    
    // Copy all of the points into a buffer compatible with Qhull
    for (auto pt : points) {
        point_buffer.append ( QhullPoint ( point_dimension, &pt[0] ) );
    }
    
    // Mark last dimension as dependent for Qhull so it can be discarded
    std::stringstream stream;
    // Qhull command "Qbk:0Bk:0" drops dimension k from consideration
    // Remove dependent coordinate (second to last, energy should be last coordinate)
    stream << " " << "Qb" << point_dimension-2 << ":0B" << point_dimension-2 << ":0";
    //  Degenerate simplex: delete energy coordinate to calculate hull
    Qhullcommand += stream.str();

    //DEBUG std::cout << "DEBUG: Qhullcommand: " << Qhullcommand.c_str() << std::endl;
    // Make the call to Qhull
    Qhull qhull ( point_buffer, Qhullcommand.c_str() );
    // Get all of the facets
    QhullFacetList facets = qhull.facetList();
  
    for (auto facet : facets) {
        bool already_added = false;
        if (facet.isDefined() && facet.isGood() && facet.isSimplicial() ) {
            double orientation = *(facet.hyperplane().constEnd()-1); // last coordinate (energy)
            if (orientation > 0) continue; // consider only the facets of the lower convex hull.
            // skip facets with no normal defined (these are duplicates for some reason)
            if ( std::distance( facet.hyperplane().begin(),facet.hyperplane().end() ) == 0 ) continue;
            QhullVertexSet vertices = facet.vertices();
            const std::size_t vertex_count = vertices.size();
            
            SimplicialFacet<double> new_facet;
            // fill basis matrix
            new_facet.basis_matrix = SimplicialFacet<double>::MatrixType ( vertex_count, vertex_count );
            for ( auto vertex = vertices.begin(); vertex != vertices.end(); ++vertex ) {
                const std::size_t column_index = std::distance ( vertices.begin(), vertex );
                new_facet.vertices.push_back ( vertex->point().id() );
                auto end_coordinate = vertex->point().end()-1; // don't add energy coordinate
                for ( auto coord = vertex->point().begin(); coord != end_coordinate; ++coord ) {
                    const std::size_t row_index = std::distance ( vertex->point().begin(), coord );
                    std::cout << "new_facet.basis_matrix(" << row_index << "," << column_index << ") = " << *coord << std::endl;
                    new_facet.basis_matrix ( row_index, column_index ) = *coord;
                }
                std::cout << "new_facet.basis_matrix(" << vertex_count-1 << "," << column_index << ") = 1" << std::endl;
                new_facet.basis_matrix ( vertex_count-1, column_index ) = 1; // last row is all 1's
            }
            //TODO: bool success = InvertMatrix ( new_facet.basis_matrix, new_facet.basis_matrix );
            //if ( !success ) std::cout << "MATRIX INVERSION FAILED" << std::endl;
            for ( const auto coord : facet.hyperplane() ) {
                new_facet.normal.push_back ( coord );
            }
            new_facet.area = facet.facetArea( qhull.runId() );
            candidates.push_back ( new_facet );
            already_added = true;
            //DEBUG std::cout << facet;
            
            continue;
            
            // Only facets with edges beyond the critical length are candidate tie hyperplanes
            // Check the length of all edges (dimension 1) in the facet
            for (auto vertex1 = 0; vertex1 < vertex_count && !already_added; ++vertex1) {
                const std::size_t vertex1_point_id = vertices[vertex1].point().id();
                const double vertex1_energy = calculate_midpoint_energy ( vertex1_point_id, vertex1_point_id );
                //std::cout << "vertex1_energy = " << vertex1_energy << std::endl;
                std::vector<double> pt_vert1 = vertices[vertex1].point().toStdVector();
                //pt_vert1.pop_back(); // Remove the last coordinate (energy) for this check
                for (auto vertex2 = 0; vertex2 < vertex1 && !already_added; ++vertex2) {
                    const std::size_t vertex2_point_id = vertices[vertex2].point().id();
                    const double vertex2_energy = calculate_midpoint_energy ( vertex2_point_id, vertex2_point_id );
                    //std::cout << "vertex2_energy = " << vertex2_energy << std::endl;
                    std::vector<double> pt_vert2 = vertices[vertex2].point().toStdVector();
                    //pt_vert2.pop_back(); // Remove the last coordinate (energy) for this check
                    std::vector<double> difference ( pt_vert2.size() );
                    std::vector<double> midpoint ( pt_vert2.size() ); // midpoint of the edge
                    std::transform (pt_vert2.begin(), pt_vert2.end(), 
                                    pt_vert1.begin(), midpoint.begin(), std::plus<double>() );
                    for (auto &coord : midpoint) coord /= 2;
                    const double lever_rule_energy = (vertex1_energy + vertex2_energy)/2;
                    // This will return a type's max() if the phases are different (always true tie line)
                    const double true_energy = calculate_midpoint_energy
                                               ( 
                                                     vertex1_point_id, 
                                                     vertex2_point_id 
                                               );
                    // If the true energy is "much" greater, it's a true tie line
                    /*std::cout << "pt_vert1(" << vertex1_point_id << "): ";
                    for (auto &coord : pt_vert1) std::cout << coord << ",";
                    std::cout << ":: ";
                    std::cout << "pt_vert2(" << vertex2_point_id << "): ";
                    for (auto &coord : pt_vert2) std::cout << coord << ",";
                    std::cout << ":: ";
                    std::cout << "midpoint: ";
                    for (auto &coord : midpoint) std::cout << coord << ",";
                    std::cout << std::endl;
                    std::cout << "true_energy: " << true_energy << " lever_rule_energy: " << lever_rule_energy << std::endl;*/
                    // We use fabs() here so we don't accidentally flip the sign of the comparison
                    if ( (true_energy-lever_rule_energy)/fabs(lever_rule_energy) < coplanarity_allowance ) {
                        continue; // not a true tie line, skip it
                    }
                    
                    double distance = 0;
                    // Subtract vertex1 from vertex2 to get the distance
                    std::transform (pt_vert2.begin(), pt_vert2.end()-1, 
                                    pt_vert1.begin(), difference.begin(), std::minus<double>() );
                    // Sum the square of all elements of vertex2-vertex1
                    for (auto coord : difference) distance += std::pow(coord,2);
                    // Square root the result
                    distance = sqrt(distance);
                    /*DEBUG std::cout << "Edge length: " << distance << std::endl;
                    std::cout << "Vertex1: ";
                    for (auto coord : pt_vert1) std::cout << coord << ",";
                    std::cout << std::endl;
                    std::cout << "Vertex2: ";
                    for (auto coord : pt_vert2) std::cout << coord << ",";
                    std::cout << std::endl;*/
                    SimplicialFacet<double> new_facet;
                    for ( auto vertex = vertices.begin(); vertex != vertices.end(); ++vertex ) {
                        new_facet.vertices.push_back ( vertex->point().id() );
                    }
                    for ( auto coord : facet.hyperplane() ) {
                        new_facet.normal.push_back ( coord );
                    }
                    new_facet.area = facet.facetArea( qhull.runId() );
                    candidates.push_back ( new_facet );
                    already_added = true;
                    //DEBUG std::cout << facet;
                }
            }
        }
    }
    return candidates;
}
} // namespace details
} // namespace Optimizer