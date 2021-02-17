
#include <assert.h>
#include <set>
#include <algorithm>

#include "algo/instantiator.h"
#include "algo/arg_iterator.h"
#include "data/htn_instance.h"
#include "util/names.h"

USigSet Instantiator::EMPTY_USIG_SET;

std::vector<Reduction> Instantiator::getApplicableInstantiations(const Reduction& r, int mode) {

    int oldMode = _inst_mode;
    if (mode >= 0) _inst_mode = mode;

    std::vector<Reduction> result;

    for (const USignature& sig : instantiate(r)) {
        //log("%s\n", TOSTR(sig));
        result.push_back(r.substituteRed(Substitution(r.getArguments(), sig._args)));
    }
    _inst_mode = oldMode;

    return result;
}

std::vector<Action> Instantiator::getApplicableInstantiations(const Action& a, int mode) {

    int oldMode = _inst_mode;
    if (mode >= 0) _inst_mode = mode;

    std::vector<Action> result;

    for (const USignature& sig : instantiate(a)) {
        //log("%s\n", TOSTR(sig));
        //assert(isFullyGround(sig) || Log::e("%s is not fully ground!\n", TOSTR(sig)));
        HtnOp newOp = a.substitute(Substitution(a.getArguments(), sig._args));
        result.push_back((Action&&) std::move(newOp));
    }

    _inst_mode = oldMode;

    return result;
}

const HtnOp* __op;
struct CompArgs {
    bool operator()(const int& a, const int& b) const {
        return rating(a) > rating(b);
    }
    int rating(int arg) const {
        int r = 0;
        for (const Signature& pre : __op->getPreconditions()) {
            for (int preArg : pre._usig._args) {
                if (preArg == arg) r++;
            } 
        }
        for (const Signature& eff : __op->getEffects()) {
            for (int effArg : eff._usig._args) {
                if (effArg == arg) r++;
            } 
        }
        return r;
    }
};

USigSet Instantiator::instantiate(const HtnOp& op) {
    __op = &op;

    /*
    // First try to naively ground the operation up to some limit
    FlatHashSet<int> argsToInstantiate;
    if (_inst_mode == INSTANTIATE_FULL) {
        for (const int& arg : op.getArguments()) {
            if (_htn->isVariable(arg)) argsToInstantiate.insert(arg);
        }
    }
    std::vector<int> argsByPriority(argsToInstantiate.begin(), argsToInstantiate.end());
    std::sort(argsByPriority.begin(), argsByPriority.end(), CompArgs());
    */
    std::vector<int> argsByPriority;

    // a) Try to naively ground _one single_ instantiation
    // -- if this fails, there is no valid instantiation at all
    USigSet inst = instantiateLimited(op, argsByPriority, 1, /*returnUnfinished=*/true);
    if (inst.empty()) return inst;

    // b) Try if the number of valid instantiations is below the user-defined threshold
    //    -- in that case, return that full instantiation
    if (_q_const_instantiation_limit > 0) {
        USigSet inst = instantiateLimited(op, argsByPriority, _q_const_instantiation_limit, /*returnUnfinished=*/false);
        if (!inst.empty()) return inst;
    }
    
    return instantiateLimited(op, argsByPriority, 0, false);

    /*
    // Collect all arguments which should be instantiated
    FlatHashSet<int> argsToInstantiate;

    // a) All variable args according to the q-constant policy
    for (size_t i = 0; i < op.getArguments().size(); i++) {
        const int& arg = op.getArguments().at(i);
        if (!_htn->isVariable(arg)) continue;

        if (_inst_mode == INSTANTIATE_FULL) {

            argsToInstantiate.insert(arg);

        } else if (_inst_mode == INSTANTIATE_PRECONDITIONS) {
    
            bool found = false;
            for (const auto& pre : op.getPreconditions()) {
                for (const int& preArg : pre._usig._args) {
                    if (arg == preArg) {
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }

            if (found) argsToInstantiate.insert(arg);
        }
    }

    // b) All variable args whose domain is below the specified q constant threshold
    if (_q_const_rating_factor > 0) {
        const auto& ratings = getPreconditionRatings(op.getSignature());
        if (_inst_mode != INSTANTIATE_FULL)
        for (size_t argIdx = 0; argIdx < op.getArguments().size(); argIdx++) {
            int arg = op.getArguments().at(argIdx);
            if (!_htn->isVariable(arg)) continue;

            int sort = _htn->getSorts(op.getNameId()).at(argIdx);
            int domainSize = _htn->getConstantsOfSort(sort).size();
            float r = ratings.at(arg);
            if (_q_const_rating_factor*r > domainSize) {
                argsToInstantiate.insert(arg);
            }
        }
    }

    // Sort args to instantiate by their priority descendingly
    argsByPriority = std::vector<int>(argsToInstantiate.begin(), argsToInstantiate.end());
    std::sort(argsByPriority.begin(), argsByPriority.end(), CompArgs());

    return instantiateLimited(op, state, argsByPriority, 0, false);
    */
}

USigSet Instantiator::instantiateLimited(const HtnOp& op, const std::vector<int>& argsByPriority, 
        size_t limit, bool returnUnfinished) {

    USigSet instantiation;
    size_t doneInstSize = argsByPriority.size();
    
    if (doneInstSize == 0) {
        if (_analysis.hasValidPreconditions(op.getPreconditions()) 
            && _analysis.hasValidPreconditions(op.getExtraPreconditions()) 
            && _htn.hasSomeInstantiation(op.getSignature())) 
            instantiation.insert(op.getSignature());
        //log("INST %s : %i instantiations X\n", TOSTR(op.getSignature()), instantiation.size());
        return instantiation;
    }

    // Create back transformation of argument positions
    FlatHashMap<int, int> argPosBackMapping;
    for (size_t j = 0; j < argsByPriority.size(); j++) {
        for (size_t i = 0; i < op.getArguments().size(); i++) {
            if (op.getArguments()[i] == argsByPriority[j]) {
                argPosBackMapping[j] = i;
                break;
            }
        }   
    }

    std::vector<std::vector<int>> assignmentsStack;
    assignmentsStack.push_back(std::vector<int>()); // begin with empty assignment
    while (!assignmentsStack.empty()) {
        const std::vector<int> assignment = assignmentsStack.back();
        assignmentsStack.pop_back();
        //for (int a : assignment) log("%i ", a); log("\n");

        // Loop over possible choices for the next argument position
        int argPos = argPosBackMapping[assignment.size()];
        int sort = _htn.getSorts(op.getNameId()).at(argPos);
        for (int c : _htn.getConstantsOfSort(sort)) {

            // Create new assignment
            std::vector<int> newAssignment(assignment);
            newAssignment.push_back(c);

            // Create corresponding op
            Substitution s;
            for (size_t i = 0; i < newAssignment.size(); i++) {
                assert(i < argsByPriority.size());
                s[argsByPriority[i]] = newAssignment[i];
            }
            HtnOp newOp = op.substitute(s);

            // Test validity
            if (!_analysis.hasValidPreconditions(newOp.getPreconditions())
                || !_analysis.hasValidPreconditions(newOp.getExtraPreconditions())) continue;

            // All ok -- add to stack
            if (newAssignment.size() == doneInstSize) {
                // If there are remaining variables: 
                // is there some valid constant for each of them?
                if (!_htn.hasSomeInstantiation(newOp.getSignature())) continue;

                // This instantiation is finished:
                // Assemble instantiated signature
                instantiation.insert(newOp.getSignature());

                if (limit > 0) {
                    if (returnUnfinished && instantiation.size() == limit) {
                        // Limit exceeded -- return unfinished instantiation
                        return instantiation;
                    }
                    if (!returnUnfinished && instantiation.size() > limit) {
                        // Limit exceeded -- return failure
                        return USigSet();
                    }
                }

            } else {
                // Unfinished instantiation
                assignmentsStack.push_back(newAssignment);
            }
        }
    }

    __op = NULL;

    //log("INST %s : %i instantiations\n", TOSTR(op.getSignature()), instantiation.size());
    return instantiation;
}

const FlatHashMap<int, float>& Instantiator::getPreconditionRatings(const USignature& opSig) {

    int nameId = opSig._name_id;
    
    // Substitution mapping
    std::vector<int> placeholderArgs;
    USignature normSig = _htn.getNormalizedLifted(opSig, placeholderArgs);
    Substitution sFromPlaceholder(placeholderArgs, opSig._args);

    if (!_precond_ratings.count(nameId)) {
        // Compute
        NodeHashMap<int, std::vector<float>> ratings;
        NodeHashMap<int, std::vector<int>> numRatings;
        
        NetworkTraversal(_htn).traverse(normSig, NetworkTraversal::TRAVERSE_PREORDER, [&](const USignature& nodeSig, int depth) {

            HtnOp op = (_htn.isAction(nodeSig) ? 
                        (HtnOp)_htn.toAction(nodeSig._name_id, nodeSig._args) : 
                        (HtnOp)_htn.toReduction(nodeSig._name_id, nodeSig._args));
            int numPrecondArgs = 0;
            int occs = 0;
            for (size_t i = 0; i < normSig._args.size(); i++) {
                int opArg = opSig._args[i];
                int normArg = normSig._args[i];
                if (!_htn.isVariable(opArg)) continue;
                
                ratings[opArg];
                numRatings[opArg];
                while ((size_t)depth >= ratings[opArg].size()) {
                    ratings[opArg].push_back(0);
                    numRatings[opArg].push_back(0);
                }

                for (const Signature& pre : op.getPreconditions()) for (const int& preArg : pre._usig._args) {
                    if (normArg == preArg) occs++;
                    numPrecondArgs++;
                }

                ratings[opArg][depth] += (numPrecondArgs > 0) ? (float)occs / numPrecondArgs : 0;
                numRatings[opArg][depth]++;
            }
        });

        _precond_ratings[nameId];
        for (const auto& entry : ratings) {
            const int& arg = entry.first;
            _precond_ratings[nameId][arg] = 0;
            for (size_t depth = 0; depth < entry.second.size(); depth++) {
                const float& r = entry.second[depth];
                const int& numR = numRatings[arg][depth];
                if (numR > 0) _precond_ratings[nameId][arg] += 1.0f/(1 << depth) * r/numR;
            }
        }
    }

    return _precond_ratings.at(nameId);
}
