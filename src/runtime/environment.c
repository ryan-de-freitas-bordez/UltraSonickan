#include "usk/environment.h"
#include "usk/typecheck.h"

#include <stdlib.h>
#include <string.h>

static char *copy_name(const char *name) {
    if (!name) return NULL;
    size_t length = strlen(name);
    if (length == (size_t)-1) return NULL;
    char *copy = (char *)malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, name, length + 1);
    return copy;
}

void environment_init(Environment *environment, Environment *parent) {
    if (!environment) return;
    environment->vars = NULL;
    environment->parent = parent;
}

bool environment_set_parent(Environment *environment, Environment *parent) {
    if (!environment || environment == parent) return false;
    for (Environment *cursor = parent; cursor; cursor = cursor->parent)
        if (cursor == environment) return false;
    environment->parent = parent;
    return true;
}

Environment *environment_root(Environment *environment) {
    if (!environment) return NULL;
    while (environment->parent) environment = environment->parent;
    return environment;
}

size_t environment_depth(const Environment *environment) {
    size_t depth = 0;
    for (; environment && environment->parent; environment = environment->parent)
        depth++;
    return depth;
}

bool environment_is_ancestor(const Environment *ancestor,
                             const Environment *scope) {
    for (; scope; scope = scope->parent)
        if (scope == ancestor) return true;
    return false;
}

Variable *lookup_local_var(Environment *environment, const char *name) {
    if (!environment || !name) return NULL;
    for (Variable *variable = environment->vars; variable; variable = variable->next) {
        if (strcmp(variable->name, name) == 0) return variable;
    }
    return NULL;
}

Variable *lookup_var(Environment *environment, const char *name) {
    if (!name) return NULL;
    for (Environment *scope = environment; scope; scope = scope->parent) {
        Variable *variable = lookup_local_var(scope, name);
        if (variable) return variable;
    }
    return NULL;
}

Variable *lookup_var_with_depth(Environment *environment, const char *name,
                                size_t *depth) {
    size_t current_depth = 0;
    if (depth) *depth = 0;
    if (!name) return NULL;
    for (Environment *scope = environment; scope; scope = scope->parent, current_depth++) {
        Variable *variable = lookup_local_var(scope, name);
        if (variable) {
            if (depth) *depth = current_depth;
            return variable;
        }
    }
    return NULL;
}

size_t environment_binding_count(const Environment *environment) {
    size_t count = 0;
    if (!environment) return count;
    for (const Variable *variable = environment->vars; variable; variable = variable->next)
        count++;
    return count;
}

static UskEnvironmentStatus define_local(Environment *environment,
                                         const char *name, Value value,
                                         bool immutable, const char *type) {
    if (!environment || !name || !name[0])
        return USK_ENV_ASSIGN_INVALID_ARGUMENT;
    char *type_copy = type && type[0] ? copy_name(type) : NULL;
    if (type && type[0] && !type_copy)
        return USK_ENV_ASSIGN_ALLOCATION_FAILURE;
    Variable *existing = lookup_local_var(environment, name);
    if (existing) {
        free(existing->declared_type);
        existing->value = value;
        existing->immutable = immutable;
        existing->declared_type = type_copy;
        return USK_ENV_ASSIGN_OK;
    }

    Variable *variable = (Variable *)calloc(1, sizeof(*variable));
    if (!variable) { free(type_copy); return USK_ENV_ASSIGN_ALLOCATION_FAILURE; }
    variable->name = copy_name(name);
    if (!variable->name) {
        free(type_copy);
        free(variable);
        return USK_ENV_ASSIGN_ALLOCATION_FAILURE;
    }
    variable->value = value;
    variable->immutable = immutable;
    variable->declared_type = type_copy;
    variable->next = environment->vars;
    environment->vars = variable;
    return USK_ENV_ASSIGN_OK;
}

UskEnvironmentStatus define_var(Environment *environment, const char *name,
                                Value value, bool immutable) {
    return define_local(environment, name, value, immutable, NULL);
}

UskEnvironmentStatus define_typed_var(Environment *environment,
                                      const char *name, Value value,
                                      bool immutable, const char *type) {
    return define_local(environment, name, value, immutable, type);
}

UskEnvironmentStatus assign_var(Environment *environment, const char *name,
                                Value value) {
    Variable *variable = lookup_var(environment, name);
    if (!variable) return USK_ENV_ASSIGN_UNDEFINED;
    if (variable->immutable) return USK_ENV_ASSIGN_IMMUTABLE;
    if (variable->declared_type &&
        !value_matches_type(variable->declared_type, value))
        return USK_ENV_ASSIGN_TYPE_MISMATCH;
    variable->value = value;
    return USK_ENV_ASSIGN_OK;
}

UskEnvironmentStatus assign_local_var(Environment *environment, const char *name,
                                      Value value) {
    Variable *variable = lookup_local_var(environment, name);
    if (!variable) return USK_ENV_ASSIGN_UNDEFINED;
    if (variable->immutable) return USK_ENV_ASSIGN_IMMUTABLE;
    if (variable->declared_type &&
        !value_matches_type(variable->declared_type, value))
        return USK_ENV_ASSIGN_TYPE_MISMATCH;
    variable->value = value;
    return USK_ENV_ASSIGN_OK;
}

bool remove_local_var(Environment *environment, const char *name) {
    if (!environment || !name) return false;
    Variable **link = &environment->vars;
    while (*link) {
        Variable *variable = *link;
        if (strcmp(variable->name, name) == 0) {
            *link = variable->next;
            free(variable->name);
            free(variable->declared_type);
            free(variable);
            return true;
        }
        link = &variable->next;
    }
    return false;
}

bool environment_contains(Environment *environment, const char *name) {
    return lookup_var(environment, name) != NULL;
}

const Variable *environment_binding_at(const Environment *environment,
                                       size_t index) {
    if (!environment) return NULL;
    const Variable *variable = environment->vars;
    while (variable && index) { variable = variable->next; index--; }
    return variable;
}

void environment_iterator_begin(const Environment *environment,
                                UskEnvironmentIterator *iterator) {
    if (!iterator) return;
    iterator->scope = environment;
    iterator->next = environment ? environment->vars : NULL;
}

const Variable *environment_iterator_next(UskEnvironmentIterator *iterator,
                                          size_t *scope_depth) {
    if (!iterator) return NULL;
    while (iterator->scope && !iterator->next) {
        iterator->scope = iterator->scope->parent;
        if (scope_depth) (*scope_depth)++;
        iterator->next = iterator->scope ? iterator->scope->vars : NULL;
    }
    if (!iterator->scope) return NULL;
    const Variable *result = iterator->next;
    iterator->next = result->next;
    return result;
}

bool environment_validate(const Environment *environment) {
    const Environment *slow = environment;
    const Environment *fast = environment;
    while (fast && fast->parent) {
        slow = slow->parent;
        fast = fast->parent->parent;
        if (slow == fast) return false;
    }
    for (const Environment *scope = environment; scope; scope = scope->parent) {
        for (const Variable *left = scope->vars; left; left = left->next) {
            if (!left->name || !left->name[0]) return false;
            for (const Variable *right = left->next; right; right = right->next)
                if (strcmp(left->name, right->name) == 0) return false;
        }
    }
    return true;
}

bool environment_has_local_name(const Environment *environment,
                                const char *name) {
    return name && lookup_local_var((Environment *)environment, name) != NULL;
}

const char *environment_local_type(const Environment *environment,
                                   const char *name) {
    Variable *variable = name
        ? lookup_local_var((Environment *)environment, name) : NULL;
    return variable ? variable->declared_type : NULL;
}

bool environment_local_is_immutable(const Environment *environment,
                                    const char *name) {
    Variable *variable = name
        ? lookup_local_var((Environment *)environment, name) : NULL;
    return variable && variable->immutable;
}

bool environment_local_get(const Environment *environment, const char *name,
                           Value *value) {
    Variable *variable = name
        ? lookup_local_var((Environment *)environment, name) : NULL;
    if (!variable || !value) return false;
    *value = variable->value;
    return true;
}

size_t environment_total_binding_count(const Environment *environment) {
    size_t count = 0;
    for (const Environment *scope = environment; scope; scope = scope->parent) {
        size_t local = environment_binding_count(scope);
        if (local > (size_t)-1 - count) return (size_t)-1;
        count += local;
    }
    return count;
}

bool environment_validate_chain(const Environment *environment,
                                size_t maximum_depth,
                                size_t *observed_depth) {
    size_t depth = environment_depth(environment);
    if (observed_depth) *observed_depth = depth;
    return depth <= maximum_depth && environment_validate(environment);
}

void environment_iterator_begin_chain(const Environment *environment,
                                      UskEnvironmentIterator *iterator) {
    if (!iterator) return;
    iterator->scope = environment;
    iterator->next = environment ? environment->vars : NULL;
}

const Variable *environment_iterator_next_local(
    UskEnvironmentIterator *iterator) {
    if (!iterator || !iterator->scope || !iterator->next) return NULL;
    const Variable *result = iterator->next;
    iterator->next = result->next;
    return result;
}

UskEnvironmentStatus environment_define_local_copy(
    Environment *environment, const Variable *source) {
    if (!source || !source->name) return USK_ENV_ASSIGN_INVALID_ARGUMENT;
    return define_local(environment, source->name, source->value,
                        source->immutable, source->declared_type);
}

bool environment_remove_all_locals(Environment *environment) {
    if (!environment) return false;
    bool removed = environment->vars != NULL;
    Variable *variable = environment->vars;
    while (variable) {
        Variable *next = variable->next;
        free(variable->name);
        free(variable->declared_type);
        free(variable);
        variable = next;
    }
    environment->vars = NULL;
    return removed;
}

const Variable *environment_find_const(const Environment *environment,
                                       const char *name, size_t *scope_depth) {
    size_t depth = 0;
    if (scope_depth) *scope_depth = 0;
    if (!name) return NULL;
    for (const Environment *scope = environment; scope;
         scope = scope->parent, ++depth) {
        for (const Variable *variable = scope->vars; variable;
             variable = variable->next) {
            if (!strcmp(variable->name, name)) {
                if (scope_depth) *scope_depth = depth;
                return variable;
            }
        }
    }
    return NULL;
}

bool environment_visit(const Environment *environment,
                       UskEnvironmentVisitor visitor, void *context) {
    if (!visitor) return false;
    size_t depth = 0;
    for (const Environment *scope = environment; scope;
         scope = scope->parent, ++depth) {
        for (const Variable *variable = scope->vars; variable;
             variable = variable->next) {
            if (!visitor(context, variable, depth)) return false;
        }
    }
    return true;
}

UskEnvironmentStatus environment_copy_visible(
    const Environment *source, Environment *destination) {
    if (!source || !destination || destination->vars ||
        !environment_validate(source) || !environment_validate(destination) ||
        environment_is_ancestor(destination, source))
        return USK_ENV_ASSIGN_INVALID_ARGUMENT;
    size_t depth = environment_depth(source);
    if (depth == (size_t)-1 || depth + 1 > (size_t)-1 / sizeof(Environment *))
        return USK_ENV_ASSIGN_ALLOCATION_FAILURE;
    size_t count = depth + 1;
    const Environment **chain = (const Environment **)malloc(
        count * sizeof(*chain));
    if (!chain) return USK_ENV_ASSIGN_ALLOCATION_FAILURE;
    const Environment *scope = source;
    for (size_t index = 0; index < count; ++index) {
        chain[index] = scope;
        scope = scope->parent;
    }
    UskEnvironmentStatus status = USK_ENV_ASSIGN_OK;
    while (count && status == USK_ENV_ASSIGN_OK) {
        const Environment *frame = chain[--count];
        for (const Variable *variable = frame->vars; variable;
             variable = variable->next) {
            status = environment_define_local_copy(destination, variable);
            if (status != USK_ENV_ASSIGN_OK) break;
        }
    }
    free(chain);
    if (status != USK_ENV_ASSIGN_OK)
        environment_remove_all_locals(destination);
    return status;
}

void environment_destroy(Environment *environment) {
    if (!environment) return;
    Variable *variable = environment->vars;
    while (variable) {
        Variable *next = variable->next;
        free(variable->name);
        free(variable->declared_type);
        free(variable);
        variable = next;
    }
    environment->vars = NULL;
    environment->parent = NULL;
}
