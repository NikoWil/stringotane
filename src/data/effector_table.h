
#ifndef DOMPASCH_TREE_REXX_EFFECTOR_GRAPH_H
#define DOMPASCH_TREE_REXX_EFFECTOR_GRAPH_H

#include <functional>

#include <data/hashmap.h>
#include "data/action.h"
#include "data/reduction.h"
#include "data/signature.h"
#include "util/names.h"

class HtnInstance;

class EffectorTable {

private:
    HtnInstance* _htn;

    // Maps an (action|reduction) name 
    // to the set of (partially lifted) fact signatures
    // that might be added to the state due to this operator. 
    HashMap<int, std::vector<Signature>> _fact_changes; 

public:
    EffectorTable(HtnInstance& htn) : _htn(&htn) {}

    // Maps a (action|reduction) signature of any grounding state
    // to the corresponding set of (partially lifted) fact signatures
    // that might be added to the state due to this operator. 
    std::vector<Signature> getPossibleFactChanges(const Signature& sig);

    std::vector<Signature> getPossibleChildren(const Signature& actionOrReduction);
};


#endif