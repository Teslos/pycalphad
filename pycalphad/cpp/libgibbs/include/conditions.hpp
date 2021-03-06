/*=============================================================================
	Copyright (c) 2012-2013 Richard Otis

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

// header file for thermodynamic state variable declaration

#ifndef INCLUDED_CONDITIONS
#define INCLUDED_CONDITIONS
#include <map>
#include <vector>
#include <cstdint>
#include "libgibbs/include/optimizer/phasestatus.hpp"

struct evalconditions { 
std::map<char,double> statevars; // state variable values
std::vector<std::string> elements; // elements under consideration
std::map<std::string,Optimizer::PhaseStatus> phases; // phases under consideration
std::map<std::string,double> xfrac; // system mole fractions
};

//#define SI_GAS_CONSTANT 8.3144621 // J/mol-K; the 2010 CODATA recommended value for molar gas constant
constexpr const double SI_GAS_CONSTANT = 8.3145; // J/mol-K; Thermo-Calc value
#endif
