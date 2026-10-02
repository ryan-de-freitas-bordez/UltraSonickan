#ifndef USK_EVALUATOR_INTERNAL_H
#define USK_EVALUATOR_INTERNAL_H

#include "usk/evaluator.h"
#include "usk/environment.h"
#include "usk/typecheck.h"

typedef enum {
    USK_FLOW_NORMAL,
    USK_FLOW_RETURN,
    USK_FLOW_BREAK,
    USK_FLOW_CONTINUE
} UskFlowKind;

typedef struct {
    UskFlowKind kind;
    Value value;
} UskFlow;

typedef struct UskRuntimeFunction {
    const UskAstDecl *declaration;
    const char *qualified_name;
    struct UskRuntimeFunction *next;
} UskRuntimeFunction;

typedef struct {
    const UskAstProgram *program;
    const char *source_name;
    UskDiagnosticList *diagnostics;
    UskEvaluatorOptions options;
    Environment globals;
    UskValueArena *value_storage;
    UskRuntimeFunction *functions;
    size_t functions_registered;
    size_t declarations_evaluated;
    size_t call_depth;
    size_t loop_depth;
    size_t switch_depth;
    bool failed;
} UskEvaluator;

UskFlow usk_flow_normal(void);
UskFlow usk_flow_make(UskFlowKind kind, Value value);
void usk_eval_error(UskEvaluator *evaluator, UskDiagnosticCode code,
                    UskSourceSpan span, const char *format, ...);
Value usk_eval_expression(UskEvaluator *evaluator, Environment *environment,
                          const UskAstExpr *expression);
UskFlow usk_eval_statement(UskEvaluator *evaluator, Environment *environment,
                           const UskAstStmt *statement);
Value usk_default_value_for_type(UskEvaluator *evaluator,
                                 const UskAstType *type);
Value usk_eval_call(UskEvaluator *evaluator, Environment *environment,
                    const UskAstExpr *callee, const Value *arguments,
                    size_t argument_count, UskSourceSpan span);
UskRuntimeFunction *usk_find_runtime_function(UskEvaluator *evaluator,
                                              const char *name);
Value usk_call_function(UskEvaluator *evaluator,
                        UskRuntimeFunction *function,
                        const Value *arguments,
                        size_t argument_count,
                        UskSourceSpan call_span);

#endif
