/*
 * Copyright (c) 1998, 2013, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "compiler/compileLog.hpp"
#include "interpreter/linkResolver.hpp"
#include "memory/universe.inline.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/valueArrayKlass.hpp"
#include "opto/addnode.hpp"
#include "opto/castnode.hpp"
#include "opto/memnode.hpp"
#include "opto/parse.hpp"
#include "opto/rootnode.hpp"
#include "opto/runtime.hpp"
#include "opto/subnode.hpp"
#include "opto/valuetypenode.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/handles.inline.hpp"

//=============================================================================
// Helper methods for _get* and _put* bytecodes
//=============================================================================
bool Parse::static_field_ok_in_clinit(ciField *field, ciMethod *method) {
  // Could be the field_holder's <clinit> method, or <clinit> for a subklass.
  // Better to check now than to Deoptimize as soon as we execute
  assert( field->is_static(), "Only check if field is static");
  // is_being_initialized() is too generous.  It allows access to statics
  // by threads that are not running the <clinit> before the <clinit> finishes.
  // return field->holder()->is_being_initialized();

  // The following restriction is correct but conservative.
  // It is also desirable to allow compilation of methods called from <clinit>
  // but this generated code will need to be made safe for execution by
  // other threads, or the transition from interpreted to compiled code would
  // need to be guarded.
  ciInstanceKlass *field_holder = field->holder();

  bool access_OK = false;
  if (method->holder()->is_subclass_of(field_holder)) {
    if (method->is_static()) {
      if (method->name() == ciSymbol::class_initializer_name()) {
        // OK to access static fields inside initializer
        access_OK = true;
      }
    } else {
      if (method->name() == ciSymbol::object_initializer_name()) {
        // It's also OK to access static fields inside a constructor,
        // because any thread calling the constructor must first have
        // synchronized on the class by executing a '_new' bytecode.
        access_OK = true;
      }
    }
  }

  return access_OK;

}


void Parse::do_field_access(bool is_get, bool is_field) {
  bool will_link;
  ciField* field = iter().get_field(will_link);
  assert(will_link, "getfield: typeflow responsibility");

  ciInstanceKlass* field_holder = field->holder();

  if (is_field && field_holder->is_valuetype()) {
    assert(is_get, "value type field store not supported");
    BasicType bt = field->layout_type();
    ValueTypeNode* vt = pop()->as_ValueType();
    Node* value = vt->field_value_by_offset(field->offset());
    push_node(bt, value);
    return;
  }

  if (is_field == field->is_static()) {
    // Interpreter will throw java_lang_IncompatibleClassChangeError
    // Check this before allowing <clinit> methods to access static fields
    uncommon_trap(Deoptimization::Reason_unhandled,
                  Deoptimization::Action_none);
    return;
  }

  if (!is_field && !field_holder->is_initialized()) {
    if (!static_field_ok_in_clinit(field, method())) {
      uncommon_trap(Deoptimization::Reason_uninitialized,
                    Deoptimization::Action_reinterpret,
                    NULL, "!static_field_ok_in_clinit");
      return;
    }
  }

  // Deoptimize on putfield writes to call site target field.
  if (!is_get && field->is_call_site_target()) {
    uncommon_trap(Deoptimization::Reason_unhandled,
                  Deoptimization::Action_reinterpret,
                  NULL, "put to call site target field");
    return;
  }

  assert(field->will_link(method(), bc()), "getfield: typeflow responsibility");

  // Note:  We do not check for an unloaded field type here any more.

  // Generate code for the object pointer.
  Node* obj;
  if (is_field) {
    int obj_depth = is_get ? 0 : field->type()->size();
    obj = null_check(peek(obj_depth));
    // Compile-time detect of null-exception?
    if (stopped())  return;

#ifdef ASSERT
    const TypeInstPtr *tjp = TypeInstPtr::make(TypePtr::NotNull, iter().get_declared_field_holder());
    assert(_gvn.type(obj)->higher_equal(tjp), "cast_up is no longer needed");
#endif

    if (is_get) {
      (void) pop();  // pop receiver before getting
      do_get_xxx(obj, field, is_field);
    } else {
      do_put_xxx(obj, field, is_field);
      if (stopped()) {
        return;
      }
      (void) pop();  // pop receiver after putting
    }
  } else {
    const TypeInstPtr* tip = TypeInstPtr::make(field_holder->java_mirror());
    obj = _gvn.makecon(tip);
    if (is_get) {
      do_get_xxx(obj, field, is_field);
    } else {
      do_put_xxx(obj, field, is_field);
    }
  }
}

void Parse::do_get_xxx(Node* obj, ciField* field, bool is_field) {
  BasicType bt = field->layout_type();

  // Does this field have a constant value?  If so, just push the value.
  if (field->is_constant() &&
      // Keep consistent with types found by ciTypeFlow: for an
      // unloaded field type, ciTypeFlow::StateVector::do_getstatic()
      // speculates the field is null. The code in the rest of this
      // method does the same. We must not bypass it and use a non
      // null constant here.
      (bt != T_OBJECT || field->type()->is_loaded())) {
    // final or stable field
    Node* con = make_constant_from_field(field, obj);
    if (con != NULL) {
      push_node(field->layout_type(), con);
      return;
    }
  }

  ciType* field_klass = field->type();
  bool is_vol = field->is_volatile();
  bool flattened = field->is_flattened();
  bool flattenable = field->is_flattenable();

  // Compute address and memory type.
  int offset = field->offset_in_bytes();
  const TypePtr* adr_type = C->alias_type(field)->adr_type();
  Node *adr = basic_plus_adr(obj, obj, offset);

  // Build the resultant type of the load
  const Type *type;

  bool must_assert_null = false;
  if (bt == T_OBJECT || bt == T_VALUETYPE) {
    if (!field->type()->is_loaded()) {
      type = TypeInstPtr::BOTTOM;
      must_assert_null = true;
    } else if (field->is_static_constant()) {
      // This can happen if the constant oop is non-perm.
      ciObject* con = field->constant_value().as_object();
      // Do not "join" in the previous type; it doesn't add value,
      // and may yield a vacuous result if the field is of interface type.
      if (con->is_null_object()) {
        type = TypePtr::NULL_PTR;
      } else {
        type = TypeOopPtr::make_from_constant(con)->isa_oopptr();
      }
      assert(type != NULL, "field singleton type must be consistent");
    } else {
      type = TypeOopPtr::make_from_klass(field_klass->as_klass());
      if (bt == T_VALUETYPE && field->is_static()) {
        // Check if static value type field is already initialized
        assert(!flattened, "static fields should not be flattened");
        ciInstance* mirror = field->holder()->java_mirror();
        ciObject* val = mirror->field_value(field).as_object();
        if (!val->is_null_object()) {
          type = type->join_speculative(TypePtr::NOTNULL);
        }
      }
    }
  } else {
    type = Type::get_const_basic_type(bt);
  }
  if (support_IRIW_for_not_multiple_copy_atomic_cpu && field->is_volatile()) {
    insert_mem_bar(Op_MemBarVolatile);   // StoreLoad barrier
  }

  // Build the load.
  //
  MemNode::MemOrd mo = is_vol ? MemNode::acquire : MemNode::unordered;
  bool needs_atomic_access = is_vol || AlwaysAtomicAccesses;
  Node* ld = NULL;
  if (flattened) {
    // Load flattened value type
    ld = ValueTypeNode::make_from_flattened(this, field_klass->as_value_klass(), obj, obj, field->holder(), offset);
  } else {
    if (bt == T_VALUETYPE && !flattenable) {
      // Non-flattenable value type field can be null and we
      // should not return the default value type in that case.
      bt = T_VALUETYPEPTR;
    }
    ld = make_load(NULL, adr, type, bt, adr_type, mo, LoadNode::DependsOnlyOnTest, needs_atomic_access);
  }

  // Adjust Java stack
  if (type2size[bt] == 1)
    push(ld);
  else
    push_pair(ld);

  if (must_assert_null) {
    // Do not take a trap here.  It's possible that the program
    // will never load the field's class, and will happily see
    // null values in this field forever.  Don't stumble into a
    // trap for such a program, or we might get a long series
    // of useless recompilations.  (Or, we might load a class
    // which should not be loaded.)  If we ever see a non-null
    // value, we will then trap and recompile.  (The trap will
    // not need to mention the class index, since the class will
    // already have been loaded if we ever see a non-null value.)
    // uncommon_trap(iter().get_field_signature_index());
    if (PrintOpto && (Verbose || WizardMode)) {
      method()->print_name(); tty->print_cr(" asserting nullness of field at bci: %d", bci());
    }
    if (C->log() != NULL) {
      C->log()->elem("assert_null reason='field' klass='%d'",
                     C->log()->identify(field->type()));
    }
    // If there is going to be a trap, put it at the next bytecode:
    set_bci(iter().next_bci());
    null_assert(peek());
    set_bci(iter().cur_bci()); // put it back
  }

  // If reference is volatile, prevent following memory ops from
  // floating up past the volatile read.  Also prevents commoning
  // another volatile read.
  if (field->is_volatile()) {
    // Memory barrier includes bogus read of value to force load BEFORE membar
    insert_mem_bar(Op_MemBarAcquire, ld);
  }
}

void Parse::do_put_xxx(Node* obj, ciField* field, bool is_field) {
  bool is_vol = field->is_volatile();
  bool is_flattened = field->is_flattened();
  // If reference is volatile, prevent following memory ops from
  // floating down past the volatile write.  Also prevents commoning
  // another volatile read.
  if (is_vol)  insert_mem_bar(Op_MemBarRelease);

  // Compute address and memory type.
  int offset = field->offset_in_bytes();
  const TypePtr* adr_type = C->alias_type(field)->adr_type();
  Node* adr = basic_plus_adr(obj, obj, offset);
  BasicType bt = field->layout_type();
  // Value to be stored
  Node* val = type2size[bt] == 1 ? pop() : pop_pair();
  // Round doubles before storing
  if (bt == T_DOUBLE)  val = dstore_rounding(val);

  // Conservatively release stores of object references.
  const MemNode::MemOrd mo =
    is_vol ?
    // Volatile fields need releasing stores.
    MemNode::release :
    // Non-volatile fields also need releasing stores if they hold an
    // object reference, because the object reference might point to
    // a freshly created object.
    StoreNode::release_if_reference(bt);

  // Store the value.
  if (bt == T_OBJECT || bt == T_VALUETYPE) {
    const TypeOopPtr* field_type;
    if (!field->type()->is_loaded()) {
      field_type = TypeInstPtr::BOTTOM;
    } else {
      field_type = TypeOopPtr::make_from_klass(field->type()->as_klass());
    }
    if (field->is_flattenable() && !val->is_ValueType()) {
      // We can see a null constant here
      assert(val->bottom_type()->remove_speculative() == TypePtr::NULL_PTR, "Anything other than null?");
      push(null());
      Deoptimization::DeoptReason reason = Deoptimization::reason_null_check(false);
      uncommon_trap(reason, Deoptimization::Action_none);
      assert(stopped(), "dead path");
      return;
    }
    if (is_flattened) {
      // if (!val->is_ValueType()) {
      //   const TypeValueTypePtr* vtptr = _gvn.type(val)->isa_valuetypeptr();
      //   val = ValueTypeNode::make_from_oop(this, val, vtptr->value_klass(), /* null_check */ false, /* buffer_check */ false);
      // }
      // Store flattened value type to a non-static field
      assert(bt == T_VALUETYPE, "flattening is only supported for value type fields");
      val->as_ValueType()->store_flattened(this, obj, obj, field->holder(), offset);
    } else {
      store_oop_to_object(control(), obj, adr, adr_type, val, field_type, bt, mo);
    }
  } else {
    bool needs_atomic_access = is_vol || AlwaysAtomicAccesses;
    store_to_memory(control(), adr, val, bt, adr_type, mo, needs_atomic_access);
  }

  // If reference is volatile, prevent following volatiles ops from
  // floating up before the volatile write.
  if (is_vol) {
    // If not multiple copy atomic, we do the MemBarVolatile before the load.
    if (!support_IRIW_for_not_multiple_copy_atomic_cpu) {
      insert_mem_bar(Op_MemBarVolatile); // Use fat membar
    }
    // Remember we wrote a volatile field.
    // For not multiple copy atomic cpu (ppc64) a barrier should be issued
    // in constructors which have such stores. See do_exits() in parse1.cpp.
    if (is_field) {
      set_wrote_volatile(true);
    }
  }

  if (is_field) {
    set_wrote_fields(true);
  }

  // If the field is final, the rules of Java say we are in <init> or <clinit>.
  // Note the presence of writes to final non-static fields, so that we
  // can insert a memory barrier later on to keep the writes from floating
  // out of the constructor.
  // Any method can write a @Stable field; insert memory barriers after those also.
  if (is_field && (field->is_final() || field->is_stable())) {
    if (field->is_final()) {
        set_wrote_final(true);
    }
    if (field->is_stable()) {
        set_wrote_stable(true);
    }

    // Preserve allocation ptr to create precedent edge to it in membar
    // generated on exit from constructor.
    // Can't bind stable with its allocation, only record allocation for final field.
    if (field->is_final() && AllocateNode::Ideal_allocation(obj, &_gvn) != NULL) {
      set_alloc_with_final(obj);
    }
  }
}

//=============================================================================

void Parse::do_newarray() {
  bool will_link;
  ciKlass* klass = iter().get_klass(will_link);

  // Uncommon Trap when class that array contains is not loaded
  // we need the loaded class for the rest of graph; do not
  // initialize the container class (see Java spec)!!!
  assert(will_link, "newarray: typeflow responsibility");

  ciArrayKlass* array_klass = ciArrayKlass::make(klass);
  // Check that array_klass object is loaded
  if (!array_klass->is_loaded()) {
    // Generate uncommon_trap for unloaded array_class
    uncommon_trap(Deoptimization::Reason_unloaded,
                  Deoptimization::Action_reinterpret,
                  array_klass);
    return;
  } else if (array_klass->element_klass() != NULL &&
             array_klass->element_klass()->is_valuetype() &&
             !array_klass->element_klass()->as_value_klass()->is_initialized()) {
    uncommon_trap(Deoptimization::Reason_uninitialized,
                  Deoptimization::Action_reinterpret,
                  NULL);
    return;
  }

  kill_dead_locals();

  const TypeKlassPtr* array_klass_type = TypeKlassPtr::make(array_klass);
  Node* count_val = pop();
  Node* obj = new_array(makecon(array_klass_type), count_val, 1);
  push(obj);
}


void Parse::do_newarray(BasicType elem_type) {
  kill_dead_locals();

  Node*   count_val = pop();
  const TypeKlassPtr* array_klass = TypeKlassPtr::make(ciTypeArrayKlass::make(elem_type));
  Node*   obj = new_array(makecon(array_klass), count_val, 1);
  // Push resultant oop onto stack
  push(obj);
}

// Expand simple expressions like new int[3][5] and new Object[2][nonConLen].
// Also handle the degenerate 1-dimensional case of anewarray.
Node* Parse::expand_multianewarray(ciArrayKlass* array_klass, Node* *lengths, int ndimensions, int nargs) {
  Node* length = lengths[0];
  assert(length != NULL, "");
  Node* array = new_array(makecon(TypeKlassPtr::make(array_klass)), length, nargs);
  if (ndimensions > 1) {
    jint length_con = find_int_con(length, -1);
    guarantee(length_con >= 0, "non-constant multianewarray");
    ciArrayKlass* array_klass_1 = array_klass->as_obj_array_klass()->element_klass()->as_array_klass();
    const TypePtr* adr_type = TypeAryPtr::OOPS;
    const TypeOopPtr*    elemtype = _gvn.type(array)->is_aryptr()->elem()->make_oopptr();
    const intptr_t header   = arrayOopDesc::base_offset_in_bytes(T_OBJECT);
    for (jint i = 0; i < length_con; i++) {
      Node*    elem   = expand_multianewarray(array_klass_1, &lengths[1], ndimensions-1, nargs);
      intptr_t offset = header + ((intptr_t)i << LogBytesPerHeapOop);
      Node*    eaddr  = basic_plus_adr(array, offset);
      store_oop_to_array(control(), array, eaddr, adr_type, elem, elemtype, T_OBJECT, MemNode::unordered);
    }
  }
  return array;
}

void Parse::do_multianewarray() {
  int ndimensions = iter().get_dimensions();

  // the m-dimensional array
  bool will_link;
  ciArrayKlass* array_klass = iter().get_klass(will_link)->as_array_klass();
  assert(will_link, "multianewarray: typeflow responsibility");

  // Note:  Array classes are always initialized; no is_initialized check.

  kill_dead_locals();

  // get the lengths from the stack (first dimension is on top)
  Node** length = NEW_RESOURCE_ARRAY(Node*, ndimensions + 1);
  length[ndimensions] = NULL;  // terminating null for make_runtime_call
  int j;
  for (j = ndimensions-1; j >= 0 ; j--) length[j] = pop();

  // The original expression was of this form: new T[length0][length1]...
  // It is often the case that the lengths are small (except the last).
  // If that happens, use the fast 1-d creator a constant number of times.
  const int expand_limit = MIN2((int)MultiArrayExpandLimit, 100);
  int expand_count = 1;        // count of allocations in the expansion
  int expand_fanout = 1;       // running total fanout
  for (j = 0; j < ndimensions-1; j++) {
    int dim_con = find_int_con(length[j], -1);
    expand_fanout *= dim_con;
    expand_count  += expand_fanout; // count the level-J sub-arrays
    if (dim_con <= 0
        || dim_con > expand_limit
        || expand_count > expand_limit) {
      expand_count = 0;
      break;
    }
  }

  // Can use multianewarray instead of [a]newarray if only one dimension,
  // or if all non-final dimensions are small constants.
  if (ndimensions == 1 || (1 <= expand_count && expand_count <= expand_limit)) {
    Node* obj = NULL;
    // Set the original stack and the reexecute bit for the interpreter
    // to reexecute the multianewarray bytecode if deoptimization happens.
    // Do it unconditionally even for one dimension multianewarray.
    // Note: the reexecute bit will be set in GraphKit::add_safepoint_edges()
    // when AllocateArray node for newarray is created.
    { PreserveReexecuteState preexecs(this);
      inc_sp(ndimensions);
      // Pass 0 as nargs since uncommon trap code does not need to restore stack.
      obj = expand_multianewarray(array_klass, &length[0], ndimensions, 0);
    } //original reexecute and sp are set back here
    push(obj);
    return;
  }

  address fun = NULL;
  switch (ndimensions) {
  case 1: ShouldNotReachHere(); break;
  case 2: fun = OptoRuntime::multianewarray2_Java(); break;
  case 3: fun = OptoRuntime::multianewarray3_Java(); break;
  case 4: fun = OptoRuntime::multianewarray4_Java(); break;
  case 5: fun = OptoRuntime::multianewarray5_Java(); break;
  };
  Node* c = NULL;

  if (fun != NULL) {
    c = make_runtime_call(RC_NO_LEAF | RC_NO_IO,
                          OptoRuntime::multianewarray_Type(ndimensions),
                          fun, NULL, TypeRawPtr::BOTTOM,
                          makecon(TypeKlassPtr::make(array_klass)),
                          length[0], length[1], length[2],
                          (ndimensions > 2) ? length[3] : NULL,
                          (ndimensions > 3) ? length[4] : NULL);
  } else {
    // Create a java array for dimension sizes
    Node* dims = NULL;
    { PreserveReexecuteState preexecs(this);
      inc_sp(ndimensions);
      Node* dims_array_klass = makecon(TypeKlassPtr::make(ciArrayKlass::make(ciType::make(T_INT))));
      dims = new_array(dims_array_klass, intcon(ndimensions), 0);

      // Fill-in it with values
      for (j = 0; j < ndimensions; j++) {
        Node *dims_elem = array_element_address(dims, intcon(j), T_INT);
        store_to_memory(control(), dims_elem, length[j], T_INT, TypeAryPtr::INTS, MemNode::unordered);
      }
    }

    c = make_runtime_call(RC_NO_LEAF | RC_NO_IO,
                          OptoRuntime::multianewarrayN_Type(),
                          OptoRuntime::multianewarrayN_Java(), NULL, TypeRawPtr::BOTTOM,
                          makecon(TypeKlassPtr::make(array_klass)),
                          dims);
  }
  make_slow_call_ex(c, env()->Throwable_klass(), false);

  Node* res = _gvn.transform(new ProjNode(c, TypeFunc::Parms));

  const Type* type = TypeOopPtr::make_from_klass_raw(array_klass);

  // Improve the type:  We know it's not null, exact, and of a given length.
  type = type->is_ptr()->cast_to_ptr_type(TypePtr::NotNull);
  type = type->is_aryptr()->cast_to_exactness(true);

  const TypeInt* ltype = _gvn.find_int_type(length[0]);
  if (ltype != NULL)
    type = type->is_aryptr()->cast_to_size(ltype);

    // We cannot sharpen the nested sub-arrays, since the top level is mutable.

  Node* cast = _gvn.transform( new CheckCastPPNode(control(), res, type) );
  push(cast);

  // Possible improvements:
  // - Make a fast path for small multi-arrays.  (W/ implicit init. loops.)
  // - Issue CastII against length[*] values, to TypeInt::POS.
}
