//===--- ExpandDeducedType.cpp -----------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "refactor/Tweak.h"

#include "support/Logger.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/LLVM.h"
#include "llvm/ADT/STLExtras.h"
#include <AST.h>
#include <string>

namespace clang {
namespace clangd {
namespace {

class OverridePureVirtuals : public Tweak {
public:
  const char *id() const override final;
  bool prepare(const Selection &Sel) override;
  Expected<Effect> apply(const Selection &Sel) override;
  std::string title() const override { return "Override pure virtual methods"; }
  llvm::StringLiteral kind() const override {
    return CodeAction::REFACTOR_KIND;
  }

private:
  const CXXRecordDecl *CurrentDecl = nullptr;
  std::vector<const CXXMethodDecl *> PureVirtualMethods;

  SourceLocation InsertionPoint;
};

REGISTER_TWEAK(OverridePureVirtuals)

std::vector<const CXXMethodDecl *>
getAllVirtualMethods(std::vector<const CXXRecordDecl *> Decls) {
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

bool OverridePureVirtuals::prepare(const Selection &Sel) {
  const SelectionTree::Node *Node = Sel.ASTSelection.commonAncestor();
  if (!Node) {
    return false;
  }

  CurrentDecl = Node->ASTNode.get<CXXRecordDecl>();
  if (!(CurrentDecl && CurrentDecl->isAbstract())) {
    return false;
  }

  PureVirtualMethods.clear();

  std::vector<const CXXRecordDecl *> Bases;
  for (auto &Base : CurrentDecl->bases()) {
    if (const auto *BaseDecl = Base.getType()->getAsCXXRecordDecl()) {
      Bases.emplace_back(BaseDecl);
    }
  }

  auto BaseVirtualMethods = getAllVirtualMethods(Bases);
  auto OverridenMethods = getOverridenMethods(CurrentDecl);

  std::set<const CXXMethodDecl *> OverridenSet;
  std::transform(OverridenMethods.begin(), OverridenMethods.end(),
                 std::inserter(OverridenSet, OverridenSet.end()),
                 [](const CXXMethodDecl *D) { return D->getCanonicalDecl(); });

  // Remove all the overriden methods from the list of methods that we want
  // to apply for the signature.
  std::copy_if(BaseVirtualMethods.begin(), BaseVirtualMethods.end(),
               std::back_inserter(PureVirtualMethods),
               [&OverridenSet](const CXXMethodDecl *D) {
                 bool AlreadyOverriden =
                     OverridenSet.find(D->getCanonicalDecl()) !=
                     OverridenSet.end();
                 return !AlreadyOverriden;
               });

  // BaseVirtualMethods.front()->getCanonicalDecl()

  // PureVirtualMethods = BaseVirtualMethods;

  return !PureVirtualMethods.empty();
}

Expected<Tweak::Effect> OverridePureVirtuals::apply(const Selection &Sel) {
  const auto &SM = Sel.AST->getSourceManager();
  // const auto &LangOpts = Sel.AST->getLangOpts();

  std::string Insertion;
  for (const auto *Method : PureVirtualMethods) {
    std::string MethodDecl = Method->getReturnType().getAsString();
    MethodDecl += " " + Method->getNameAsString() + "(";

    for (unsigned i = 0; i < Method->getNumParams(); ++i) {
      if (i > 0)
        MethodDecl += ", ";
      MethodDecl += Method->getParamDecl(i)->getType().getAsString();
      MethodDecl += " " + Method->getParamDecl(i)->getNameAsString();
    }

    MethodDecl += ")";
    if (Method->isConst())
      MethodDecl += " const";
    MethodDecl += " override;\n";

    Insertion += MethodDecl;
  }

  if (Insertion.empty())
    return Effect::mainFileEdit(SM, tooling::Replacements());
  // ClassDecl.getvoso
  SourceLocation InsertLoc =
      CurrentDecl->getBraceRange().getEnd().getLocWithOffset(-1);
  tooling::Replacement Repl(SM, InsertLoc, 0, "\n" + Insertion);
  return Effect::mainFileEdit(SM, tooling::Replacements(Repl));
}

} // namespace
} // namespace clangd
} // namespace clang
