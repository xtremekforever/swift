//===--- SILIsolationInfo.cpp ---------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SILOptimizer/Utils/SILIsolationInfo.h"

#include "swift/AST/ASTWalker.h"
#include "swift/AST/Expr.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "swift/SILOptimizer/Utils/VariableNameUtils.h"

using namespace swift;
using namespace swift::PatternMatch;

static std::optional<ActorIsolation>
getGlobalActorInitIsolation(SILFunction *fn) {
  auto block = fn->begin();

  // Make sure our function has a single block. We should always have a single
  // block today. Return nullptr otherwise.
  if (block == fn->end() || std::next(block) != fn->end())
    return {};

  GlobalAddrInst *gai = nullptr;
  if (!match(cast<SILInstruction>(block->getTerminator()),
             m_ReturnInst(m_AddressToPointerInst(m_GlobalAddrInst(gai)))))
    return {};

  auto *globalDecl = gai->getReferencedGlobal()->getDecl();
  if (!globalDecl)
    return {};

  // See if our globalDecl is specifically guarded.
  return getActorIsolation(globalDecl);
}

static DeclRefExpr *getDeclRefExprFromExpr(Expr *expr) {
  struct LocalWalker final : ASTWalker {
    DeclRefExpr *result = nullptr;

    PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
      assert(!result && "Shouldn't have a result yet");

      if (auto *dre = dyn_cast<DeclRefExpr>(expr)) {
        result = dre;
        return Action::Stop();
      }

      if (isa<CoerceExpr, MemberRefExpr, ImplicitConversionExpr, IdentityExpr>(
              expr))
        return Action::Continue(expr);

      return Action::Stop();
    }
  };

  LocalWalker walker;

  if (auto *ae = dyn_cast<AssignExpr>(expr)) {
    ae->getSrc()->walk(walker);
  } else {
    expr->walk(walker);
  }

  return walker.result;
}

SILIsolationInfo SILIsolationInfo::get(SILInstruction *inst) {
  if (auto fas = FullApplySite::isa(inst)) {
    if (auto crossing = fas.getIsolationCrossing()) {
      if (auto info = SILIsolationInfo::getWithIsolationCrossing(*crossing))
        return info;
    }

    if (fas.hasSelfArgument()) {
      auto &self = fas.getSelfArgumentOperand();
      if (fas.getArgumentParameterInfo(self).hasOption(
              SILParameterInfo::Isolated)) {
        CanType astType = self.get()->getType().getASTType();
        if (auto *nomDecl =
                astType->lookThroughAllOptionalTypes()->getAnyActor()) {
          // TODO: We really should be doing this based off of an Operand. Then
          // we would get the SILValue() for the first element. Today this can
          // only mess up isolation history.
          return SILIsolationInfo::getActorInstanceIsolated(
              SILValue(), self.get(), nomDecl);
        }
      }
    }
  }

  if (auto *pai = dyn_cast<PartialApplyInst>(inst)) {
    if (auto *ace = pai->getLoc().getAsASTNode<AbstractClosureExpr>()) {
      auto actorIsolation = ace->getActorIsolation();

      if (actorIsolation.isGlobalActor()) {
        return SILIsolationInfo::getGlobalActorIsolated(
            pai, actorIsolation.getGlobalActor());
      }

      if (actorIsolation.isActorInstanceIsolated()) {
        ApplySite as(pai);
        SILValue actorInstance;
        for (auto &op : as.getArgumentOperands()) {
          if (as.getArgumentParameterInfo(op).hasOption(
                  SILParameterInfo::Isolated)) {
            actorInstance = op.get();
            break;
          }
        }

        if (actorInstance) {
          return SILIsolationInfo::getActorInstanceIsolated(
              pai, actorInstance, actorIsolation.getActor());
        }

        // For now, if we do not have an actor instance, just create an actor
        // instance isolated without an actor instance.
        //
        // If we do not have an actor instance, that means that we have a
        // partial apply for which the isolated parameter was not closed over
        // and is an actual argument that we pass in. This means that the
        // partial apply is actually flow sensitive in terms of which specific
        // actor instance we are isolated to.
        //
        // TODO: How do we want to resolve this.
        return SILIsolationInfo::getPartialApplyActorInstanceIsolated(
            pai, actorIsolation.getActor());
      }

      assert(actorIsolation.getKind() != ActorIsolation::Erased &&
             "Implement this!");
    }
  }

  // See if the memory base is a ref_element_addr from an address. If so, add
  // the actor derived flag.
  //
  // This is important so we properly handle setters.
  if (auto *rei = dyn_cast<RefElementAddrInst>(inst)) {
    auto *nomDecl =
        rei->getOperand()->getType().getNominalOrBoundGenericNominal();

    if (nomDecl->isAnyActor())
      return SILIsolationInfo::getActorInstanceIsolated(rei, rei->getOperand(),
                                                        nomDecl);

    if (auto isolation = swift::getActorIsolation(nomDecl)) {
      assert(isolation.isGlobalActor());
      return SILIsolationInfo::getGlobalActorIsolated(
          rei, isolation.getGlobalActor());
    }
  }

  // Check if we have a global_addr inst.
  if (auto *ga = dyn_cast<GlobalAddrInst>(inst)) {
    if (auto *global = ga->getReferencedGlobal()) {
      if (auto *globalDecl = global->getDecl()) {
        auto isolation = swift::getActorIsolation(globalDecl);
        if (isolation.isGlobalActor()) {
          return SILIsolationInfo::getGlobalActorIsolated(
              ga, isolation.getGlobalActor());
        }
      }
    }
  }

  // Treat function ref as either actor isolated or sendable.
  if (auto *fri = dyn_cast<FunctionRefInst>(inst)) {
    auto isolation = fri->getReferencedFunction()->getActorIsolation();
    if (isolation.isActorIsolated()) {
      if (isolation.isGlobalActor()) {
        return SILIsolationInfo::getGlobalActorIsolated(
            fri, isolation.getGlobalActor());
      }

      // TODO: We need to be able to support flow sensitive actor instances like
      // we do for partial apply. Until we do so, just store SILValue() for
      // this. This could cause a problem if we can construct a function ref and
      // invoke it with two different actor instances of the same type and pass
      // in the same parameters to both. We should error and we would not with
      // this impl since we could not distinguish the two.
      if (isolation.getKind() == ActorIsolation::ActorInstance) {
        return SILIsolationInfo::getFlowSensitiveActorIsolated(fri, isolation);
      }

      assert(isolation.getKind() != ActorIsolation::Erased &&
             "Implement this!");
    }

    // Otherwise, lets look at the AST and see if our function ref is from an
    // autoclosure.
    if (auto *autoclosure = fri->getLoc().getAsASTNode<AutoClosureExpr>()) {
      if (auto *funcType = autoclosure->getType()->getAs<AnyFunctionType>()) {
        if (funcType->hasGlobalActor()) {
          if (funcType->hasGlobalActor()) {
            return SILIsolationInfo::getGlobalActorIsolated(
                fri, funcType->getGlobalActor());
          }
        }

        if (auto *resultFType =
                funcType->getResult()->getAs<AnyFunctionType>()) {
          if (resultFType->hasGlobalActor()) {
            return SILIsolationInfo::getGlobalActorIsolated(
                fri, resultFType->getGlobalActor());
          }
        }
      }
    }
  }

  if (auto *cmi = dyn_cast<ClassMethodInst>(inst)) {
    // Ok, we know that we do not have an actor... but we might have a global
    // actor isolated method. Use the AST to compute the actor isolation and
    // check if we are self. If we are not self, we want this to be
    // disconnected.
    if (auto *expr = cmi->getLoc().getAsASTNode<Expr>()) {
      if (auto *dre = getDeclRefExprFromExpr(expr)) {
        if (auto isolation = swift::getActorIsolation(dre->getDecl())) {
          if (isolation.isActorIsolated() &&
              (isolation.getKind() != ActorIsolation::ActorInstance ||
               isolation.getActorInstanceParameter() == 0)) {
            if (cmi->getOperand()->getType().isAnyActor()) {
              return SILIsolationInfo::getActorInstanceIsolated(
                  cmi, cmi->getOperand(),
                  cmi->getOperand()
                      ->getType()
                      .getNominalOrBoundGenericNominal());
            }
            return SILIsolationInfo::getGlobalActorIsolated(
                cmi, isolation.getGlobalActor());
          }
        }

        if (auto type = dre->getType()->getNominalOrBoundGenericNominal()) {
          if (auto isolation = swift::getActorIsolation(type)) {
            if (isolation.isActorIsolated() &&
                (isolation.getKind() != ActorIsolation::ActorInstance ||
                 isolation.getActorInstanceParameter() == 0)) {
              if (cmi->getOperand()->getType().isAnyActor()) {
                return SILIsolationInfo::getActorInstanceIsolated(
                    cmi, cmi->getOperand(),
                    cmi->getOperand()
                        ->getType()
                        .getNominalOrBoundGenericNominal());
              }
              return SILIsolationInfo::getGlobalActorIsolated(
                  cmi, isolation.getGlobalActor());
            }
          }
        }
      }
    }
  }

  // See if we have a struct_extract from a global actor isolated type.
  if (auto *sei = dyn_cast<StructExtractInst>(inst)) {
    return SILIsolationInfo::getGlobalActorIsolated(sei, sei->getStructDecl());
  }

  if (auto *seai = dyn_cast<StructElementAddrInst>(inst)) {
    return SILIsolationInfo::getGlobalActorIsolated(seai,
                                                    seai->getStructDecl());
  }

  // See if we have an unchecked_enum_data from a global actor isolated type.
  if (auto *uedi = dyn_cast<UncheckedEnumDataInst>(inst)) {
    return SILIsolationInfo::getGlobalActorIsolated(uedi, uedi->getEnumDecl());
  }

  // See if we have an unchecked_enum_data from a global actor isolated type.
  if (auto *utedi = dyn_cast<UncheckedTakeEnumDataAddrInst>(inst)) {
    return SILIsolationInfo::getGlobalActorIsolated(utedi,
                                                    utedi->getEnumDecl());
  }

  // Check if we have an unsafeMutableAddressor from a global actor, mark the
  // returned value as being actor derived.
  if (auto applySite = dyn_cast<ApplyInst>(inst)) {
    if (auto *calleeFunction = applySite->getCalleeFunction()) {
      if (calleeFunction->isGlobalInit()) {
        auto isolation = getGlobalActorInitIsolation(calleeFunction);
        if (isolation && isolation->isGlobalActor()) {
          return SILIsolationInfo::getGlobalActorIsolated(
              applySite, isolation->getGlobalActor());
        }
      }
    }
  }

  // See if we have a convert function from a Sendable actor isolated function,
  // we want to treat the result of the convert function as being actor isolated
  // so that we cannot escape the value.
  //
  // NOTE: At this point, we already know that cfi's result is not sendable,
  // since we would have exited above already.
  if (auto *cfi = dyn_cast<ConvertFunctionInst>(inst)) {
    SILValue operand = cfi->getOperand();
    if (operand->getType().getAs<SILFunctionType>()->isSendable()) {
      SILValue newValue = operand;
      do {
        operand = newValue;

        newValue = lookThroughOwnershipInsts(operand);
        if (auto *ttfi = dyn_cast<ThinToThickFunctionInst>(newValue)) {
          newValue = ttfi->getOperand();
        }

        if (auto *cfi = dyn_cast<ConvertFunctionInst>(newValue)) {
          newValue = cfi->getOperand();
        }

        if (auto *pai = dyn_cast<PartialApplyInst>(newValue)) {
          newValue = pai->getCallee();
        }
      } while (newValue != operand);

      if (auto *ai = dyn_cast<ApplyInst>(operand)) {
        if (auto *callExpr = ai->getLoc().getAsASTNode<ApplyExpr>()) {
          if (auto *callType = callExpr->getType()->getAs<AnyFunctionType>()) {
            if (callType->hasGlobalActor()) {
              return SILIsolationInfo::getGlobalActorIsolated(
                  ai, callType->getGlobalActor());
            }
          }
        }
      }

      if (auto *fri = dyn_cast<FunctionRefInst>(operand)) {
        if (auto isolation = SILIsolationInfo::get(fri)) {
          return isolation;
        }
      }
    }
  }

  // Try to infer using SIL first since we might be able to get the source name
  // of the actor.
  if (ApplyExpr *apply = inst->getLoc().getAsASTNode<ApplyExpr>()) {
    if (auto crossing = apply->getIsolationCrossing()) {
      if (auto info = SILIsolationInfo::getWithIsolationCrossing(*crossing))
        return info;

      if (crossing->getCalleeIsolation().isNonisolated()) {
        return SILIsolationInfo::getDisconnected();
      }
    }
  }

  return SILIsolationInfo();
}

SILIsolationInfo SILIsolationInfo::get(SILArgument *arg) {
  // Return early if we do not have a non-Sendable type.
  if (!SILIsolationInfo::isNonSendableType(arg->getType(), arg->getFunction()))
    return {};

  // Handle a switch_enum from a global actor isolated type.
  if (auto *phiArg = dyn_cast<SILPhiArgument>(arg)) {
    if (auto *singleTerm = phiArg->getSingleTerminator()) {
      if (auto *swi = dyn_cast<SwitchEnumInst>(singleTerm)) {
        auto enumDecl =
            swi->getOperand()->getType().getEnumOrBoundGenericEnum();
        return SILIsolationInfo::getGlobalActorIsolated(arg, enumDecl);
      }
    }
    return SILIsolationInfo();
  }

  auto *fArg = cast<SILFunctionArgument>(arg);

  // Transferring is always disconnected.
  if (!fArg->isIndirectResult() && !fArg->isIndirectErrorResult() &&
      ((fArg->isClosureCapture() &&
        fArg->getFunction()->getLoweredFunctionType()->isSendable()) ||
       fArg->isSending()))
    return SILIsolationInfo::getDisconnected();

  // Before we do anything further, see if we have an isolated parameter. This
  // handles isolated self and specifically marked isolated.
  if (auto *isolatedArg = fArg->getFunction()->maybeGetIsolatedArgument()) {
    auto astType = isolatedArg->getType().getASTType();
    if (auto *nomDecl = astType->lookThroughAllOptionalTypes()->getAnyActor()) {
      return SILIsolationInfo::getActorInstanceIsolated(fArg, isolatedArg,
                                                        nomDecl);
    }
  }

  // Otherwise, see if we have an allocator decl ref. If we do and we have an
  // actor instance isolation, then we know that we are actively just calling
  // the initializer. To just make region isolation work, treat this as
  // disconnected so we can construct the actor value. Users cannot write
  // allocator functions so we just need to worry about compiler generated
  // code. In the case of a non-actor, we can only have an allocator that is
  // global actor isolated, so we will never hit this code path.
  if (auto declRef = fArg->getFunction()->getDeclRef()) {
    if (declRef.kind == SILDeclRef::Kind::Allocator) {
      if (fArg->getFunction()->getActorIsolation().isActorInstanceIsolated()) {
        return SILIsolationInfo::getDisconnected();
      }
    }

    if (auto functionIsolation = fArg->getFunction()->getActorIsolation()) {
      if (declRef.getDecl()) {
        if (auto *accessor =
                dyn_cast_or_null<AccessorDecl>(declRef.getFuncDecl())) {
          if (accessor->isInitAccessor()) {
            assert(functionIsolation.isActorInstanceIsolated());
            return SILIsolationInfo::getActorInstanceIsolated(
                fArg, ActorInstance::getForActorAccessorInit(),
                functionIsolation.getActor());
          }
        }
      }
    }
  }

  // Otherwise, if we do not have an isolated argument and are not in an
  // alloactor, then we might be isolated via global isolation.
  if (auto functionIsolation = fArg->getFunction()->getActorIsolation()) {
    if (functionIsolation.isActorIsolated()) {
      assert(functionIsolation.isGlobalActor());
      return SILIsolationInfo::getGlobalActorIsolated(
          fArg, functionIsolation.getGlobalActor());
    }
  }

  return SILIsolationInfo::getTaskIsolated(fArg);
}

void SILIsolationInfo::print(llvm::raw_ostream &os) const {
  switch (Kind(*this)) {
  case Unknown:
    os << "unknown";
    return;
  case Disconnected:
    os << "disconnected";
    return;
  case Actor:
    if (ActorInstance instance = getActorInstance()) {
      switch (instance.getKind()) {
      case ActorInstance::Kind::Value: {
        SILValue value = instance.getValue();
        if (auto name = VariableNameInferrer::inferName(value)) {
          os << "'" << *name << "'-isolated\n";
          os << "instance: " << *value;
          return;
        }
        break;
      }
      case ActorInstance::Kind::ActorAccessorInit:
        os << "'self'-isolated\n";
        os << "instance: actor accessor init\n";
        return;
      }
    }

    if (getActorIsolation().getKind() == ActorIsolation::ActorInstance) {
      if (auto *vd = getActorIsolation().getActorInstance()) {
        os << "'" << vd->getBaseIdentifier() << "'-isolated";
        return;
      }
    }

    getActorIsolation().printForDiagnostics(os);
    return;
  case Task:
    os << "task-isolated\n";
    os << "instance: " << *getIsolatedValue();
    return;
  }
}

SILIsolationInfo SILIsolationInfo::merge(SILIsolationInfo other) const {
  // If we are greater than the other kind, then we are further along the
  // lattice. We ignore the change.
  if (unsigned(other.kind) < unsigned(kind))
    return *this;

  // TODO: Make this failing mean that we emit an unknown SIL error instead of
  // asserting.
  assert((!other.isActorIsolated() || !isActorIsolated() ||
          hasSameIsolation(other)) &&
         "Actor can only be merged with the same actor");

  // Otherwise, take the other value.
  return other;
}

bool SILIsolationInfo::hasSameIsolation(ActorIsolation actorIsolation) const {
  if (getKind() != Kind::Actor)
    return false;
  return getActorIsolation() == actorIsolation;
}

bool SILIsolationInfo::hasSameIsolation(const SILIsolationInfo &other) const {
  if (getKind() != other.getKind())
    return false;

  switch (getKind()) {
  case Unknown:
  case Disconnected:
    return true;
  case Task:
    return getIsolatedValue() == other.getIsolatedValue();
  case Actor: {
    ActorInstance actor1 = getActorInstance();
    ActorInstance actor2 = other.getActorInstance();

    // If either are non-null, and the actor instance doesn't match, return
    // false.
    if ((actor1 || actor2) && actor1 != actor2)
      return false;

    auto lhsIsolation = getActorIsolation();
    auto rhsIsolation = other.getActorIsolation();
    return lhsIsolation == rhsIsolation;
  }
  }
}

bool SILIsolationInfo::isEqual(const SILIsolationInfo &other) const {
  // First check if the two types have the same isolation.
  if (!hasSameIsolation(other))
    return false;

  // Then check if both have the same isolated value state. If they do not
  // match, bail they cannot equal.
  if (hasIsolatedValue() != other.hasIsolatedValue())
    return false;

  // Then actually check if we have an isolated value. If we do not, then both
  // do not have an isolated value due to our earlier check, so we can just
  // return true early.
  if (!hasIsolatedValue())
    return true;

  // Otherwise, equality is determined by directly comparing the isolated value.
  return getIsolatedValue() == other.getIsolatedValue();
}

void SILIsolationInfo::Profile(llvm::FoldingSetNodeID &id) const {
  id.AddInteger(getKind());
  switch (getKind()) {
  case Unknown:
  case Disconnected:
    return;
  case Task:
    id.AddPointer(getIsolatedValue());
    return;
  case Actor:
    id.AddPointer(getIsolatedValue());
    getActorIsolation().Profile(id);
    return;
  }
}

void SILIsolationInfo::printForDiagnostics(llvm::raw_ostream &os) const {
  switch (Kind(*this)) {
  case Unknown:
    llvm::report_fatal_error("Printing unknown for diagnostics?!");
    return;
  case Disconnected:
    os << "disconnected";
    return;
  case Actor:
    if (auto instance = getActorInstance()) {
      switch (instance.getKind()) {
      case ActorInstance::Kind::Value: {
        SILValue value = instance.getValue();
        if (auto name = VariableNameInferrer::inferName(value)) {
          os << "'" << *name << "'-isolated";
          return;
        }
        break;
      }
      case ActorInstance::Kind::ActorAccessorInit:
        os << "'self'-isolated";
        return;
      }
    }

    if (getActorIsolation().getKind() == ActorIsolation::ActorInstance) {
      if (auto *vd = getActorIsolation().getActorInstance()) {
        os << "'" << vd->getBaseIdentifier() << "'-isolated";
        return;
      }
    }

    getActorIsolation().printForDiagnostics(os);
    return;
  case Task:
    os << "task-isolated";
    return;
  }
}

// Check if the passed in type is NonSendable.
//
// NOTE: We special case RawPointer and NativeObject to ensure they are
// treated as non-Sendable and strict checking is applied to it.
bool SILIsolationInfo::isNonSendableType(SILType type, SILFunction *fn) {
  // Treat Builtin.NativeObject and Builtin.RawPointer as non-Sendable.
  if (type.getASTType()->is<BuiltinNativeObjectType>() ||
      type.getASTType()->is<BuiltinRawPointerType>()) {
    return true;
  }

  // Treat Builtin.SILToken as Sendable. It cannot escape from the current
  // function. We should change isSendable to hardwire this.
  if (type.getASTType()->is<SILTokenType>()) {
    return false;
  }

  // Otherwise, delegate to seeing if type conforms to the Sendable protocol.
  return !type.isSendable(fn);
}
