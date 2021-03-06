/*=============================================================================
	Copyright (c) 2012-2014 Richard Otis

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

// definition for CompositionSet class

#include "libgibbs/include/libgibbs_pch.hpp"
#include "libtdb/include/logging.hpp"
#include "libgibbs/include/compositionset.hpp"
#include "libgibbs/include/utils/ast_container_rename.hpp"
#include "libgibbs/include/utils/ast_multi_index_rename.hpp"
#include "libgibbs/include/utils/math_expr.hpp"
#include "libgibbs/include/utils/qr.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/numeric/ublas/symmetric.hpp>
#include <boost/numeric/ublas/vector.hpp>
#include <boost/numeric/ublas/matrix_proxy.hpp>
#include <boost/numeric/ublas/matrix.hpp>
#include <boost/numeric/ublas/operation.hpp>
#include <boost/numeric/ublas/io.hpp>
#include <boost/bimap.hpp>

using boost::multi_index_container;
using namespace boost::multi_index;

CompositionSet::CompositionSet (
    const Phase &phaseobj,
    const parameter_set &pset,
    const sublattice_set &sublset,
    boost::bimap<std::string, int> const &main_indices )
{
    typedef boost::bimap<std::string, int>::value_type position;
    BOOST_LOG_NAMED_SCOPE ( "CompositionSet::CompositionSet" );
    logger comp_log ( journal::keywords::channel = "optimizer" );
    cset_name = phaseobj.name();

    // Now initialize the appropriate models
    models["PURE_ENERGY"] = std::unique_ptr<EnergyModel> ( new PureCompoundEnergyModel ( phaseobj.name(), sublset, pset ) );
    models["IDEAL_MIX"] = std::unique_ptr<EnergyModel> ( new IdealMixingModel ( phaseobj.name(), sublset ) );
    models["REDLICH_KISTER"] = std::unique_ptr<EnergyModel> ( new RedlichKisterExcessEnergyModel ( phaseobj.name(), sublset, pset ) );
    models["IHJ_MAGNETIC"] = std::unique_ptr<EnergyModel> ( new IHJMagneticModel ( phaseobj.name(), sublset, pset,
                             phaseobj.magnetic_afm_factor, phaseobj.magnetic_sro_enthalpy_order_fraction ) );

    for ( auto i = models.begin(); i != models.end(); ++i ) {
        auto symbol_table = i->second->get_symbol_table();
        symbols.insert ( symbol_table.begin(), symbol_table.end() ); // copy model symbols into main symbol table
        // TODO: we don't check for duplicate symbols at all here...models police themselves to avoid collisions
        // One idea: put all symbols into model-specific namespaces
        for ( auto j = symbol_table.begin(); j != symbol_table.end(); ++j ) {
            BOOST_LOG_SEV ( comp_log, debug ) << "added symbol " << j->first << " to composition set " << cset_name << ": "<< j->second.get();
        }
    }

    // Calculate first derivative ASTs of all variables
    for ( auto i = main_indices.left.begin(); i != main_indices.left.end(); ++i ) {
        std::list<std::string> diffvars;
        diffvars.push_back ( i->first );
        if ( !boost::algorithm::starts_with ( i->first, cset_name ) ) {
            // the differentiating variable doesn't come from this composition set
            // the derivative should be zero, so skip calculation
            continue;
        }
        for ( auto j = models.cbegin(); j != models.cend(); ++j ) {
            boost::spirit::utree difftree;
            if ( i->first == ( cset_name + "_FRAC" ) ) {
                // the derivative w.r.t the phase fraction is just the energy of this phase
                difftree = j->second->get_ast();
            } else {
                difftree = simplify_utree ( differentiate_utree ( j->second->get_ast(), i->first, symbols ) );
            }
            if ( !is_zero_tree ( difftree ) ) {
                tree_data.insert ( ast_entry ( diffvars, j->first, difftree ) );
            }

            // Calculate second derivative ASTs of all variables (doesn't include constraint contribution)
            for ( auto k = main_indices.left.begin(); k != main_indices.left.end(); ++k ) {
                // second derivative of obj function w.r.t i,j
                if ( i->second > k->second ) {
                    continue;    // skip upper triangular
                }
                std::list<std::string> second_diffvars = diffvars;
                second_diffvars.push_back ( k->first );
                if ( k->first == ( cset_name + "_FRAC" ) ) {
                    // second derivative w.r.t phase fraction is zero
                } else if ( !boost::algorithm::starts_with ( k->first, cset_name ) ) {
                    // the differentiating variable doesn't come from this composition set
                    // the derivative should be zero, so skip calculation
                } else {
                    boost::spirit::utree second_difftree = simplify_utree ( differentiate_utree ( difftree, k->first, symbols ) );
                    if ( !is_zero_tree ( second_difftree ) ) {
                        tree_data.insert ( ast_entry ( second_diffvars, j->first, second_difftree ) );
                    }
                }
            }
        }
    }

    // Add the mandatory site fraction balance constraints
    boost::multi_index::index<sublattice_set,phase_subl>::type::iterator ic0,ic1;
    int sublindex = 0;
    int varcount = 0;
    ic0 = boost::multi_index::get<phase_subl> ( sublset ).lower_bound ( boost::make_tuple ( phaseobj.name(),sublindex ) );
    ic1 = boost::multi_index::get<phase_subl> ( sublset ).upper_bound ( boost::make_tuple ( phaseobj.name(),sublindex ) );
    while ( ic0 != ic1 ) {
        // Current sublattice
        std::vector<std::string> subl_list;
        for ( ; ic0 != ic1 ; ++ic0 ) {
            subl_list.push_back ( ic0->species );
            phase_indices.insert ( position ( ic0->name(), varcount++ ) );
            BOOST_LOG_SEV ( comp_log, debug ) << "phase_indices[" << ic0->name() << "] = " << varcount-1;
            BOOST_LOG_SEV ( comp_log, debug ) << "phase_indices.size() = " << phase_indices.size();
        }
        if ( subl_list.size() >= 1 ) {
            cm.addConstraint (
                SublatticeBalanceConstraint (
                    phaseobj.name(),
                    sublindex,
                    subl_list.cbegin(),
                    subl_list.cend()
                )
            );
        }

        ++sublindex;
        ic0 = boost::multi_index::get<phase_subl> ( sublset ).lower_bound ( boost::make_tuple ( phaseobj.name(),sublindex ) );
        ic1 = boost::multi_index::get<phase_subl> ( sublset ).upper_bound ( boost::make_tuple ( phaseobj.name(),sublindex ) );
    }

    // Calculate first derivative ASTs of all constraints
    for ( auto i = phase_indices.left.begin(); i != phase_indices.left.end(); ++i ) {
        // for each variable, calculate derivatives of all the constraints
        for ( auto j = cm.constraints.begin(); j != cm.constraints.end(); ++j ) {
            boost::spirit::utree lhs = differentiate_utree ( j->lhs, i->first );
            boost::spirit::utree rhs = differentiate_utree ( j->rhs, i->first );
            lhs = simplify_utree ( lhs );
            rhs = simplify_utree ( rhs );
            if (
                ( lhs.which() == boost::spirit::utree_type::double_type || lhs.which() == boost::spirit::utree_type::int_type )
                &&
                ( rhs.which() == boost::spirit::utree_type::double_type || rhs.which() == boost::spirit::utree_type::int_type )
            ) {
                double lhsget, rhsget;
                lhsget = lhs.get<double>();
                rhsget = rhs.get<double>();
                if ( lhsget == rhsget ) {
                    continue;    // don't add zeros to the Jacobian
                }
            }
            boost::spirit::utree subtract_tree;
            subtract_tree.push_back ( "-" );
            subtract_tree.push_back ( lhs );
            subtract_tree.push_back ( rhs );
            int var_index = i->second;
            int cons_index = std::distance ( cm.constraints.begin(),j );
            jac_g_trees.push_back ( jacobian_entry ( cons_index,var_index,false,subtract_tree ) );
            BOOST_LOG_SEV ( comp_log, debug ) << "Jacobian of constraint  " << cons_index << " wrt variable " << var_index << " pre-calculated";
        }
    }

    build_constraint_basis_matrices ( sublset ); // Construct the orthonormal basis in the constraints
}

// make CompositionSet from another CompositionSet; used for miscibility gaps
// this will create a copy
CompositionSet::CompositionSet (
    const CompositionSet &other,
    const std::map<std::string,double> &new_starting_point,
    const std::string &new_name )
{
    BOOST_LOG_NAMED_SCOPE ( "CompositionSet::CompositionSet" );
    logger comp_log ( journal::keywords::channel = "optimizer" );
    BOOST_LOG_SEV( comp_log, debug ) << "entered";
    const std::string old_phase_name ( other.cset_name );
    const std::string new_phase_name ( new_name );
    BOOST_LOG_SEV( comp_log, debug ) << "old_phase_name = " << old_phase_name;
    BOOST_LOG_SEV( comp_log, debug ) << "new_phase_name = " << new_phase_name;
    // These are specified by the user
    cset_name = new_name;
    BOOST_LOG_SEV( comp_log, debug ) << "cset_name set";
    starting_point = new_starting_point;
    BOOST_LOG_SEV( comp_log, debug ) << "new starting_point set";

    // Copy everything else from the parent CompositionSet
    // Deep copy/rename the model map
    for ( auto energymod = other.models.begin(); energymod != other.models.end(); ++energymod ) {
        BOOST_LOG_SEV( comp_log, debug ) << "init DCR EnergyModel " << energymod->first;
        models.emplace ( energymod->first,energymod->second->clone_with_renamed_phase ( old_phase_name, new_phase_name ) );
        BOOST_LOG_SEV( comp_log, debug ) << "DCR complete EnergyModel " << energymod->first;
    }
    BOOST_LOG_SEV( comp_log, debug ) << "DCR EnergyModels complete";
    jac_g_trees = ast_copy_with_renamed_phase ( other.jac_g_trees, old_phase_name, new_phase_name );
    BOOST_LOG_SEV( comp_log, debug ) << "DCR jac_g_trees";
    hessian_data = ast_copy_with_renamed_phase ( other.hessian_data, old_phase_name, new_phase_name );
    BOOST_LOG_SEV( comp_log, debug ) << "DCR hessian_data";
    tree_data = ast_copy_with_renamed_phase ( other.tree_data, old_phase_name, new_phase_name ) ;
    BOOST_LOG_SEV( comp_log, debug ) << "DCR tree_data";
    first_derivatives = ast_copy_with_renamed_phase ( other.first_derivatives, old_phase_name, new_phase_name );
    BOOST_LOG_SEV( comp_log, debug ) << "DCR first_derivatives";
    symbols = ast_copy_with_renamed_phase ( other.symbols, old_phase_name, new_phase_name );
    BOOST_LOG_SEV( comp_log, debug ) << "DCR symbols";
    cm = ConstraintManager();
    for ( const auto &old_cons : other.cm.constraints ) {
        Constraint new_cons ( old_cons );
        ast_variable_rename ( new_cons.lhs,old_phase_name,new_phase_name );
        ast_variable_rename ( new_cons.rhs,old_phase_name,new_phase_name );
        cm.addConstraint ( new_cons );
    }
    BOOST_LOG_SEV( comp_log, debug ) << "DCR cm";
    phase_indices = ast_copy_with_renamed_phase ( other.phase_indices, old_phase_name, new_phase_name );
    BOOST_LOG_SEV( comp_log, debug ) << "DCR phase_indices";
    constraint_null_space_matrix = other.constraint_null_space_matrix;
    BOOST_LOG_SEV( comp_log, debug ) << "exiting";
}
double CompositionSet::evaluate_objective (
    evalconditions const& conditions,
    boost::bimap<std::string, int> const &main_indices,
    double* const x ) const
{
    BOOST_LOG_NAMED_SCOPE ( "CompositionSet::evaluate_objective(evalconditions const& conditions,boost::bimap<std::string, int> const &main_indices,double* const x)" );
    double objective = 0;
    const std::string compset_name ( cset_name + "_FRAC" );

    for ( auto i = models.cbegin(); i != models.cend(); ++i ) {
        objective += process_utree ( i->second->get_ast(), conditions, main_indices, symbols, x ).get<double>();
    }
    return objective;
}
double CompositionSet::evaluate_objective (
    evalconditions const &conditions, std::map<std::string,double> const &variables ) const
{
    // Need to translate this variable map into something process_utree can understand
    BOOST_LOG_NAMED_SCOPE ( "CompositionSet::evaluate_objective(evalconditions const &conditions, std::map<std::string,double> const &variables)" );
    logger comp_log ( journal::keywords::channel = "optimizer" );
    BOOST_LOG_SEV ( comp_log, debug ) << "enter";
    double vars[variables.size()]; // Create Ipopt-style double array
    boost::bimap<std::string, int> main_indices;
    typedef boost::bimap<std::string, int>::value_type position;
    for ( auto i = variables.begin(); i != variables.end(); ++i ) {
        vars[std::distance ( variables.begin(),i )] = i->second; // Copy values into array
        BOOST_LOG_SEV ( comp_log, debug ) << "main_indices.insert(" << i->first << ", " << std::distance ( variables.begin(), i ) << ")";
        main_indices.insert ( position ( i->first, std::distance ( variables.begin(),i ) ) ); // Create fictitious indices
    }
    for ( auto i = main_indices.left.begin(); i != main_indices.left.end(); ++i ) {
        BOOST_LOG_SEV ( comp_log, debug ) << i->first << " -> " << i->second;
    }
    BOOST_LOG_SEV ( comp_log, debug ) << "returning";
    return evaluate_objective ( conditions, main_indices, vars );
}

std::map<int,double> CompositionSet::evaluate_objective_gradient (
    evalconditions const& conditions, boost::bimap<std::string, int> const &main_indices, double* const x ) const
{
    std::map<int,double> retmap;
    boost::multi_index::index<ast_set,ast_deriv_order_index>::type::const_iterator ast_begin,ast_end;
    ast_begin = get<ast_deriv_order_index> ( tree_data ).lower_bound ( 1 );
    ast_end = get<ast_deriv_order_index> ( tree_data ).upper_bound ( 1 );
    const std::string compset_name ( cset_name + "_FRAC" );

    for ( auto i = main_indices.left.begin(); i != main_indices.left.end(); ++i ) {
        retmap[i->second] = 0; // initialize all indices as zero
    }
    for ( ast_set::const_iterator i = ast_begin; i != ast_end; ++i ) {
        const double diffvalue = process_utree ( i->ast, conditions, main_indices, symbols, x ).get<double>();
        const std::string diffvar = * ( i->diffvars.cbegin() ); // get differentiating variable
        const int varindex = main_indices.left.at ( diffvar );
        if ( diffvar != compset_name ) {
            retmap[varindex] += x[main_indices.left.at ( compset_name )] * diffvalue; // multiply derivative by phase fraction
        } else {
            // don't multiply derivative by phase fraction because this is the derivative w.r.t phase fraction
            retmap[varindex] += diffvalue;
        }
    }

    return retmap;
}

std::map<int,double> CompositionSet::evaluate_single_phase_objective_gradient (
    evalconditions const& conditions, boost::bimap<std::string, int> const &main_indices, double* const x ) const
    {
        std::map<int,double> retmap;
        boost::multi_index::index<ast_set,ast_deriv_order_index>::type::const_iterator ast_begin,ast_end;
        ast_begin = get<ast_deriv_order_index> ( tree_data ).lower_bound ( 1 );
        ast_end = get<ast_deriv_order_index> ( tree_data ).upper_bound ( 1 );
        const std::string compset_name ( cset_name + "_FRAC" );
        
        for ( auto i = main_indices.left.begin(); i != main_indices.left.end(); ++i ) {
            retmap[i->second] = 0; // initialize all indices as zero
        }
        for ( ast_set::const_iterator i = ast_begin; i != ast_end; ++i ) {
            const double diffvalue = process_utree ( i->ast, conditions, main_indices, symbols, x ).get<double>();
            const std::string diffvar = * ( i->diffvars.cbegin() ); // get differentiating variable
            const int varindex = main_indices.left.at ( diffvar );
            retmap[varindex] += diffvalue;
        }
        
        return retmap;
    }

std::vector<double> CompositionSet::evaluate_internal_objective_gradient (
    evalconditions const& conditions, double* const x ) const
{
    std::vector<double> gradient ( phase_indices.size() );
    const double perturbation = 1e-7;
    double x_copy[phase_indices.size()] ;
    for ( auto i = 0; i < phase_indices.size(); ++i ) {
        x_copy[i] = x[i];
    }
    for ( auto i = 0; i < phase_indices.size(); ++i ) {
        double lower_obj, upper_obj;
        x_copy[i] = x[i] - perturbation;
        lower_obj = evaluate_objective ( conditions, phase_indices, x_copy );
        x_copy[i] = x[i] + perturbation;
        upper_obj = evaluate_objective ( conditions, phase_indices, x_copy );
        gradient[i] = ( upper_obj - lower_obj ) / ( 2.0 * perturbation );
    }

    return gradient;
}

std::map<int,double> CompositionSet::evaluate_objective_gradient (
    evalconditions const &conditions, std::map<std::string,double> const &variables ) const
{
    // Need to translate this variable map into something process_utree can understand
    BOOST_LOG_NAMED_SCOPE ( "CompositionSet::evaluate_objective_gradient" );
    logger comp_log ( journal::keywords::channel = "optimizer" );
    BOOST_LOG_SEV ( comp_log, debug ) << "enter";
    double vars[variables.size()]; // Create Ipopt-style double array
    boost::bimap<std::string, int> main_indices;
    typedef boost::bimap<std::string, int>::value_type position;
    for ( auto i = variables.begin(); i != variables.end(); ++i ) {
        vars[std::distance ( variables.begin(),i )] = i->second; // Copy values into array
        main_indices.insert ( position ( i->first, std::distance ( variables.begin(),i ) ) ); // Create fictitious indices
    }
    for ( auto i = main_indices.left.begin(); i != main_indices.left.end(); ++i ) {
        BOOST_LOG_SEV ( comp_log, debug ) << i->first << " -> " << i->second;
    }
    BOOST_LOG_SEV ( comp_log, debug ) << "returning";
    return evaluate_objective_gradient ( conditions, main_indices, vars );
}

std::map<int,double> CompositionSet::evaluate_single_phase_objective_gradient (
    evalconditions const &conditions, std::map<std::string,double> const &variables ) const
    {
        // Need to translate this variable map into something process_utree can understand
        BOOST_LOG_NAMED_SCOPE ( "CompositionSet::evaluate_single_phase_objective_gradient" );
        logger comp_log ( journal::keywords::channel = "optimizer" );
        BOOST_LOG_SEV ( comp_log, debug ) << "enter";
        double vars[variables.size()]; // Create Ipopt-style double array
        boost::bimap<std::string, int> main_indices;
        typedef boost::bimap<std::string, int>::value_type position;
        for ( auto i = variables.begin(); i != variables.end(); ++i ) {
            vars[std::distance ( variables.begin(),i )] = i->second; // Copy values into array
            main_indices.insert ( position ( i->first, std::distance ( variables.begin(),i ) ) ); // Create fictitious indices
        }
        for ( auto i = main_indices.left.begin(); i != main_indices.left.end(); ++i ) {
            BOOST_LOG_SEV ( comp_log, debug ) << i->first << " -> " << i->second;
        }
        BOOST_LOG_SEV ( comp_log, debug ) << "returning";
        return evaluate_single_phase_objective_gradient ( conditions, main_indices, vars );
    }

std::map<std::list<int>,double> CompositionSet::evaluate_objective_hessian (
    evalconditions const& conditions,
    boost::bimap<std::string, int> const &main_indices,
    double* const x ) const
{
    BOOST_LOG_NAMED_SCOPE ( "CompositionSet::evaluate_objective_hessian" );
    logger comp_log ( journal::keywords::channel = "optimizer" );
    std::map<std::list<int>,double> retmap;
    boost::multi_index::index<ast_set,ast_deriv_order_index>::type::const_iterator ast_begin,ast_end;
    ast_begin = get<ast_deriv_order_index> ( tree_data ).lower_bound ( 2 );
    ast_end = get<ast_deriv_order_index> ( tree_data ).upper_bound ( 2 );
    const std::string compset_name ( cset_name + "_FRAC" );

    for ( auto i = main_indices.left.begin(); i != main_indices.left.end(); ++i ) {
        for ( auto j = main_indices.left.begin(); j != main_indices.left.end(); ++j ) {
            if ( i->second > j->second ) {
                continue;    // skip upper triangular
            }
            const std::list<int> searchlist {i->second,j->second};
            retmap[searchlist] = 0; // initialize all indices as zero
        }
    }

    for ( ast_set::const_iterator i = ast_begin; i != ast_end; ++i ) {
        const double diffvalue = process_utree ( i->ast, conditions, main_indices, symbols, x ).get<double>();
        const std::string diffvar1 = * ( i->diffvars.cbegin() );
        const std::string diffvar2 = * ( ++ ( i->diffvars.cbegin() ) );
        const int varindex1 = main_indices.left.at ( diffvar1 );
        const int varindex2 = main_indices.left.at ( diffvar2 );
        std::list<int> searchlist;
        if ( varindex1 <= varindex2 ) searchlist = {varindex1,varindex2};
        else searchlist = {varindex2, varindex1};
        // multiply derivative by phase fraction
        if ( diffvar1 == compset_name || diffvar2 == compset_name ) {
            retmap[searchlist] += diffvalue;
        } else {
            retmap[searchlist] += x[main_indices.left.at ( compset_name )] * diffvalue;
        }
    }
    return retmap;
}

// NOTE: this is explicitly for the single-phase Hessian
boost::numeric::ublas::symmetric_matrix<double,boost::numeric::ublas::lower> CompositionSet::evaluate_objective_hessian_matrix (
    evalconditions const& conditions,
    boost::bimap<std::string, int> const &main_indices,
    std::vector<double> const &x ) const
{
    BOOST_LOG_NAMED_SCOPE ( "CompositionSet::evaluate_objective_hessian_matrix" );
    typedef boost::numeric::ublas::symmetric_matrix<double,boost::numeric::ublas::lower> sym_matrix;
    using boost::numeric::ublas::zero_matrix;
    logger comp_log ( journal::keywords::channel = "optimizer" );
    sym_matrix retmatrix ( zero_matrix<double> ( x.size(),x.size() ) );
    boost::multi_index::index<ast_set,ast_deriv_order_index>::type::const_iterator ast_begin,ast_end;
    ast_begin = get<ast_deriv_order_index> ( tree_data ).lower_bound ( 2 );
    ast_end = get<ast_deriv_order_index> ( tree_data ).upper_bound ( 2 );
    const std::string compset_name ( cset_name + "_FRAC" );


    for ( ast_set::const_iterator i = ast_begin; i != ast_end; ++i ) {
        const std::string diffvar1 = * ( i->diffvars.cbegin() );
        const std::string diffvar2 = * ( ++ ( i->diffvars.cbegin() ) );
        if ( diffvar1 == compset_name || diffvar2 == compset_name ) {
            continue;    // skip phase fraction variable for single-phase calc
        }
        const int varindex1 = main_indices.left.at ( diffvar1 );
        const int varindex2 = main_indices.left.at ( diffvar2 );
        // TODO: const_cast is obviously suboptimal here, but it saves a copy and will do until process_utree is fixed
        const double diffvalue = process_utree ( i->ast, conditions, main_indices, symbols, const_cast<double*> ( &x[0] ) ).get<double>();
        retmatrix ( varindex1,varindex2 ) += diffvalue;
    }
    return retmatrix;
}

std::set<std::list<int>> CompositionSet::hessian_sparsity_structure (
                          boost::bimap<std::string, int> const &main_indices ) const
{
    std::set<std::list<int>> retset;
    boost::multi_index::index<ast_set,ast_deriv_order_index>::type::const_iterator ast_begin,ast_end;
    ast_begin = get<ast_deriv_order_index> ( tree_data ).lower_bound ( 2 );
    ast_end = get<ast_deriv_order_index> ( tree_data ).upper_bound ( 2 );
    for ( ast_set::const_iterator i = ast_begin; i != ast_end; ++i ) {
        const std::string diffvar1 = * ( i->diffvars.cbegin() );
        const std::string diffvar2 = * ( ++ ( i->diffvars.cbegin() ) );
        const int varindex1 = main_indices.left.at ( diffvar1 );
        const int varindex2 = main_indices.left.at ( diffvar2 );
        std::list<int> nonzero_entry;
        if ( varindex1 <= varindex2 ) nonzero_entry = {varindex1,varindex2};
        else nonzero_entry = {varindex2, varindex1};
        retset.insert ( nonzero_entry );
    }
    return retset;
}

// Constructs an orthonormal basis using the linear constraints to generate feasible points
// Reference: Nocedal and Wright, 2006, ch. 15.2, p. 429
void CompositionSet::build_constraint_basis_matrices ( sublattice_set const &sublset )
{
    BOOST_LOG_NAMED_SCOPE ( "CompositionSet::build_constraint_basis_matrices" );
    logger comp_log ( journal::keywords::channel = "optimizer" );
    BOOST_LOG_SEV ( comp_log, debug ) << "enter";
    using namespace boost::numeric::ublas;
    typedef boost::numeric::ublas::matrix<double> ublas_matrix;
    typedef boost::numeric::ublas::vector<double> ublas_vector;
    typedef boost::multi_index::index<sublattice_set,phase_subl>::type::iterator subl_iterator;
    // A is the active linear constraint matrix; satisfies Ax=b
    ublas_matrix Atrans ( zero_matrix<double> ( phase_indices.size(), cm.constraints.size() ) );
    ublas_vector b ( zero_vector<double> ( cm.constraints.size() ) );

    subl_iterator subl_iter = boost::multi_index::get<phase_subl> ( sublset ).lower_bound ( boost::make_tuple ( cset_name,0 ) );
    subl_iterator subl_iter_end = boost::multi_index::get<phase_subl> ( sublset ).upper_bound ( boost::make_tuple ( cset_name,0 ) );
    int sublindex = 0;
    int constraintindex = 0;
    // This is code for handling the sublattice balance constraint
    // TODO: Handle charge balance constraints (relatively straightforward extension once sublattice_entry has charge attribute)
    // This planned extension is why we keep track of the constraint count separately
    while ( subl_iter != subl_iter_end ) {
        // Current sublattice
        std::vector<std::string> subl_list;
        for ( ; subl_iter != subl_iter_end ; ++subl_iter ) {
            const auto variablefind = phase_indices.left.find ( subl_iter->name() );
            if ( variablefind == phase_indices.left.end() ) {
                BOOST_LOG_SEV ( comp_log, critical ) << "Can't find " << subl_iter->name() << "!";
                continue;    // this is bad
            }
            const int variableindex = variablefind->second;
            Atrans ( variableindex,constraintindex ) = 1;
        }
        b ( sublindex ) = 1; // sublattice site fractions must sum to 1
        ++sublindex;
        ++constraintindex;
        subl_iter = boost::multi_index::get<phase_subl> ( sublset ).lower_bound ( boost::make_tuple ( cset_name,sublindex ) );
        subl_iter_end = boost::multi_index::get<phase_subl> ( sublset ).upper_bound ( boost::make_tuple ( cset_name,sublindex ) );
    }

    BOOST_LOG_SEV ( comp_log, debug ) << "Atrans: " << Atrans;
    BOOST_LOG_SEV ( comp_log, debug ) << "b: " << b;
    // Compute the full QR decomposition of Atrans
    std::vector<double> betas = inplace_qr ( Atrans );
    ublas_matrix Q ( zero_matrix<double> ( Atrans.size1(),Atrans.size1() ) );
    ublas_matrix R ( zero_matrix<double> ( Atrans.size1(), Atrans.size2() ) );
    recoverQ ( Atrans, betas, Q, R );
    BOOST_LOG_SEV ( comp_log, debug ) << "Q: " << Q;
    BOOST_LOG_SEV ( comp_log, debug ) << "R: " << R;
    // Copy the last m-n columns of Q into Z (related to the bottom m-n rows of R which should all be zero)
    const std::size_t Zcolumns = Atrans.size1() - Atrans.size2();
    // Copy the rest into Y
    const std::size_t Ycolumns = Atrans.size2();
    constraint_null_space_matrix = zero_matrix<double> ( Atrans.size1(), Zcolumns );
    ublas_matrix Y ( Atrans.size1(), Ycolumns );
    // Z is the submatrix of Q that includes all of Q's rows and its rightmost m-n columns
    constraint_null_space_matrix = subrange ( Q, 0,Atrans.size1(), Atrans.size2(),Atrans.size1() );
    // Y is the remaining columns of Q
    Y = subrange ( Q, 0,Atrans.size1(), 0,Atrans.size2() );
    BOOST_LOG_SEV ( comp_log, debug ) << "Z: " << constraint_null_space_matrix;
    BOOST_LOG_SEV ( comp_log, debug ) << "Y: " << Y;
    
    gradient_projector = ublas_matrix ( prod ( constraint_null_space_matrix,trans ( constraint_null_space_matrix ) ) );

    BOOST_LOG_SEV ( comp_log, debug ) << "exit";
}
