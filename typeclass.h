#ifndef TYPECLASS_H
#define TYPECLASS_H

#include <stdbool.h>
#include <llvm-c/Core.h>
#include "reader.h"
#include "types.h"

/// Forward declarations
typedef struct CodegenContext CodegenContext;

/// Type Class Method Signature
//
// Represents a single method declared in a class, e.g.
//   (=) :: a -> a -> Bool
//
typedef struct TCMethod {
    char *name;      // "=", "!="
    char *type_str;  // "a -> a -> Bool"
} TCMethod;

/// Type Class Declaration
//
// Stores the class name, type variable, method signatures,
// and any default implementations provided in the class body.
//
typedef struct TCClass {
    char     *name;            // "Eq"
    char     *type_var;        // "a"
    TCMethod *methods;         // method signatures
    int       method_count;
    char    **default_names;   // methods with default impls
    AST     **default_bodies;  // lambda ASTs for defaults
    int       default_count;
} TCClass;

/// Type Class Instance
//
// Stores a concrete instantiation of a class for a specific type,
// e.g. (instance Eq TrafficLight where ...).
// The dict_global is an LLVM global holding a struct of function pointers —
// one per method — that implements the class for this type.
//
typedef struct TCInstance {
    char         *class_name;    // "Eq"
    char         *type_name;     // "TrafficLight"
    LLVMValueRef  dict_global;   // @__dict_Eq_TrafficLight global
    char        **method_names;  // method names in this instance
    LLVMValueRef *method_funcs;  // LLVM functions implementing each method
    int           method_count;
} TCInstance;

/// Type Class Registry
//
// Central registry holding all known classes and instances.
// One registry lives on the CodegenContext and persists across
// REPL modules for the lifetime of the compiler session.
//
typedef struct TypeClassRegistry {
    TCClass    *classes;
    int         class_count;
    int         class_cap;
    TCInstance *instances;
    int         instance_count;
    int         instance_cap;
} TypeClassRegistry;

/// Lifecycle

TypeClassRegistry *tc_registry_create(void);
void               tc_registry_free(TypeClassRegistry *reg);

/// Class registration
//
// Called when an AST_CLASS node is encountered during codegen.
// Records the class, its methods, and any default implementations.
//
void tc_register_class(TypeClassRegistry *reg, AST *class_decl);

/// Instance registration
//
// Called when an AST_INSTANCE node is encountered during codegen.
// Generates the LLVM dictionary struct and registers the instance.
//
void tc_register_instance(TypeClassRegistry *reg, AST *instance_decl,
                          CodegenContext *ctx);

/// Lookup

// Find a class by name, NULL if not found.
TCClass *tc_find_class(TypeClassRegistry *reg, const char *class_name);

// Find an instance for a specific class+type pair, NULL if not found.
TCInstance *tc_find_instance(TypeClassRegistry *reg, const char *class_name,
                             const char *type_name);

/// Method dispatch
//
// Returns the LLVM function implementing method_name for the given type.
// Looks up the instance, finds the method in the dictionary.
// Returns NULL if no instance is registered for this class+type.
//
LLVMValueRef tc_get_method(TypeClassRegistry *reg, const char *class_name,
                            const char *type_name, const char *method_name,
                            CodegenContext *ctx);

// Returns the dictionary global for a class+type pair.
LLVMValueRef tc_get_dict(TypeClassRegistry *reg, const char *class_name,
                         const char *type_name, CodegenContext *ctx);


/// Method introspection
//
// These let codegen.c check whether a symbol being called is actually
// a type class method, and if so which class it belongs to.
//

// Returns true if method_name is declared in any registered class.
bool tc_is_method(TypeClassRegistry *reg, const char *method_name);

// Returns the class name that owns method_name, NULL if not a method.
const char *tc_method_class(TypeClassRegistry *reg, const char *method_name);

/// Dictionary type generation
//
// Returns (or creates) the LLVM struct type for a class's dictionary.
// e.g. for Eq: { ptr eq_fn, ptr neq_fn }
//
LLVMTypeRef tc_dict_type(TypeClassRegistry *reg, const char *class_name,
                         CodegenContext *ctx);

/// Dictionary name helpers

// Returns the mangled name for a class dictionary global.
// e.g. tc_dict_name("Eq", "TrafficLight") -> "__dict_Eq_TrafficLight"
void tc_dict_name(const char *class_name, const char *type_name,
                  char *out, size_t out_size);

// Returns the mangled name for an instance method function.
// e.g. "__impl_Eq_TrafficLight_eq"
void tc_method_name(const char *class_name, const char *type_name,
                    const char *method_name, char *out, size_t out_size);

#endif
