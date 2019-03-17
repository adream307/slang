//------------------------------------------------------------------------------
// TimingControl.cpp
// Timing control creation and analysis.
//
// File is under the MIT license; see LICENSE for details.
//------------------------------------------------------------------------------
#include "slang/binding/TimingControl.h"

#include "slang/binding/Expressions.h"
#include "slang/compilation/Compilation.h"

namespace slang {

const TimingControl& TimingControl::bind(const TimingControlSyntax& syntax,
                                         const BindContext& context) {
    auto& comp = context.scope.getCompilation();
    TimingControl* result;
    switch (syntax.kind) {
        case SyntaxKind::DelayControl:
            result = &DelayControl::fromSyntax(comp, syntax.as<DelaySyntax>(), context);
            break;
        case SyntaxKind::EventControl:
            result =
                &SignalEventControl::fromSyntax(comp, syntax.as<EventControlSyntax>(), context);
            break;
        case SyntaxKind::EventControlWithExpression: {
            result = &EventListControl::fromSyntax(
                comp, *syntax.as<EventControlWithExpressionSyntax>().expr, context);
            break;
        }
        case SyntaxKind::CycleDelay:
        case SyntaxKind::ImplicitEventControl:
        case SyntaxKind::RepeatedEventControl:
            context.addDiag(DiagCode::NotYetSupported, syntax.sourceRange());
            result = &badCtrl(comp, nullptr);
            break;
        default:
            THROW_UNREACHABLE;
    }

    result->syntax = &syntax;
    return *result;
}

TimingControl& TimingControl::badCtrl(Compilation& compilation, const TimingControl* ctrl) {
    return *compilation.emplace<InvalidTimingControl>(ctrl);
}

TimingControl& DelayControl::fromSyntax(Compilation& compilation, const DelaySyntax& syntax,
                                        const BindContext& context) {
    auto& expr = Expression::bind(*syntax.delayValue, context);
    auto result = compilation.emplace<DelayControl>(expr);
    if (expr.bad())
        return badCtrl(compilation, result);

    if (!expr.type->isNumeric()) {
        context.addDiag(DiagCode::DelayNotNumeric, expr.sourceRange) << *expr.type;
        return badCtrl(compilation, result);
    }

    return *result;
}

TimingControl& SignalEventControl::fromSyntax(Compilation& compilation,
                                              const SignalEventExpressionSyntax& syntax,
                                              const BindContext& context) {
    auto edge = SemanticFacts::getEdgeKind(syntax.edge.kind);
    auto& expr = Expression::bind(*syntax.expr, context);
    return fromExpr(compilation, edge, expr, context);
}

TimingControl& SignalEventControl::fromSyntax(Compilation& compilation,
                                              const EventControlSyntax& syntax,
                                              const BindContext& context) {
    auto& expr = Expression::bind(*syntax.eventName, context);
    return fromExpr(compilation, EdgeKind::None, expr, context);
}

TimingControl& SignalEventControl::fromExpr(Compilation& compilation, EdgeKind edge,
                                            const Expression& expr, const BindContext& context) {
    auto result = compilation.emplace<SignalEventControl>(edge, expr);
    if (expr.bad())
        return badCtrl(compilation, result);

    if (edge == EdgeKind::None) {
        if (expr.type->isAggregate()) {
            context.addDiag(DiagCode::InvalidEventExpression, expr.sourceRange) << *expr.type;
            return badCtrl(compilation, result);
        }
    }
    else if (!expr.type->isIntegral()) {
        context.addDiag(DiagCode::InvalidEdgeEventExpression, expr.sourceRange);
        return badCtrl(compilation, result);
    }

    // Warn if the expression is constant, since it'll never change to trigger off.
    if (expr.constant)
        context.addDiag(DiagCode::EventExpressionConstant, expr.sourceRange);

    return *result;
}

static void collectEvents(const BindContext& context, const EventExpressionSyntax& expr,
                          SmallVector<TimingControl*>& results) {
    switch (expr.kind) {
        case SyntaxKind::ParenthesizedEventExpression:
            collectEvents(context, *expr.as<ParenthesizedEventExpressionSyntax>().expr, results);
            break;
        case SyntaxKind::SignalEventExpression:
            results.append(&SignalEventControl::fromSyntax(
                context.scope.getCompilation(), expr.as<SignalEventExpressionSyntax>(), context));
            break;
        case SyntaxKind::BinaryEventExpression: {
            auto& bin = expr.as<BinaryEventExpressionSyntax>();
            collectEvents(context, *bin.left, results);
            collectEvents(context, *bin.right, results);
            break;
        }
        default:
            THROW_UNREACHABLE;
    }
}

TimingControl& EventListControl::fromSyntax(Compilation& compilation,
                                            const EventExpressionSyntax& syntax,
                                            const BindContext& context) {
    SmallVectorSized<TimingControl*, 4> events;
    collectEvents(context, syntax, events);

    if (events.size() == 1)
        return *events[0];

    auto result = compilation.emplace<EventListControl>(events.copy(compilation));
    for (auto ev : events) {
        if (ev->bad())
            return badCtrl(compilation, result);
    }

    return *result;
}

} // namespace slang