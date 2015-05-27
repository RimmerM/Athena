
#include "resolve.h"

namespace athena {
namespace resolve {

template<class F>
TypeRef Resolver::mapType(F&& f, TypeRef type) {
	switch(type->kind) {
		case Type::Alias:
			return mapType(f, ((AliasType*)type)->target);
		case Type::Tuple: {
			auto t = (TupleType*)type;
			auto fields = t->fields;
			for(auto& field : fields) {
				field.type = f(field.type);
			}
			return types.getTuple(fields);
		}
		case Type::Var: {
			/*auto t = (VarType*)type;
			auto cs = t->list;
			for(auto& c : cs) {
				for(auto& i : c.contents) {
					i = f(i);
				}
			}*/
			break;
		}
		case Type::Array:
			// TODO
			break;
		case Type::Map:
			// TODO
			break;
		case Type::Lvalue:
			return types.getLV(f(((LVType*)type)->type));
		case Type::Gen:
			return f(type);
		case Type::App:
			return f(type);
		case Type::Ptr:
			return types.getPtr(f(((PtrType*)type)->type));
	}

	return type;
}

TypeRef Resolver::resolveAlias(Scope& scope, AliasType* type) {
	ASSERT(type->astDecl);
	auto target = resolveType(scope, type->astDecl->target, false, type->astDecl->type);
	type->astDecl = nullptr;
	return target;
}

TypeRef Resolver::resolveVariant(Scope& scope, VarType* type) {
	// Resolve each declared constructor.
	for(auto& c : type->list) {
		ast::walk(c->astDecl, [&](auto i) {
			c->contents += this->resolveType(scope, i, false, type->astDecl->type);
		});
		c->astDecl = nullptr;
	}
	type->astDecl = nullptr;
	return type;
}

TypeRef Resolver::resolveTuple(Scope& scope, ast::TupleType& type, ast::SimpleType* tscope) {
	// Generate a hash for the fields.
	Core::Hasher h;
	ast::walk(type.fields, [&](auto i) {
		auto t = this->resolveType(scope, i.type, false, tscope);
		h.Add(t);
		// Include the name to ensure that different tuples with the same memory layout are not exactly the same.
		if(i.name) h.Add(i.name());
	});

	// Check if this kind of tuple has been used already.
	TupleType* result = nullptr;
	if(!types.getTuple(h, result)) {
		// Otherwise, create the type.
		new (result) TupleType;
		uint i = 0;
		bool resolved = true;
		ast::walk(type.fields, [&](auto it) {
			auto t = this->resolveType(scope, it.type, false, tscope);
			if(!t->resolved) resolved = false;
			result->fields += Field{it.name ? it.name() : 0, i, t, result, nullptr, true};
			i++;
		});

		result->resolved = resolved;
	}

	return result;
}

inline Maybe<uint> getGenIndex(ast::SimpleType& type, Id name) {
	auto t = type.kind;
	uint i = 0;
	while(t) {
		if(t->item == name) return i;
		i++;
		t = t->next;
	}

	return Nothing;
}

TypeRef Resolver::resolveType(ScopeRef scope, ast::TypeRef type, bool constructor, ast::SimpleType* tscope) {
	// Check if this is a primitive type.
	if(type->kind == ast::Type::Unit) {
		return types.getUnit();
	} else if(type->kind == ast::Type::Ptr) {
		ast::Type t{ast::Type::Con, type->con};
		return types.getPtr(resolveType(scope, &t, constructor, tscope));
	} else if(type->kind == ast::Type::Tup) {
		return resolveTuple(scope, *(ast::TupleType*)type, tscope);
	} else if(type->kind == ast::Type::Gen) {
		if(tscope) {
			auto i = getGenIndex(*tscope, type->con);
			if(i) {
				auto g = build<GenType>(i());
				g->resolved = false;
				return g;
			}
		}

		error("undefined generic type");
		return types.getUnknown();
	} else if(type->kind == ast::Type::App) {
		// Find the base type and instantiate it for these arguments.
		auto atype = (ast::AppType*)type;
		auto base = resolveType(scope, atype->base, constructor, tscope);
		if(base->isGeneric()) {
			return build<AppType>(((GenType*)base)->index, atype->apps);
		} else {
			return instantiateType(scope, base, atype->apps, tscope);
		}
	} else {
		// Check if this type has been defined in this scope.
		if(constructor) {
			if (auto t = scope.findConstructor(type->con))
				return t->type;

			// TODO: This is kind of a hack.
			// The Bool primitive type has separate constructors.
			auto name = context.Find(type->con).name;
			if(name == "True" || name == "False") {
				return types.getBool();
			} else if(name == "Bool") {
				// The Bool name is not a constructor.
				error("'Bool' cannot be used as a constructor; use True or False instead");
			} else {
				// Check if this is a primitive type.
				if (auto t = types.primMap.Get(type->con)) return *t;
			}
		} else {
			if (auto t = scope.findType(type->con)) return t;
			// Check if this is a primitive type.
			if (auto t = types.primMap.Get(type->con)) return *t;
		}

		return types.getUnknown();
	}
}

TypeRef Resolver::instantiateType(ScopeRef scope, TypeRef base, ast::TypeList* apps, ast::SimpleType* tscope) {
	if(base->isAlias()) {
		if(ast::count(apps) != ((AliasType*)base)->generics) {
			error("number of generics in the type must be equal to the amount applied");
			return base;
		}

		TypeList list;
		ast::walk(apps, [&](auto i) { list += this->resolveType(scope, i, false, tscope); });

		return mapType([&](TypeRef t) {
			switch (t->kind) {
				case Type::Gen:
					return (TypeRef)list[((GenType*)t)->index];
				case Type::App:
					return instantiateType(scope, list[((AppType*)t)->baseIndex], ((AppType*)t)->apps, tscope);
				default:
					return t;
			}
		}, base);
	} else {
		error("must be a generic type");
		return base;
	}
}

}} // namespace athena::resolve
