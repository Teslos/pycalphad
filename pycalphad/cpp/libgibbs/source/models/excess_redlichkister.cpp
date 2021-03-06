/*=============================================================================
	Copyright (c) 2012-2013 Richard Otis

	Based on example code from Boost MultiIndex.
	Copyright (c) 2003-2008 Joaquin M Lopez Munoz.
	See http://www.boost.org/libs/multi_index for library home page.

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

// Model for excess Gibbs energy using Redlich-Kister polynomials

#include "libgibbs/include/libgibbs_pch.hpp"
#include "libgibbs/include/models.hpp"
#include "libgibbs/include/optimizer/opt_Gibbs.hpp"
#include "libgibbs/include/utils/math_expr.hpp"
#include "libtdb/include/logging.hpp"
#include <string>
#include <sstream>
#include <set>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/tuple/tuple.hpp>

using boost::spirit::utree;
typedef boost::spirit::utree_type utree_type;
using boost::multi_index_container;
using namespace boost::multi_index;

RedlichKisterExcessEnergyModel::RedlichKisterExcessEnergyModel(
		const std::string &phasename,
		const sublattice_set &subl_set,
		const parameter_set &param_set
		) : EnergyModel(phasename, subl_set, param_set) {
	BOOST_LOG_NAMED_SCOPE("RedlichKisterExcessEnergyModel::RedlichKisterExcessEnergyModel");
	logger model_log(journal::keywords::channel = "optimizer");
	BOOST_LOG_SEV(model_log, debug) << "enter";
	sublattice_set_view ssv;
	parameter_set_view psv;
	parameter_set_view psv_subview;

	// Get all the sublattices for this phase
	boost::multi_index::index<sublattice_set,phases>::type::iterator ic0,ic1;
	boost::tuples::tie(ic0,ic1)=get<phases>(subl_set).equal_range(phasename);

	// Construct a view from the iterators
	while (ic0 != ic1) {
		ssv.insert(&*ic0);
		++ic0;
	}

	// Get all the parameters for this phase
	boost::multi_index::index<parameter_set,phase_index>::type::iterator pa_start,pa_end;
	boost::tuples::tie(pa_start,pa_end)=get<phase_index>(param_set).equal_range(phasename);

	// Construct a view from the iterators
	while (pa_start != pa_end) {
		psv.insert(&*pa_start);
		++pa_start;
	}

	// build a subview to the parameters that we are interested in
	boost::multi_index::index<parameter_set_view,type_index>::type::iterator it0, it1;
	std::string scantype = "G";
	boost::tuples::tie(it0,it1)=get<type_index>(psv).equal_range(scantype);

	// Construct a subview from the view
	while (it0 != it1) {
		psv_subview.insert(*it0);
		++it0;
	}

	// Also include parameters of type "L"
	scantype = "L";
	boost::tuples::tie(it0,it1)=get<type_index>(psv).equal_range(scantype);

	// Construct a subview from the view
	while (it0 != it1) {
		psv_subview.insert(*it0);
		++it0;
	}

	// Get the reference energy by permuting the site fractions and finding parameters
	model_ast = permute_site_fractions_with_interactions(ssv, sublattice_set_view(), psv_subview, (int)0);
	// Normalize the reference Gibbs energy by the total number of mixing sites in this phase
	normalize_utree(model_ast, ssv);
	BOOST_LOG_SEV(model_log, debug) << "exit";
}

utree EnergyModel::permute_site_fractions_with_interactions (
		const sublattice_set_view &total_view, // all sublattices
		const sublattice_set_view &subl_view, // the active sublattice permutation
		const parameter_set_view &param_view,
		const int &sublindex,
		const double &param_division_factor // divide all parameters by this factor
		) {
	BOOST_LOG_NAMED_SCOPE("EnergyModel::permute_site_fractions_with_interactions");
	logger model_log(journal::keywords::channel = "optimizer");
	BOOST_LOG_SEV(model_log, debug) << "enter";
	utree ret_tree;
	// Construct a view of just the current sublattice
	boost::multi_index::index<sublattice_set_view,myindex>::type::iterator ic0,ic1;
	boost::tuples::tie(ic0,ic1)=get<myindex>(total_view).equal_range(sublindex);

	if (ic0 == ic1) {
		/* We are at the bottom of the recursive loop
		 * or there's an empty sublattice for some reason (bad).
		 * Use the sublattice permutation to find a matching
		 * parameter, if it exists.
		 */

		if (subl_view.size() == sublindex) return utree(); // skip non-interaction parameters

		utree ret_tree = find_parameter_ast(subl_view, param_view);

		if (param_division_factor != 1) {
			utree temp_tree;
			temp_tree.push_back("/");
			temp_tree.push_back(ret_tree);
			temp_tree.push_back(param_division_factor);
			ret_tree.swap(temp_tree);
		}

		return ret_tree;
	}

	for (auto i = ic0; i != ic1; ++i) {
		BOOST_LOG_SEV(model_log, debug) << "checking " << (*i)->name();
		sublattice_set_view temp_view = subl_view;
		utree current_product;
		utree buildtree;
		temp_view.insert((*i)); // add current species to the view
		/* Construct the expression tree.
		 * Start by building the recursive product of site fractions.
		 */

		utree recursive_term = permute_site_fractions_with_interactions(total_view, temp_view, param_view, sublindex+1, param_division_factor);

		// Calculate all the two-species interactions
		for (auto j = ic0; j != ic1; ++j) {
			if (j == i) continue; // ignore self-interactions
			BOOST_LOG_SEV(model_log, debug) << "checking " << (*i)->name() << "," << (*j)->name();
			sublattice_set_view interaction_view = temp_view;
			utree interact_product, interact_recursive_term, interact_temptree;
			interaction_view.insert(*j); // add interacting species to subview

			interact_recursive_term = simplify_utree(permute_site_fractions_with_interactions(total_view, interaction_view, param_view, sublindex+1, param_division_factor));

			// Calculate all the three-species interactions
			for (auto k = ic0; k != ic1; ++k) {
				if (k == j) continue; // ignore self-interactions
				if (k == i) continue; // ignore self-interactions
				BOOST_LOG_SEV(model_log, debug) << "checking " << (*i)->name() << "," << (*j)->name() << "," << (*k)->name();
				sublattice_set_view ternary_interaction_view = interaction_view;
				utree ternary_interact_product;
				utree ternary_interact_recursive_term;
				ternary_interaction_view.insert(*k); // add interacting species to subview

				ternary_interact_recursive_term = simplify_utree(permute_site_fractions_with_interactions(total_view, ternary_interaction_view, param_view, sublindex+1, param_division_factor));

				if (is_zero_tree(ternary_interact_recursive_term)) continue;
				if (ternary_interact_recursive_term.which() == utree_type::invalid_type) continue;
				// only here for non-zero terms
				BOOST_LOG_SEV(model_log, debug) << "found: " << (*i)->name() << "," << (*j)->name() << "," << (*k)->name();

				// interacting species multiplication
				ternary_interact_product.push_back("*");
				ternary_interact_product.push_back(utree((*k)->name()));
				ternary_interact_product.push_back(ternary_interact_recursive_term);

				if (interact_recursive_term.which() == utree_type::invalid_type) {
					interact_recursive_term = ternary_interact_product; // no prior product exists
				}
				else {
					// Contribute term to the sum
					utree ternary_temptree;
					ternary_temptree.push_back("+");
					ternary_temptree.push_back(ternary_interact_product);
					ternary_temptree.push_back(interact_recursive_term);
					interact_recursive_term.swap(ternary_temptree);
				}
			}

			if (is_zero_tree(interact_recursive_term)) continue;
			if (interact_recursive_term.which() == utree_type::invalid_type) continue;

			// We only get here for non-zero terms
			interact_product.push_back("*");
			// interacting species multiplication
			interact_product.push_back(utree((*j)->name()));
			interact_product.push_back(interact_recursive_term);

			if (recursive_term.which() == utree_type::invalid_type) {
				recursive_term = interact_product; // no prior product exists
			}
			else {
				// Contribute term to the sum
				interact_temptree.push_back("+");
				interact_temptree.push_back(interact_product);
				interact_temptree.push_back(recursive_term);
				recursive_term.swap(interact_temptree);
			}
		}


		if (is_zero_tree(recursive_term)) continue;
		if (recursive_term.which() == utree_type::invalid_type) continue;

		// we only get here for non-zero terms
		current_product.push_back("*");
		current_product.push_back(utree((*i)->name()));
		current_product.push_back(recursive_term);
		// Contribute this product to the sum
		// Check if we are on the first (or only) term in the sum
		if (ret_tree.which() != utree_type::invalid_type) {
			buildtree.push_back("+");
			buildtree.push_back(ret_tree);
			buildtree.push_back(current_product);
			ret_tree.swap(buildtree);
		}
		else ret_tree.swap(current_product);
	}

	if (ret_tree.which() == utree_type::invalid_type) ret_tree = utree(0); // no parameter for this term
	BOOST_LOG_SEV(model_log, debug) << "returning " << ret_tree;
	return ret_tree;
}
