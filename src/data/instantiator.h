
#ifndef DOMPASCH_TREE_REXX_INSTANTIATOR_H
#define DOMPASCH_TREE_REXX_INSTANTIATOR_H

#include <unordered_set>
#include <functional>

#include "parser/cwa.hpp"
#include "data/reduction.h"
#include "data/action.h"
#include "data/code_table.h"
#include "data/signature.h"
#include "util/params.h"

class HtnInstance; // incomplete forward def

struct ArgComparator {
    HtnOp& op;

    ArgComparator(HtnOp& op) : op(op) {}
    bool operator()(const int& a, const int& b) const {
        return rating(a) > rating(b);
    }
    int rating(int arg) const {
        int r = 0;
        for (const Signature& pre : op.getPreconditions()) {
            for (int preArg : pre._usig._args) {
                if (preArg == arg) r++;
            } 
        }
        for (const Signature& eff : op.getEffects()) {
            for (int effArg : eff._usig._args) {
                if (effArg == arg) r++;
            } 
        }
        return r;
    }
};

const int INSTANTIATE_NOTHING = 0;
const int INSTANTIATE_PRECONDITIONS = 1;
const int INSTANTIATE_FULL = 2;

class Instantiator {

private:
    Parameters& _params;
    HtnInstance* _htn;
    int _inst_mode;

public:
    Instantiator(Parameters& params, HtnInstance& htn) : _params(params), _htn(&htn) {
        if (_params.isSet("qq")) {
            _inst_mode = INSTANTIATE_NOTHING;
        } else if (_params.isSet("q")) {
            _inst_mode = INSTANTIATE_PRECONDITIONS;
        } else {
            _inst_mode = INSTANTIATE_FULL;
        }
    }

    std::vector<Reduction> getApplicableInstantiations(const Reduction& r,
            const std::function<bool(const Signature&)>& state, int mode = -1);
    std::vector<Action> getApplicableInstantiations(const Action& a,
            const std::function<bool(const Signature&)>& state, int mode = -1);

    USigSet instantiate(const HtnOp& op, const std::function<bool(const Signature&)>& state);

    HashMap<USignature, std::unordered_set<substitution_t, Substitution::Hasher>, USignatureHasher> 
        getOperationSubstitutionsCausingEffect(
            const std::unordered_set<USignature, USignatureHasher>& operations, 
            const USignature& fact, bool negated);

    bool isFullyGround(const USignature& sig);
    std::vector<int> getFreeArgPositions(const std::vector<int>& sigArgs);
    bool fits(USignature& sig, USignature& groundSig, HashMap<int, int>* substitution);
    bool hasSomeInstantiation(const USignature& sig);

    bool hasConsistentlyTypedArgs(const USignature& sig);
    std::vector<TypeConstraint> getQConstantTypeConstraints(const USignature& sig);

    bool test(const Signature& sig, const std::function<bool(const Signature&)>& state);
    bool hasValidPreconditions(const HtnOp& op, const std::function<bool(const Signature&)>& state);
};


#endif