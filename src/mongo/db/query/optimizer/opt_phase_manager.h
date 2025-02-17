/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <unordered_set>

#include "mongo/db/query/optimizer/cascades/interfaces.h"
#include "mongo/db/query/optimizer/cascades/logical_rewriter.h"
#include "mongo/db/query/optimizer/cascades/physical_rewriter.h"
#include "mongo/db/query/optimizer/reference_tracker.h"


namespace mongo::optimizer {

using namespace cascades;

/**
 * This class wraps together different optimization phases.
 * First the transport rewrites are applied such as constant folding and redundant expression
 * elimination. Second the logical and physical reordering rewrites are applied using the memo.
 * Third the final transport rewritesd are applied.
 */

#define OPT_PHASE(F)                                                                               \
    /* ConstEval performs the following rewrites: constant folding, inlining, and dead code        \
     * elimination. */                                                                             \
    F(ConstEvalPre)                                                                                \
    F(PathFuse)                                                                                    \
                                                                                                   \
    /* Memo phases below perform Cascades-style optimization. Reorder and transform nodes. Convert \
     * Filter and Eval nodes to SargableNodes, and possibly merge them.*/                          \
    F(MemoSubstitutionPhase)                                                                       \
    /* Performs Local-global and rewrites to enable index intersection. If there is an             \
     * implementation phase, it runs integrated with the top-down optimization. If there is no     \
     * implementation phase, it runs standalone.*/                                                 \
    F(MemoExplorationPhase)                                                                        \
    /* Implementation and enforcement rules. */                                                    \
    F(MemoImplementationPhase)                                                                     \
                                                                                                   \
    F(PathLower)                                                                                   \
    F(ConstEvalPost)

MAKE_PRINTABLE_ENUM(OptPhase, OPT_PHASE);
MAKE_PRINTABLE_ENUM_STRING_ARRAY(OptPhaseEnum, OptPhase, OPT_PHASE);
#undef OPT_PHASE

class OptPhaseManager {
public:
    using PhaseSet = opt::unordered_set<OptPhase>;

    OptPhaseManager(PhaseSet phaseSet,
                    PrefixId& prefixId,
                    bool requireRID,
                    Metadata metadata,
                    std::unique_ptr<CardinalityEstimator> explorationCE,
                    std::unique_ptr<CardinalityEstimator> substitutionCE,
                    std::unique_ptr<CostEstimator> costEstimator,
                    PathToIntervalFn pathToInterval,
                    ConstFoldFn constFold,
                    bool supportExplain,
                    DebugInfo debugInfo,
                    QueryHints queryHints = {});

    // We only allow moving.
    OptPhaseManager(const OptPhaseManager& /*other*/) = delete;
    OptPhaseManager(OptPhaseManager&& /*other*/) = default;
    OptPhaseManager& operator=(const OptPhaseManager& /*other*/) = delete;
    OptPhaseManager& operator=(OptPhaseManager&& /*other*/) = delete;

    /**
     * Optimization modifies the input argument.
     * If there is a failure, program will tassert.
     */
    void optimize(ABT& input);

    /**
     * Similar to optimize, but returns a bool to indicate success or failure. True means success;
     * false means failure.
     */
    [[nodiscard]] bool optimizeNoAssert(ABT& input);

    static const PhaseSet& getAllRewritesSet();

    MemoPhysicalNodeId getPhysicalNodeId() const;
    const boost::optional<ABT>& getPostMemoPlan() const;

    const QueryHints& getHints() const;
    QueryHints& getHints();

    const Memo& getMemo() const;

    const PathToIntervalFn& getPathToInterval() const;

    const Metadata& getMetadata() const;

    const NodeToGroupPropsMap& getNodeToGroupPropsMap() const;
    NodeToGroupPropsMap& getNodeToGroupPropsMap();

private:
    bool hasPhase(OptPhase phase) const;

    template <OptPhase phase, class C>
    void runStructuralPhase(C instance, VariableEnvironment& env, ABT& input);

    /**
     * Run two structural phases until mutual fixpoint.
     * We assume we can construct from the types by initializing with env.
     */
    template <const OptPhase phase1, const OptPhase phase2, class C1, class C2>
    void runStructuralPhases(C1 instance1, C2 instance2, VariableEnvironment& env, ABT& input);

    void runMemoLogicalRewrite(OptPhase phase,
                               VariableEnvironment& env,
                               const LogicalRewriter::RewriteSet& rewriteSet,
                               GroupIdType& rootGroupId,
                               bool runStandalone,
                               std::unique_ptr<LogicalRewriter>& logicalRewriter,
                               ABT& input);

    [[nodiscard]] bool runMemoPhysicalRewrite(OptPhase phase,
                                              VariableEnvironment& env,
                                              GroupIdType rootGroupId,
                                              std::unique_ptr<LogicalRewriter>& logicalRewriter,
                                              ABT& input);

    [[nodiscard]] bool runMemoRewritePhases(VariableEnvironment& env, ABT& input);


    static PhaseSet _allRewrites;

    const PhaseSet _phaseSet;

    /**
     * True if we should maintain extra internal state in support of explain.
     */
    const bool _supportExplain;

    const DebugInfo _debugInfo;

    QueryHints _hints;

    Metadata _metadata;

    /**
     * Final state of the memo after physical rewrites are complete.
     */
    Memo _memo;

    /**
     * Logical properties derivation implementation.
     */
    std::unique_ptr<LogicalPropsInterface> _logicalPropsDerivation;

    /**
     * Cardinality estimation implementation to be used during the exploraton phase..
     */
    std::unique_ptr<CardinalityEstimator> _explorationCE;

    /**
     * Cardinality estimation implementation to be used during the substitution phase.
     *
     * The substitution phase typically doesn't care about CE, because it doesn't generate/compare
     * alternatives. Since some CE implementations are expensive (sampling), we let the caller pass
     * a different one for this phase.
     */
    std::unique_ptr<CardinalityEstimator> _substitutionCE;

    /**
     * Cost derivation implementation.
     */
    std::unique_ptr<CostEstimator> _costEstimator;

    /**
     * Path ABT node to index bounds converter implementation.
     */
    PathToIntervalFn _pathToInterval;

    /**
     * Constant fold an expression.
     */
    ConstFoldFn _constFold;

    /**
     * Root physical node if we have performed physical rewrites.
     */
    MemoPhysicalNodeId _physicalNodeId;

    /**
     * Post memo exploration phase plan (set if '_supportExplain' is set and if we have performed
     * memo rewrites).
     */
    boost::optional<ABT> _postMemoPlan;

    /**
     * Map from node to logical and physical properties.
     */
    NodeToGroupPropsMap _nodeToGroupPropsMap;

    /**
     * Used to optimize update and delete statements. If set will include indexing requirement with
     * seed physical properties.
     */
    const bool _requireRID;

    /**
     * RID projection names we have generated for each scanDef. Used for physical rewriting.
     */
    RIDProjectionsMap _ridProjections;

    // We don't own this.
    PrefixId& _prefixId;
};

}  // namespace mongo::optimizer
