
#include <iostream>
#include <functional>
#include <regex>
#include <string>

#include "parser/plan.hpp"

#include "planner.h"
#include "util/log.h"

int Planner::findPlan() {
    
    int iteration = 0;
    log("ITERATION %i\n", iteration);

    createFirstLayer();

    // Bounds on depth to solve / explore
    int firstSatCallIteration = _params.getIntParam("d");
    int maxIterations = _params.getIntParam("D");

    bool solved = false;
    if (iteration >= firstSatCallIteration) {
        _enc.addAssumptions(_layer_idx);
        solved = _enc.solve();
    } 
    
    // Next layers
    while (!solved && (maxIterations == 0 || iteration < maxIterations)) {

        if (iteration >= firstSatCallIteration) {

            _enc.printFailedVars(_layers.back());

            if (_params.isSet("cs")) { // check solvability
                log("Unsolvable at layer %i with assumptions\n", _layer_idx);

                // Attempt to solve formula again, now without assumptions
                // (is usually simple; if it fails, we know the entire problem is unsolvable)
                solved = _enc.solve();
                if (!solved) {
                    log("Unsolvable at layer %i even without assumptions!\n", _layer_idx);
                    break;
                } else {
                    log("Solvable without assumptions - expanding by another layer\n");
                }
            } else {
                log("Unsolvable at layer %i -- expanding.\n", _layer_idx);
            }
        }

        iteration++;      
        log("ITERATION %i\n", iteration);
        
        createNextLayer();

        if (iteration >= firstSatCallIteration) {
            _enc.addAssumptions(_layer_idx);
            solved = _enc.solve();
        } 
    }

    if (!solved) {
        if (iteration >= firstSatCallIteration) _enc.printFailedVars(_layers.back());
        log("No success. Exiting.\n");
        return 1;
    }

    log("Found a solution at layer %i.\n", _layers.size()-1);

    outputPlan();

    _enc.printStages();
    //_enc.printSatisfyingAssignment();
    
    return 0;
}

void Planner::outputPlan() {

    // Extract solution
    auto planPair = _enc.extractPlan();

    // Create stringstream which is being fed the plan
    std::stringstream stream;

    // Print plan into stream

    // -- primitive part
    stream << "==>\n";
    FlatHashSet<int> actionIds;
    FlatHashSet<int> idsToRemove;
    for (PlanItem& item : planPair.first) {
        if (item.id < 0) continue;
        if (_htn._name_back_table[item.abstractTask._name_id].rfind("_SECOND") != std::string::npos) {
            // Second part of a split action: discard
            idsToRemove.insert(item.id);
            continue;
        }
        if (_htn._name_back_table[item.abstractTask._name_id].rfind("_FIRST") != std::string::npos) {
            // First part of a split action: change name, then handle normally
            item.abstractTask._name_id = _htn._split_action_from_first[item.abstractTask._name_id];
        }
        actionIds.insert(item.id);

        // Do not write blank actions or the virtual goal action
        if (item.abstractTask == _htn._action_blank.getSignature()) continue;
        if (item.abstractTask._name_id == _htn.getNameId("_GOAL_ACTION_")) continue;

        stream << item.id << " " << Names::to_string_nobrackets(_htn.cutNonoriginalTaskArguments(item.abstractTask)) << "\n";
    }
    // -- decomposition part
    bool root = true;
    for (PlanItem& item : planPair.second) {
        if (item.id < 0) continue;

        std::string subtaskIdStr = "";
        for (int subtaskId : item.subtaskIds) {
            if (!idsToRemove.count(subtaskId)) subtaskIdStr += " " + std::to_string(subtaskId);
        }
        
        if (root) {
            stream << "root " << subtaskIdStr << "\n";
            root = false;
            continue;
        } else if (item.id <= 0 || actionIds.count(item.id)) continue;
        
        stream << item.id << " " << Names::to_string_nobrackets(_htn.cutNonoriginalTaskArguments(item.abstractTask)) << " -> " 
            << Names::to_string_nobrackets(item.reduction) << subtaskIdStr << "\n";
    }
    stream << "<==\n";

    // Feed plan into parser to convert it into a plan to the original problem
    // (w.r.t. previous compilations the parser did) and output it
    std::ostringstream outstream;
    convert_plan(stream, outstream);
    log(outstream.str().c_str());
    log("<==\n");
    
    log("End of solution plan.\n");
}

void Planner::createFirstLayer() {

    // Initial layer of size 2 (top level reduction + goal action)
    int initSize = 2;
    log("Creating initial layer of size %i\n", initSize);
    _layer_idx = 0;
    _pos = 0;
    _layers.push_back(Layer(0, initSize));
    Layer& initLayer = _layers[0];
    initLayer[_pos].setPos(_layer_idx, _pos);
    
    /***** LAYER 0, POSITION 0 ******/

    // Initial state
    SigSet initState = _htn.getInitState();
    for (const Signature& fact : initState) {
        initLayer[_pos].addFact(fact._usig);
        initLayer[_pos].addDefinitiveFact(fact);
        getLayerState().add(_pos, fact);
    }

    // Instantiate all possible init. reductions
    _htn._init_reduction_choices = _instantiator.getApplicableInstantiations(
            _htn._init_reduction, getStateEvaluator());
    std::vector<Reduction> roots = _htn._init_reduction_choices;
    for (const Reduction& r : roots) {

        Reduction red = _htn.replaceQConstants(r, _layer_idx, _pos, getStateEvaluator());
        USignature sig = red.getSignature();
        
        // Check validity
        if (!_instantiator.hasConsistentlyTypedArgs(sig)) continue;
        if (!_instantiator.hasValidPreconditions(red.getPreconditions(), getStateEvaluator())) continue;
        
        // Remove unneeded rigid conditions from the reduction
        _htn.removeRigidConditions(red);

        _htn._reductions_by_sig[sig] = red;

        assert(_instantiator.isFullyGround(sig));
        initLayer[_pos].addReduction(sig);
        initLayer[_pos].addAxiomaticOp(sig);
        initLayer[_pos].addExpansionSize(red.getSubtasks().size());
        // Add preconditions
        for (const Signature& fact : red.getPreconditions()) {
            addPrecondition(sig, fact);
        }
        addQConstantTypeConstraints(sig);
    }
    addNewFalseFacts();
    _enc.encode(_layer_idx, _pos++);

    /***** LAYER 0, POSITION 1 ******/

    createNextPosition(); // position 1

    // Create virtual goal action
    Action goalAction(_htn.getNameId("_GOAL_ACTION_"), std::vector<int>());
    USignature goalSig = goalAction.getSignature();
    _htn._actions[goalSig._name_id] = goalAction;
    initLayer[_pos].addAction(goalSig);
    initLayer[_pos].addAxiomaticOp(goalSig);
    
    // Extract primitive goals, add to preconds of goal action
    SigSet goalSet = _htn.getGoals();
    for (const Signature& fact : goalSet) {
        assert(getLayerState().contains(_pos, fact));
        assert(initLayer[_pos].hasFact(fact.getUnsigned()));
        goalAction.addPrecondition(fact);
        addPrecondition(goalSig, fact);
    }
    _htn._actions_by_sig[goalSig] = goalAction;
    
    _enc.encode(_layer_idx, _pos++);

    /***** LAYER 0 END ******/

    initLayer.consolidate();
}

void Planner::createNextLayer() {

    _layers.emplace_back(_layers.size(), _layers.back().getNextLayerSize());
    Layer& newLayer = _layers.back();
    log(" NEW_LAYER_SIZE %i\n", newLayer.size());
    Layer& oldLayer = _layers[_layer_idx];
    _layer_idx++;
    _pos = 0;

    for (_old_pos = 0; _old_pos < oldLayer.size(); _old_pos++) {
        int newPos = oldLayer.getSuccessorPos(_old_pos);
        int maxOffset = oldLayer[_old_pos].getMaxExpansionSize();

        for (int offset = 0; offset < maxOffset; offset++) {
            assert(_pos == newPos + offset);
            log(" Position (%i,%i)\n", _layer_idx, _pos);
            log("  Instantiating ...\n");

            //log("%i,%i,%i,%i\n", oldPos, newPos, offset, newLayer.size());
            assert(newPos+offset < newLayer.size());

            createNextPosition();
            log("  Instantiation done. (r=%i a=%i f=%i qf=%i)\n", 
                    _layers[_layer_idx][_pos].getReductions().size(),
                    _layers[_layer_idx][_pos].getActions().size(),
                    _layers[_layer_idx][_pos].getFacts().size(),
                    _layers[_layer_idx][_pos].getNumQFacts()
            );
            _enc.encode(_layer_idx, _pos++);
        }
    }

    newLayer.consolidate();
}

void Planner::createNextPosition() {

    // Set up all facts that may hold at this position.
    if (_pos == 0) {
        propagateInitialState();
    } else {
        Position& left = _layers[_layer_idx][_pos-1];
        createNextPositionFromLeft(left);
    }

    // Generate this new position's content based on the facts and the position above.
    if (_layer_idx > 0) {
        Position& above = _layers[_layer_idx-1][_old_pos];
        createNextPositionFromAbove(above);
    }

    // In preparation for the upcoming position,
    // add all effects of the actions and reductions occurring HERE
    // as (initially false) facts to THIS position.  
    addNewFalseFacts();
}

void Planner::createNextPositionFromAbove(const Position& above) {
    Position& newPos = _layers[_layer_idx][_pos];
    newPos.setPos(_layer_idx, _pos);

    int offset = _pos - _layers[_layer_idx-1].getSuccessorPos(_old_pos);
    if (offset == 0) {
        // Propagate facts
        for (const auto& entry : above.getQFacts()) for (const USignature& fact : entry.second) {
            newPos.addQFact(fact);
        }
        for (const USignature& fact : above.getFacts()) {
            //assert(newPos.getFacts().count(entry.first)); // already added as false fact!
            newPos.addFact(fact);
        }
    }

    propagateActions(offset);
    propagateReductions(offset);
}

void Planner::createNextPositionFromLeft(const Position& left) {
    Position& newPos = _layers[_layer_idx][_pos];
    newPos.setPos(_layer_idx, _pos);
    assert(left.getPos() == IntPair(_layer_idx, _pos-1));

    // Propagate state
    //newPos.extendState(left.getState());

    FlatHashSet<int> relevantQConstants;

    // Propagate fact changes from operations from previous position
    for (const USignature& aSig : left.getActions()) {
        for (const Signature& fact : left.getFactChanges(aSig)) {
            addEffect(aSig, fact);
        }
        for (const int& arg : aSig._args) {
            if (_htn._q_constants.count(arg)) relevantQConstants.insert(arg);
        }
    }
    for (const USignature& rSig : left.getReductions()) {
        if (rSig == Position::NONE_SIG) continue;
        for (const Signature& fact : left.getFactChanges(rSig)) {
            addEffect(rSig, fact);
        }
        for (const int& arg : rSig._args) {
            if (_htn._q_constants.count(arg)) relevantQConstants.insert(arg);
        }
    }

    // Propagate occurring facts
    for (const USignature& fact : left.getFacts()) newPos.addFact(fact);
    for (const auto& entry : left.getQFacts()) for (const USignature& fact : entry.second) {
        bool add = true;
        for (const int& arg : fact._args) {
            if (_htn._q_constants.count(arg) && !relevantQConstants.count(arg)) add = false;
        }
        if (!add) {
            // forget q-facts that have become irrelevant
            //log("  FORGET %s\n", Names::to_string(entry.first).c_str());
            getLayerState().withdraw(_pos, fact, true);
            getLayerState().withdraw(_pos, fact, false);
            continue;
        }
        newPos.addQFact(fact);
    }
}

void Planner::addPrecondition(const USignature& op, const Signature& fact) {
    Position& pos = _layers[_layer_idx][_pos];
    const USignature& factAbs = fact.getUnsigned();

    bool isQFact = _htn.hasQConstants(factAbs);

    if (fact._negated && !isQFact && !pos.hasFact(factAbs)) {
        // Negative precondition not contained in facts: initialize
        //log("NEG_PRE %s\n", Names::to_string(fact).c_str());
        introduceNewFalseFact(pos, factAbs);
    }
    
    //log("pre %s of %s\n", Names::to_string(fact).c_str(), Names::to_string(op).c_str());
    // Precondition must be valid (or a q fact)
    if (!isQFact) assert(getLayerState().contains(_pos, fact) 
            || fail(Names::to_string(fact) + " not contained in state!\n"));

    // Add additional reason for the fact / add it first if it's a q-constant
    if (isQFact) pos.addQFact(factAbs);
    else pos.addFact(factAbs);

    // For each fact decoded from the q-fact:
    assert(!isQFact || !_htn.getDecodedObjects(factAbs).empty());
    for (const USignature& decFactAbs : _htn.getDecodedObjects(factAbs)) {
        Signature decFact(decFactAbs, fact._negated);
        
        if (!_instantiator.test(decFact, getStateEvaluator())) {
            // Fact cannot be true here
            pos.addForbiddenSubstitution(op, factAbs._args, decFactAbs._args);
            continue;
        }

        if (!pos.hasFact(decFactAbs)) {
            // Decoded fact did not occur before.
            introduceNewFalseFact(pos, decFactAbs);
        }

        _htn.addQFactDecoding(factAbs, decFactAbs);
        pos.addFact(decFactAbs); // also add fact as an (indirect) consequence of op
    }
}

void Planner::addEffect(const USignature& opSig, const Signature& fact) {
    Position& pos = _layers[_layer_idx][_pos];
    assert(_pos > 0);
    Position& left = _layers[_layer_idx][_pos-1];
    //Position& left = _layers[_layer_idx][_pos-1];
    USignature factAbs = fact.getUnsigned();
    bool isQFact = _htn.hasQConstants(factAbs);
    if (isQFact) pos.addQFact(factAbs);
    else pos.addFact(factAbs);

    // Depending on whether fact supports are encoded for primitive ops only,
    // add the fact to the op's support accordingly
    if (_params.isSet("nps") || _htn._actions_by_sig.count(opSig)) {
        pos.addFactSupport(fact, opSig);
    } else {
        // Remember that there is some (unspecified) support for this fact
        pos.touchFactSupport(fact);
    }
    
    getLayerState().add(_pos, fact);

    if (!isQFact) return;

    //log("Calc decoded effects for %s:%s\n", Names::to_string(opSig).c_str(), Names::to_string(fact).c_str());

    HtnOp& op = _htn.getOp(opSig);
    assert(!_htn.getDecodedObjects(factAbs).empty());
    int numValid = 0;
    for (const USignature& decFactAbs : _htn.getDecodedObjects(factAbs)) {
        
        /*
        // Check if the preconditions of op subject to such a fact encoding can hold
        const auto& subs = _instantiator.getOperationSubstitutionsCausingEffect(left.getFactChanges(opSig), decFactAbs, fact._negated);
        bool valid = false;
        // For each possible substitution leading to the fact:
        for (const auto& sub : subs) {
            HtnOp opSub = op.substitute(sub);

            // Try to get a single valid (w.r.t. preconds) instantiation of the operator
            std::vector<int> varArgs;
            for (const int& arg : opSub.getArguments()) {
                if (_htn._var_ids.count(arg)) varArgs.push_back(arg);
            }
            USigSet inst = _instantiator.instantiateLimited(opSub, getStateEvaluator(_layer_idx, _pos-1), varArgs, 1, true);

            if (!inst.empty()) {
                // Success -- the effect may actually happen
                valid = true;
                break;
            }
        }
        if (!valid) continue;
        */
        
        Signature decFact(decFactAbs, fact._negated);
        _htn.addQFactDecoding(factAbs, decFactAbs);
        getLayerState().add(_pos, decFact);
        numValid++;
    }

    if (numValid == 0) {
        //log("No valid decoded effects for %s!\n", Names::to_string(opSig).c_str());
    }
}

void Planner::propagateInitialState() {
    assert(_layer_idx > 0);
    assert(_pos == 0);

    Position& newPos = _layers[_layer_idx][0];
    Position& above = _layers[_layer_idx-1][0];

    // Propagate occurring facts
    for (const USignature& fact : above.getFacts()) {
        newPos.addFact(fact);
    }
    for (const auto& entry : above.getQFacts()) for (const USignature& fact : entry.second) {
        newPos.addQFact(fact);
    }
    // Propagate TRUE facts
    for (const USignature& fact : above.getTrueFacts())
        newPos.addTrueFact(fact);
    for (const USignature& fact : above.getFalseFacts())
        newPos.addFalseFact(fact);

    // Propagate state: initial position and all q-facts
    getLayerState(_layer_idx) = //LayerState(getLayerState(_layer_idx-1), _layers[_layer_idx-1].getOffsets());
    LayerState();
    const auto& oldState = getLayerState(_layer_idx-1);
    auto& newState = getLayerState(_layer_idx);
    for (bool neg : {true, false}) {
        for (const auto& entry : neg ? oldState.getNegFactOccurrences() : oldState.getPosFactOccurrences()) {
            const USignature& fact = entry.first;
            //log("  ~~~> %s\n", Names::to_string(fact).c_str());
            const auto& range = entry.second;
            if (range.first == 0 || _htn.hasQConstants(fact)) {
                int newRangeFirst = _layers[_layer_idx-1].getSuccessorPos(range.first);
                newState.add(newRangeFirst, fact, neg);
                if (range.second != INT32_MAX) {
                    int newRangeSecond = _layers[_layer_idx-1].getSuccessorPos(range.second);    
                    newState.withdraw(newRangeSecond, fact, neg);
                }
            }
        }
    }
    log("%i neg, %i pos ~~~> %i neg, %i pos\n", oldState.getNegFactOccurrences().size(), oldState.getPosFactOccurrences().size(), 
                                                newState.getNegFactOccurrences().size(), newState.getPosFactOccurrences().size());
    
}

void Planner::propagateActions(int offset) {
    Position& newPos = _layers[_layer_idx][_pos];
    Position& above = _layers[_layer_idx-1][_old_pos];

    // Propagate actions
    for (const USignature& aSig : above.getActions()) {
        if (aSig == Position::NONE_SIG) continue;
        const Action& a = _htn._actions_by_sig[aSig];

        // Can the action occur here w.r.t. the current state?
        bool valid = _instantiator.hasValidPreconditions(a.getPreconditions(), getStateEvaluator());

        // If not: forbid the action, i.e., its parent action
        if (!valid) {
            //log("FORBIDDING action %s@(%i,%i): no children at offset %i\n", 
            //        Names::to_string(aSig).c_str(), _layer_idx-1, _old_pos, offset);
            newPos.addExpansion(aSig, Position::NONE_SIG);
            continue;
        }

        if (offset < 1) {
            // proper action propagation
            assert(_instantiator.isFullyGround(aSig));
            newPos.addAction(aSig);
            newPos.addExpansion(aSig, aSig);
            // Add preconditions of action
            for (const Signature& fact : a.getPreconditions()) {
                addPrecondition(aSig, fact);
            }
        } else {
            // action expands to "blank" at non-zero offsets
            USignature blankSig = _htn._action_blank.getSignature();
            newPos.addAction(blankSig);
            newPos.addExpansion(aSig, blankSig);
        }
    }
}

void Planner::propagateReductions(int offset) {
    Position& newPos = _layers[_layer_idx][_pos];
    Position& above = _layers[_layer_idx-1][_old_pos];

    // Expand reductions
    for (const USignature& rSig : above.getReductions()) {
        if (rSig == Position::NONE_SIG) continue;
        const Reduction r = _htn._reductions_by_sig[rSig];
        
        int numAdded = 0;
        if (offset < r.getSubtasks().size()) {
            // Proper expansion
            const USignature& subtask = r.getSubtasks()[offset];
            // reduction(s)?
            for (const USignature& subRSig : getAllReductionsOfTask(subtask, getStateEvaluator())) {
                numAdded++;
                assert(_htn._reductions_by_sig.count(subRSig));
                const Reduction& subR = _htn._reductions_by_sig[subRSig];

                assert(subRSig == _htn._reductions_by_sig[subRSig].getSignature());
                assert(_instantiator.isFullyGround(subRSig));
                
                newPos.addReduction(subRSig);
                newPos.addExpansion(rSig, subRSig);
                //if (_layer_idx <= 1) log("ADD %s:%s @ (%i,%i)\n", Names::to_string(subR.getTaskSignature()).c_str(), Names::to_string(subRSig).c_str(), _layer_idx, _pos);
                newPos.addExpansionSize(subR.getSubtasks().size());
                // Add preconditions of reduction
                //log("PRECONDS %s ", Names::to_string(subRSig).c_str());
                for (const Signature& fact : subR.getPreconditions()) {
                    addPrecondition(subRSig, fact);
                    //log("%s ", Names::to_string(fact).c_str());
                }
                addQConstantTypeConstraints(subRSig);
                //log("\n");
            }
            // action(s)?
            for (const USignature& aSig : getAllActionsOfTask(subtask, getStateEvaluator())) {
                numAdded++;
                assert(_instantiator.isFullyGround(aSig));
                newPos.addAction(aSig);
                newPos.addExpansion(rSig, aSig);
                // Add preconditions of action
                const Action& a = _htn._actions_by_sig[aSig];
                for (const Signature& fact : a.getPreconditions()) {
                    addPrecondition(aSig, fact);
                }
                addQConstantTypeConstraints(aSig);
            }
        } else {
            // Blank
            numAdded++;
            USignature blankSig = _htn._action_blank.getSignature();
            newPos.addAction(blankSig);
            newPos.addExpansion(rSig, blankSig);
        }

        if (numAdded == 0) {
            // Explicitly forbid the parent!
            //log("FORBIDDING reduction %s@(%i,%i): no children at offset %i\n", 
            //        Names::to_string(rSig).c_str(), _layer_idx-1, _old_pos, offset);
            newPos.addExpansion(rSig, Position::NONE_SIG);
        }
    }
}

std::vector<USignature> Planner::getAllActionsOfTask(const USignature& task, std::function<bool(const Signature&)> state) {
    std::vector<USignature> result;

    if (!_htn._actions.count(task._name_id)) return result;

    // Check if the supplied task has consistent sorts
    //if (!_instantiator.hasConsistentlyTypedArgs(task)) return result;

    const Action& a = _htn._actions[task._name_id];

    HtnOp op = a.substitute(Substitution(a.getArguments(), task._args));
    Action act = (Action) op;
    //log("  task %s : action found: %s\n", Names::to_string(task).c_str(), Names::to_string(act).c_str());
    
    std::vector<Action> actions = _instantiator.getApplicableInstantiations(act, state);
    for (Action& action : actions) {
        USignature sig = action.getSignature();

        //if (!_instantiator.hasConsistentlyTypedArgs(sig)) continue;

        // Rename any remaining variables in each action as unique q-constants,
        action = _htn.replaceQConstants(action, _layer_idx, _pos, getStateEvaluator());

        // Remove any inconsistent effects that were just created
        action.removeInconsistentEffects();

        // Check validity
        if (action.getSignature() != task) continue;
        if (!_instantiator.isFullyGround(action.getSignature())) continue;
        if (!_instantiator.hasConsistentlyTypedArgs(sig)) continue;
        if (!_instantiator.hasValidPreconditions(action.getPreconditions(), state)) continue;
        
        // Remove unneeded rigid conditions from the reduction
        _htn.removeRigidConditions(action);

        sig = action.getSignature();
        _htn._actions_by_sig[sig] = action;
        result.push_back(sig);
    }
    return result;
}

std::vector<USignature> Planner::getAllReductionsOfTask(const USignature& task, std::function<bool(const Signature&)> state) {
    std::vector<USignature> result;

    if (!_htn._task_id_to_reduction_ids.count(task._name_id)) return result;

    // Check if the created reduction has consistent sorts
    //if (!_instantiator.hasConsistentlyTypedArgs(task)) return result;

    const std::vector<int>& redIds = _htn._task_id_to_reduction_ids[task._name_id];
    log("  task %s : %i reductions found\n", Names::to_string(task).c_str(), redIds.size());

    // Filter and minimally instantiate methods
    // applicable in current (super)state
    for (int redId : redIds) {
        Reduction r = _htn._reductions[redId];
        log("%s %s\n", Names::to_string(r.getTaskSignature()).c_str(), Names::to_string(r.getSignature()).c_str());
        std::vector<Substitution> subs = Substitution::getAll(r.getTaskArguments(), task._args);
        for (const Substitution& s : subs) {
            for (const auto& entry : s) assert(entry.second != 0);

            //if (_layer_idx <= 1) log("SUBST %s\n", Names::to_string(s).c_str());
            Reduction rSub = r.substituteRed(s);
            USignature origSig = rSub.getSignature();
            if (!_instantiator.hasConsistentlyTypedArgs(origSig)) continue;
            
            //log("   reduction %s ~> %i instantiations\n", Names::to_string(origSig).c_str(), reductions.size());
            std::vector<Reduction> reductions = _instantiator.getApplicableInstantiations(rSub, state);
            for (Reduction& red : reductions) {
                USignature sig = red.getSignature();

                // Rename any remaining variables in each action as new, unique q-constants 
                red = _htn.replaceQConstants(red, _layer_idx, _pos, getStateEvaluator());
                
                // Check validity
                if (red.getTaskSignature() != task) continue;
                if (!_instantiator.isFullyGround(red.getSignature())) continue;
                if (!_instantiator.hasConsistentlyTypedArgs(sig)) continue;
                if (!_instantiator.hasValidPreconditions(red.getPreconditions(), state)) continue;
                
                // Remove unneeded rigid conditions from the reduction
                _htn.removeRigidConditions(red);
                
                sig = red.getSignature();
                _htn._reductions_by_sig[sig] = red;
                result.push_back(sig);
            }
        }
    }
    return result;
}

void Planner::addNewFalseFacts() {
    Position& newPos = _layers[_layer_idx][_pos];
    
    // For each action:
    for (const USignature& aSig : newPos.getActions()) {
        newPos.setFactChanges(aSig, _instantiator.getAllFactChanges(aSig));
        
        for (const Signature& eff : newPos.getFactChanges(aSig)) {

            if (!_htn.hasQConstants(eff._usig) && !newPos.hasFact(eff._usig)) {
                // New fact: set to false before the action may happen
                introduceNewFalseFact(newPos, eff._usig);
            }

            for (const USignature& decEff : _htn.getDecodedObjects(eff._usig)) {
                if (!newPos.hasFact(decEff)) {
                    // New fact: set to false before the action may happen
                    introduceNewFalseFact(newPos, decEff);
                }
            }
        }
    }

    // For each possible reduction effect: 
    for (const USignature& rSig : newPos.getReductions()) {
        if (rSig == Position::NONE_SIG) continue;
        newPos.setFactChanges(rSig, _instantiator.getAllFactChanges(rSig));

        for (const Signature& eff : newPos.getFactChanges(rSig)) {

            if (!_htn.hasQConstants(eff._usig) && !newPos.getFacts().count(eff._usig)) {
                // New fact: set to false before the reduction may happen
                introduceNewFalseFact(newPos, eff._usig);
            }

            for (const USignature& decEff : _htn.getDecodedObjects(eff._usig)) {
                if (!newPos.hasFact(decEff)) {
                    // New fact: set to false before the action may happen
                    introduceNewFalseFact(newPos, decEff);
                }
            }
        }
    }

    // For each fact from "above" the next position:
    if (_layer_idx == 0) return;
    if (_old_pos+1 < _layers[_layer_idx-1].size() && _layers[_layer_idx-1].getSuccessorPos(_old_pos+1) == _pos+1) {
        Position& newAbove = _layers[_layer_idx-1][_old_pos+1];

        for (const USignature& fact : newAbove.getFacts()) {
            // If fact was not seen here before
            if (!newPos.hasFact(fact)) {
                // Add fact
                newPos.addFact(fact);
                introduceNewFalseFact(newPos, fact);
            }
        }
        for (const auto& entry : newAbove.getQFacts()) for (const USignature& fact : entry.second) {
            // If fact was not seen here before
            if (!newPos.hasQFact(fact)) {
                newPos.addQFact(fact);
            }
        }
    }
}

void Planner::introduceNewFalseFact(Position& newPos, const USignature& fact) {
    Signature sig(fact, /*negated=*/true);
    assert(!_htn.hasQConstants(fact));
    newPos.addDefinitiveFact(sig);
    newPos.addFact(fact);
    getLayerState(newPos.getPos().first).add(newPos.getPos().second, sig);
    //log("FALSE_FACT %s @(%i,%i)\n", Names::to_string(fact.abs().opposite()).c_str(), 
    //        newPos.getPos().first, newPos.getPos().second);
}

void Planner::addQConstantTypeConstraints(const USignature& op) {
    // Add type constraints for q constants
    std::vector<TypeConstraint> cs = _instantiator.getQConstantTypeConstraints(op);
    // Add to this position's data structure
    for (const TypeConstraint& c : cs) {
        _layers[_layer_idx][_pos].addQConstantTypeConstraint(op, c);
    }
}

LayerState& Planner::getLayerState(int layer) {
    if (layer == -1) layer = _layer_idx;
    return _layers[layer].getState();
}

std::function<bool(const Signature&)> Planner::getStateEvaluator(int layer, int pos) {
    if (layer == -1) layer = _layer_idx;
    if (pos == -1) pos = _pos;
    return [this,layer,pos](const Signature& sig) {
        return getLayerState(layer).contains(pos, sig);
    };
}
