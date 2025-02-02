// Copyright (C) 2020 Free Software Foundation, Inc.

// This file is part of GCC.

// GCC is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3, or (at your option) any later
// version.

// GCC is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// for more details.

// You should have received a copy of the GNU General Public License
// along with GCC; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

#include "rust-hir-type-check.h"
#include "rust-hir-full.h"
#include "rust-hir-type-check-toplevel.h"
#include "rust-hir-type-check-item.h"
#include "rust-hir-type-check-expr.h"
#include "rust-hir-type-check-struct-field.h"

extern bool
saw_errors (void);

namespace Rust {
namespace Resolver {

void
TypeResolution::Resolve (HIR::Crate &crate)
{
  for (auto it = crate.items.begin (); it != crate.items.end (); it++)
    TypeCheckTopLevel::Resolve (it->get ());

  if (saw_errors ())
    return;

  for (auto it = crate.items.begin (); it != crate.items.end (); it++)
    TypeCheckItem::Resolve (it->get ());

  if (saw_errors ())
    return;

  auto mappings = Analysis::Mappings::get ();
  auto context = TypeCheckContext::get ();

  context->iterate ([&] (HirId id, TyTy::BaseType *ty) mutable -> bool {
    if (ty->get_kind () == TyTy::TypeKind::ERROR)
      {
	rust_error_at (mappings->lookup_location (id),
		       "failure in type resolution for %u", id);
	return false;
      }

    // nothing to do
    if (ty->get_kind () != TyTy::TypeKind::INFER)
      return true;

    TyTy::InferType *infer_var = (TyTy::InferType *) ty;
    switch (infer_var->get_infer_kind ())
      {
      case TyTy::InferType::GENERAL:
	rust_error_at (mappings->lookup_location (id),
		       "unable to determine type: please give this a type: %u",
		       id);
	break;

	case TyTy::InferType::INTEGRAL: {
	  TyTy::BaseType *default_integer;
	  bool ok = context->lookup_builtin ("i32", &default_integer);
	  rust_assert (ok);

	  auto result = ty->unify (default_integer);
	  result->set_ref (id);
	  context->insert_type (
	    Analysis::NodeMapping (mappings->get_current_crate (), 0, id,
				   UNKNOWN_LOCAL_DEFID),
	    result);
	}
	break;

	case TyTy::InferType::FLOAT: {
	  TyTy::BaseType *default_float;
	  bool ok = context->lookup_builtin ("f32", &default_float);
	  rust_assert (ok);

	  auto result = ty->unify (default_float);
	  result->set_ref (id);
	  context->insert_type (
	    Analysis::NodeMapping (mappings->get_current_crate (), 0, id,
				   UNKNOWN_LOCAL_DEFID),
	    result);
	}
	break;
      }

    return true;
  });
}

// RUST_HIR_TYPE_CHECK_EXPR
void
TypeCheckExpr::visit (HIR::BlockExpr &expr)
{
  TyTy::BaseType *block_tyty
    = new TyTy::UnitType (expr.get_mappings ().get_hirid ());

  expr.iterate_stmts ([&] (HIR::Stmt *s) mutable -> bool {
    bool is_final_stmt = expr.is_final_stmt (s);
    bool has_final_expr = expr.has_expr () && expr.tail_expr_reachable ();
    bool stmt_is_final_expr = is_final_stmt && !has_final_expr;

    auto resolved = TypeCheckStmt::Resolve (s, inside_loop);
    if (resolved == nullptr)
      {
	rust_error_at (s->get_locus_slow (), "failure to resolve type");
	return false;
      }

    if (stmt_is_final_expr)
      {
	delete block_tyty;
	block_tyty = resolved;
      }
    else if (resolved->get_kind () != TyTy::TypeKind::UNIT)
      {
	rust_error_at (s->get_locus_slow (), "expected () got %s",
		       resolved->as_string ().c_str ());
      }

    return true;
  });

  if (expr.has_expr ())
    {
      delete block_tyty;

      block_tyty
	= TypeCheckExpr::Resolve (expr.get_final_expr ().get (), inside_loop);
    }

  infered = block_tyty->clone ();
}

// RUST_HIR_TYPE_CHECK_STRUCT_FIELD

void
TypeCheckStructExpr::visit (HIR::StructExprStructFields &struct_expr)
{
  struct_expr.get_struct_name ().accept_vis (*this);
  if (struct_path_resolved == nullptr)
    {
      rust_fatal_error (struct_expr.get_struct_name ().get_locus (),
			"Failed to resolve type");
      return;
    }

  TyTy::ADTType *struct_def = struct_path_resolved;
  if (struct_expr.has_struct_base ())
    {
      TyTy::BaseType *base_resolved
	= TypeCheckExpr::Resolve (struct_expr.struct_base->base_struct.get (),
				  false);
      struct_def
	= (TyTy::ADTType *) struct_path_resolved->unify (base_resolved);
      if (struct_def == nullptr)
	{
	  rust_fatal_error (
	    struct_expr.struct_base->base_struct->get_locus_slow (),
	    "incompatible types for base struct reference");
	  return;
	}
    }

  std::vector<TyTy::StructFieldType *> infered_fields;
  bool ok = true;
  struct_expr.iterate ([&] (HIR::StructExprField *field) mutable -> bool {
    resolved_field_value_expr = nullptr;
    field->accept_vis (*this);
    if (resolved_field_value_expr == nullptr)
      {
	rust_fatal_error (field->get_locus (),
			  "failed to resolve type for field");
	ok = false;
	return false;
      }

    context->insert_type (field->get_mappings (), resolved_field_value_expr);
    return true;
  });

  // something failed setting up the fields
  if (!ok)
    {
      rust_error_at (struct_expr.get_locus (),
		     "constructor type resolution failure");
      return;
    }

  // check the arguments are all assigned and fix up the ordering
  if (fields_assigned.size () != struct_path_resolved->num_fields ())
    {
      if (!struct_expr.has_struct_base ())
	{
	  rust_error_at (struct_expr.get_locus (),
			 "constructor is missing fields");
	  return;
	}
      else
	{
	  // we have a struct base to assign the missing fields from.
	  // the missing fields can be implicit FieldAccessExprs for the value
	  std::set<std::string> missing_fields;
	  struct_path_resolved->iterate_fields (
	    [&] (TyTy::StructFieldType *field) mutable -> bool {
	      auto it = fields_assigned.find (field->get_name ());
	      if (it == fields_assigned.end ())
		missing_fields.insert (field->get_name ());
	      return true;
	    });

	  // we can generate FieldAccessExpr or TupleAccessExpr for the values
	  // of the missing fields.
	  for (auto &missing : missing_fields)
	    {
	      HIR::Expr *receiver
		= struct_expr.struct_base->base_struct->clone_expr_impl ();

	      HIR::StructExprField *implicit_field = nullptr;

	      std::vector<HIR::Attribute> outer_attribs;
	      auto crate_num = mappings->get_current_crate ();
	      Analysis::NodeMapping mapping (
		crate_num,
		struct_expr.struct_base->base_struct->get_mappings ()
		  .get_nodeid (),
		mappings->get_next_hir_id (crate_num), UNKNOWN_LOCAL_DEFID);

	      HIR::Expr *field_value = new HIR::FieldAccessExpr (
		mapping, std::unique_ptr<HIR::Expr> (receiver), missing,
		std::move (outer_attribs),
		struct_expr.struct_base->base_struct->get_locus_slow ());

	      implicit_field = new HIR::StructExprFieldIdentifierValue (
		mapping, missing, std::unique_ptr<HIR::Expr> (field_value),
		struct_expr.struct_base->base_struct->get_locus_slow ());

	      size_t field_index;
	      bool ok = struct_path_resolved->get_field (missing, &field_index);
	      rust_assert (ok);

	      adtFieldIndexToField[field_index] = implicit_field;
	      struct_expr.get_fields ().push_back (
		std::unique_ptr<HIR::StructExprField> (implicit_field));
	    }
	}
    }

  // everything is ok, now we need to ensure all field values are ordered
  // correctly. The GIMPLE backend uses a simple algorithm that assumes each
  // assigned field in the constructor is in the same order as the field in the
  // type

  std::vector<std::unique_ptr<HIR::StructExprField> > expr_fields
    = struct_expr.get_fields_as_owner ();
  for (auto &f : expr_fields)
    f.release ();

  std::vector<std::unique_ptr<HIR::StructExprField> > ordered_fields;
  for (size_t i = 0; i < adtFieldIndexToField.size (); i++)
    {
      ordered_fields.push_back (
	std::unique_ptr<HIR::StructExprField> (adtFieldIndexToField[i]));
    }
  struct_expr.set_fields_as_owner (std::move (ordered_fields));

  resolved = struct_def;
}

void
TypeCheckStructExpr::visit (HIR::PathInExpression &expr)
{
  NodeId ast_node_id = expr.get_mappings ().get_nodeid ();

  // then lookup the reference_node_id
  NodeId ref_node_id;
  if (!resolver->lookup_resolved_name (ast_node_id, &ref_node_id))
    {
      if (!resolver->lookup_resolved_type (ast_node_id, &ref_node_id))
	{
	  rust_error_at (expr.get_locus (),
			 "Failed to lookup reference for node: %s",
			 expr.as_string ().c_str ());
	  return;
	}
    }

  // node back to HIR
  HirId ref;
  if (!mappings->lookup_node_to_hir (expr.get_mappings ().get_crate_num (),
				     ref_node_id, &ref))
    {
      rust_error_at (expr.get_locus (), "reverse lookup failure");
      return;
    }

  // the base reference for this name _must_ have a type set
  TyTy::BaseType *lookup;
  if (!context->lookup_type (ref, &lookup))
    {
      rust_error_at (mappings->lookup_location (ref),
		     "consider giving this a type: %s",
		     expr.as_string ().c_str ());
      return;
    }

  if (lookup->get_kind () != TyTy::TypeKind::ADT)
    {
      rust_fatal_error (mappings->lookup_location (ref),
			"expected an ADT type");
      return;
    }

  struct_path_resolved = static_cast<TyTy::ADTType *> (lookup);
  if (struct_path_resolved->has_substitions ())
    {
      HIR::PathExprSegment seg = expr.get_final_segment ();
      struct_path_resolved
	= seg.has_generic_args ()
	    ? struct_path_resolved->handle_substitions (seg.get_generic_args ())
	    : struct_path_resolved->infer_substitions ();
    }
}

void
TypeCheckStructExpr::visit (HIR::StructExprFieldIdentifierValue &field)
{
  auto it = fields_assigned.find (field.field_name);
  if (it != fields_assigned.end ())
    {
      rust_fatal_error (field.get_locus (), "used more than once");
      return;
    }

  size_t field_index;
  TyTy::StructFieldType *field_type
    = struct_path_resolved->get_field (field.field_name, &field_index);
  if (field_type == nullptr)
    {
      rust_error_at (field.get_locus (), "unknown field");
      return;
    }

  TyTy::BaseType *value = TypeCheckExpr::Resolve (field.get_value (), false);
  resolved_field_value_expr = field_type->get_field_type ()->unify (value);
  if (resolved_field_value_expr != nullptr)
    {
      fields_assigned.insert (field.field_name);
      adtFieldIndexToField[field_index] = &field;
    }
}

void
TypeCheckStructExpr::visit (HIR::StructExprFieldIndexValue &field)
{
  std::string field_name (std::to_string (field.get_tuple_index ()));
  auto it = fields_assigned.find (field_name);
  if (it != fields_assigned.end ())
    {
      rust_fatal_error (field.get_locus (), "used more than once");
      return;
    }

  size_t field_index;

  TyTy::StructFieldType *field_type
    = struct_path_resolved->get_field (field_name, &field_index);
  if (field_type == nullptr)
    {
      rust_error_at (field.get_locus (), "unknown field");
      return;
    }

  TyTy::BaseType *value = TypeCheckExpr::Resolve (field.get_value (), false);
  resolved_field_value_expr = field_type->get_field_type ()->unify (value);
  if (resolved_field_value_expr != nullptr)
    {
      fields_assigned.insert (field_name);
      adtFieldIndexToField[field_index] = &field;
    }
}

void
TypeCheckStructExpr::visit (HIR::StructExprFieldIdentifier &field)
{
  auto it = fields_assigned.find (field.get_field_name ());
  if (it != fields_assigned.end ())
    {
      rust_fatal_error (field.get_locus (), "used more than once");
      return;
    }

  size_t field_index;
  TyTy::StructFieldType *field_type
    = struct_path_resolved->get_field (field.get_field_name (), &field_index);
  if (field_type == nullptr)
    {
      rust_error_at (field.get_locus (), "unknown field");
      return;
    }

  // we can make the field look like an identifier expr to take advantage of
  // existing code to figure out the type
  HIR::IdentifierExpr expr (field.get_mappings (), field.get_field_name (),
			    field.get_locus ());
  TyTy::BaseType *value = TypeCheckExpr::Resolve (&expr, false);

  resolved_field_value_expr = field_type->get_field_type ()->unify (value);
  if (resolved_field_value_expr != nullptr)

    {
      fields_assigned.insert (field.field_name);
      adtFieldIndexToField[field_index] = &field;
    }
}

} // namespace Resolver
} // namespace Rust
