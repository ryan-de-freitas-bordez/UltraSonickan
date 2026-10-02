#ifndef USK_C_BACKEND_INTERNAL_H
#define USK_C_BACKEND_INTERNAL_H

#include "usk/c_backend.h"

#include <stdarg.h>

typedef enum {
    USK_C_VALUE_UNKNOWN,
    USK_C_VALUE_NULL,
    USK_C_VALUE_BOOL,
    USK_C_VALUE_INT8,
    USK_C_VALUE_INT32,
    USK_C_VALUE_INT64,
    USK_C_VALUE_UINT8,
    USK_C_VALUE_UINT32,
    USK_C_VALUE_UINT64,
    USK_C_VALUE_FLOAT32,
    USK_C_VALUE_FLOAT64,
    USK_C_VALUE_STRING,
    USK_C_VALUE_POINTER
} UskCValueKind;

typedef struct UskCBinding {
    const char *name;
    UskCValueKind kind;
    struct UskCBinding *next;
} UskCBinding;

typedef struct {
    const UskAstProgram *program;
    const char *source_name;
    FILE *output;
    UskCBackendOptions options;
    UskDiagnosticList *diagnostics;
    UskCBackendResult *result;
    const char *current_owner;
    UskCBinding *locals;
    UskCValueKind expected_value_kind;
    UskCValueKind current_return_kind;
    size_t expression_depth;
    size_t indentation;
    bool failed;
} UskCEmitter;

bool usk_c_emit_write(UskCEmitter *emitter, const char *format, ...);
bool usk_c_emit_vwrite(UskCEmitter *emitter, const char *format,
                       va_list arguments);
void usk_c_emit_indent(UskCEmitter *emitter);
void usk_c_emit_error(UskCEmitter *emitter, UskDiagnosticCode code,
                      UskSourceSpan span, const char *format, ...);
void usk_c_emit_unsupported(UskCEmitter *emitter, UskSourceSpan span,
                            const char *feature);
bool usk_c_emit_type(UskCEmitter *emitter, const UskAstType *type);
bool usk_c_emit_expression(UskCEmitter *emitter,
                           const UskAstExpr *expression);
bool usk_c_emit_condition(UskCEmitter *emitter,
                          const UskAstExpr *expression);
bool usk_c_emit_statement(UskCEmitter *emitter,
                          const UskAstStmt *statement);
bool usk_c_emit_declaration(UskCEmitter *emitter,
                            const UskAstDecl *declaration,
                            const char *owner, bool definition);
bool usk_c_emit_runtime_support(UskCEmitter *emitter);
UskCBinding *usk_c_scope_begin(UskCEmitter *emitter);
void usk_c_scope_end(UskCEmitter *emitter, UskCBinding *marker);
bool usk_c_bind_local(UskCEmitter *emitter, const char *name,
                      UskCValueKind kind);
UskCValueKind usk_c_binding_kind(const UskCEmitter *emitter,
                                 const char *name);
UskCValueKind usk_c_value_kind_from_type(const UskAstType *type);
UskCValueKind usk_c_infer_expression_kind(const UskCEmitter *emitter,
                                          const UskAstExpr *expression);
const char *usk_c_value_kind_name(UskCValueKind kind);
char *usk_c_mangle_name(const char *qualified_name);
char *usk_c_join_name(const char *owner, const char *name);
bool usk_c_name_is_entry(const UskCEmitter *emitter,
                         const UskAstDecl *declaration);

#endif
