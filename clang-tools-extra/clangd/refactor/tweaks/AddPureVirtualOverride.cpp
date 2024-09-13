//===--- ExpandDeducedType.cpp -----------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "refactor/Tweak.h"

#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/LLVM.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include <AST.h>
#include <string>

namespace clang {
namespace clangd {
namespace {

class OverridePureVirtuals : public Tweak {
public:
  const char *id() const final; // defined by REGISTER_TWEAK.
  bool prepare(const Selection &Sel) override;
  Expected<Effect> apply(const Selection &Sel) override;
  std::string title() const override { return "Override pure virtual methods"; }
  llvm::StringLiteral kind() const override {
    return CodeAction::REFACTOR_KIND;
  }

private:
  void preApply();
  const CXXRecordDecl *CurrentDecl = nullptr;
  std::vector<const CXXMethodDecl *> MissingPureVirtualMethods;
  SourceLocation InsertionPoint;
  std::map<AccessSpecifier, SourceLocation> AccessSpecifierLocations;
};

REGISTER_TWEAK(OverridePureVirtuals)

std::vector<const CXXMethodDecl *>
getAllPureVirtualMethods(std::vector<const CXXRecordDecl *> Decls) {
  std::vector<const CXXMethodDecl *> Result;
  std::function<void(const CXXRecordDecl *)> AddVirtualMethods;
  AddVirtualMethods = [&Result, &AddVirtualMethods](const CXXRecordDecl *Decl) {
    if (!Decl) {
      return;
    }
    // Add virtual methods from the current class.
    std::copy_if(Decl->method_begin(), Decl->method_end(),
                 std::back_inserter(Result),
                 [](CXXMethodDecl *M) { return M->isPureVirtual(); });
    // Recursively add virtual methods from base classes.
    for (const auto &Base : Decl->bases()) {
      if (const CXXRecordDecl *BaseDecl =
              Base.getType()->getAsCXXRecordDecl()) {
        AddVirtualMethods(BaseDecl->getCanonicalDecl());
      }
    }
  };
  for (auto &Decl : Decls) {
    AddVirtualMethods(Decl);
  }
  return Result;
}

std::vector<const CXXMethodDecl *> getOverridenMethods(const CXXRecordDecl *D) {
  std::vector<const CXXMethodDecl *> R;
  for (const CXXMethodDecl *M : D->methods()) {
    llvm::append_range(R, llvm::to_vector(M->overridden_methods()));
  }
  return R;
}

// Get the location of every colon of the `AccessSpecifier`.
std::map<AccessSpecifier, SourceLocation>
getSpecifierLocations(const CXXRecordDecl *D) {
  std::map<AccessSpecifier, SourceLocation> Locs;
  for (auto &Decl : D->decls()) {
    if (const auto *ASD = llvm::dyn_cast<AccessSpecDecl>(Decl)) {
      Locs[ASD->getAccess()] = ASD->getColonLoc();
    }
  }
  return Locs;
}

// Check if the current class has any pure virtual method to be implemented.
bool OverridePureVirtuals::prepare(const Selection &Sel) {
  const SelectionTree::Node *Node = Sel.ASTSelection.commonAncestor();
  if (!Node) {
    return false;
  }
  CurrentDecl = Node->ASTNode.get<CXXRecordDecl>();
  return CurrentDecl &&
         std::any_of(CurrentDecl->bases().begin(), CurrentDecl->bases().end(),
                     [](const CXXBaseSpecifier &Base) {
                       auto *BaseDecl = Base.getType()->getAsCXXRecordDecl();
                       return BaseDecl && BaseDecl->isAbstract();
                     });
}

void OverridePureVirtuals::preApply() {
  AccessSpecifierLocations = getSpecifierLocations(CurrentDecl);
  MissingPureVirtualMethods.clear();

  std::vector<std::pair<AccessSpecifier, const CXXRecordDecl *>> Bases;
  for (auto &Base : CurrentDecl->bases()) {
    if (const auto *BaseDecl =
            Base.getType()->getAsCXXRecordDecl()->getCanonicalDecl()) {
      Bases.emplace_back(std::make_pair(Base.getAccessSpecifier(), BaseDecl));
    }
  }

  // TODO(marco): Relate the access specifiers with the methods to be overriden.
  std::vector<const CXXMethodDecl*> BaseVirtualMethods;
  for(auto &Var: Bases) {
      BaseVirtualMethods = getAllPureVirtualMethods({Var.second});
  }
  // const auto BaseVirtualMethods = getAllPureVirtualMethods(Bases);
  const auto OverridenMethods = getOverridenMethods(CurrentDecl);

  std::set<const CXXMethodDecl *> OverridenSet;
  std::transform(OverridenMethods.cbegin(), OverridenMethods.cend(),
                 std::inserter(OverridenSet, OverridenSet.end()),
                 [](const CXXMethodDecl *D) { return D->getCanonicalDecl(); });

  // Copy only the methods that weren't overriden to the list that we want to
  // apply for the signature.
  std::copy_if(BaseVirtualMethods.begin(), BaseVirtualMethods.end(),
               std::back_inserter(MissingPureVirtualMethods),
               [&OverridenSet](const CXXMethodDecl *D) {
                 bool AlreadyOverriden =
                     OverridenSet.find(D->getCanonicalDecl()) !=
                     OverridenSet.end();
                 return !AlreadyOverriden;
               });
}

Expected<Tweak::Effect> OverridePureVirtuals::apply(const Selection &Sel) {
  preApply();

  const auto &SM = Sel.AST->getSourceManager();

  std::string Insertion;

  for (const auto *Method : MissingPureVirtualMethods) {
    std::vector<std::string> ParamsAsString;
    ParamsAsString.reserve(Method->parameters().size());
    std::transform(Method->param_begin(), Method->param_end(),
                   std::back_inserter(ParamsAsString),
                   [](const ParmVarDecl *P) {
                     return llvm::formatv("{0} {1}", P->getType().getAsString(),
                                          P->getNameAsString());
                   });
    const auto Params = llvm::join(ParamsAsString, ", ");

    Insertion +=
        llvm::formatv("{0} {1}({2}) {3}override {{ static_assert(false, "
                      "\"`{1}` is unimplemented.\"); }\n",
                      Method->getReturnType().getAsString(),
                      Method->getNameAsString(), Params,
                      std::string(Method->isConst() ? "const " : ""))
            .str();
  }

  SourceLocation InsertLoc =
      CurrentDecl->getBraceRange().getBegin().getLocWithOffset(1);
  if (auto It = AccessSpecifierLocations.find(AccessSpecifier::AS_public);
      It != AccessSpecifierLocations.end()) {
    InsertLoc = It->second.getLocWithOffset(2);
  }

  tooling::Replacement Repl(SM, InsertLoc, 0, Insertion);
  return Effect::mainFileEdit(SM, tooling::Replacements(Repl));
}

} // namespace
} // namespace clangd
} // namespace clang
