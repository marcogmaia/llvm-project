//===--- ExpandDeducedType.cpp -----------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "refactor/Tweak.h"

#include "AST.h"
#include "support/Logger.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/LLVM.h"
#include "clang/Sema/HeuristicResolver.h"
#include <optional>
#include <string>

namespace clang {
namespace clangd {
namespace {

std::string injectQualifier(llvm::StringRef TypeString,
                            llvm::StringRef UnqualifiedName,
                            llvm::StringRef QualifiedName) {
  llvm::StringRef ShortName = TypeString.rtrim();

  // Find the name in the string.
  // We need to be careful not to match substrings (e.g. "Names" vs "Name").
  // We also need to handle qualifiers (const/volatile) and pointers/refs.
  size_t Pos = ShortName.find(UnqualifiedName);
  if (Pos == llvm::StringRef::npos)
    return TypeString.str();

  // Check if it's a whole word match.
  bool StartOk =
      (Pos == 0) || !isAsciiIdentifierContinue(
                        static_cast<unsigned char>(ShortName[Pos - 1]));
  bool EndOk = (Pos + UnqualifiedName.size() == ShortName.size()) ||
               !isAsciiIdentifierContinue(static_cast<unsigned char>(
                   ShortName[Pos + UnqualifiedName.size()]));

  if (!StartOk || !EndOk)
    return TypeString.str();

  if (QualifiedName.empty() || QualifiedName == UnqualifiedName)
    return TypeString.str();

  // We have a qualified name, check if the current name is already
  // fully qualified.
  // We do this by checking if the text *before* the match ends with
  // the qualification prefix.
  // e.g. QualifiedName = "ns::Class::Nested", UnqualifiedName = "Nested"
  // ExpectedPrefix = "ns::Class::"
  // ShortName = "ns::Class::Nested" -> Pos of Nested is 11.
  // ShortName.substr(0, 11) is "ns::Class::", which matches.

  // Handle "::" global scope properly
  llvm::StringRef ExpectedPrefix = QualifiedName;
  if (ExpectedPrefix.ends_with(UnqualifiedName))
    ExpectedPrefix = ExpectedPrefix.drop_back(UnqualifiedName.size());

  if (ShortName.substr(0, Pos).ends_with(ExpectedPrefix))
    return TypeString.str();

  return (ShortName.take_front(Pos) + ExpectedPrefix +
          ShortName.drop_front(Pos))
      .str();
}

// Determines the string representation of the deduced type, including
// qualification and handling of declarators.
llvm::Expected<std::string>
computeDeducedTypeName(ASTContext &Ctx, const HeuristicResolver *Resolver,
                       const DeclContext &CurContext,
                       const SelectionTree::Node *Node, SourceLocation Loc,
                       QualType DeducedType) {
  // Some types aren't written as single chunks of text, e.g:
  //   auto fptr = &func; // auto is void(*)()
  // ==>
  //   void (*fptr)() = &func;
  // Replacing these requires examining the declarator, we don't support it yet.
  std::string PrettyDeclarator =
      printType(DeducedType, CurContext, "DECLARATOR_ID");

  const NamedDecl *ND = Resolver->resolveTypeToTagDecl(DeducedType);
  if (!ND || !ND->getIdentifier()) {
    llvm::StringRef PrettyTypeName = PrettyDeclarator;
    if (!PrettyTypeName.consume_back("DECLARATOR_ID"))
      return error("Could not expand type that isn't a simple string");
    return PrettyTypeName.rtrim().str();
  }

  std::string QualifiedName =
      getQualification(Ctx, Resolver, &CurContext, Node->ASTNode, Loc, ND);
  if (QualifiedName != ND->getNameAsString()) {
    llvm::StringRef ShortName = PrettyDeclarator;
    if (ShortName.consume_back("DECLARATOR_ID")) {
      std::string NewDeclarator =
          injectQualifier(ShortName, ND->getName(), QualifiedName);
      if (NewDeclarator != ShortName)
        PrettyDeclarator = NewDeclarator + " DECLARATOR_ID";
    }
  }

  llvm::StringRef PrettyTypeName = PrettyDeclarator;
  if (!PrettyTypeName.consume_back("DECLARATOR_ID"))
    return error("Could not expand type that isn't a simple string");
  return PrettyTypeName.rtrim().str();
}

/// Expand the "auto" type to the derived type
/// Before:
///    auto x = Something();
///    ^^^^
/// After:
///    MyClass x = Something();
///    ^^^^^^^
/// Expand `decltype(expr)` to the deduced type
/// Before:
///   decltype(0) i;
///   ^^^^^^^^^^^
/// After:
///   int i;
///   ^^^
class ExpandDeducedType : public Tweak {
public:
  const char *id() const final;
  llvm::StringLiteral kind() const override {
    return CodeAction::REFACTOR_KIND;
  }
  bool prepare(const Selection &Inputs) override;
  Expected<Effect> apply(const Selection &Inputs) override;
  std::string title() const override;

private:
  SourceRange Range;
};

REGISTER_TWEAK(ExpandDeducedType)

std::string ExpandDeducedType::title() const {
  return "Replace with deduced type";
}

// Structured bindings must use auto, e.g. `const auto& [a,b,c] = ...;`.
// Return whether N (an AutoTypeLoc) is such an auto that must not be expanded.
bool isStructuredBindingType(const SelectionTree::Node *N) {
  // Walk up the TypeLoc chain, because auto may be qualified.
  while (N && N->ASTNode.get<TypeLoc>())
    N = N->Parent;
  // The relevant type is the only direct type child of a Decomposition.
  return N && N->ASTNode.get<DecompositionDecl>();
}

bool isLambda(QualType QT) {
  if (!QT.isNull())
    if (const auto *RD = QT->getAsRecordDecl())
      return RD->isLambda();
  return false;
}

// Returns true iff Node is a lambda, and thus should not be expanded. Loc is
// the location of the auto type.
bool isDeducedAsLambda(const SelectionTree::Node *Node, SourceLocation Loc) {
  // getDeducedType() does a traversal, which we want to avoid in prepare().
  // But at least check this isn't auto x = []{...};, which can't ever be
  // expanded.
  // (It would be nice if we had an efficient getDeducedType(), instead).
  for (const auto *It = Node; It; It = It->Parent) {
    if (const auto *DD = It->ASTNode.get<DeclaratorDecl>()) {
      if (DD->getTypeSourceInfo() &&
          DD->getTypeSourceInfo()->getTypeLoc().getBeginLoc() == Loc &&
          isLambda(DD->getType()))
        return true;
    }
  }
  return false;
}

// Returns true iff "auto" in Node is really part of the template parameter,
// which we cannot expand.
bool isTemplateParam(const SelectionTree::Node *Node) {
  if (Node->Parent)
    if (Node->Parent->ASTNode.get<NonTypeTemplateParmDecl>())
      return true;
  return false;
}

bool ExpandDeducedType::prepare(const Selection &Inputs) {
  if (auto *Node = Inputs.ASTSelection.commonAncestor()) {
    if (auto *TypeNode = Node->ASTNode.get<TypeLoc>()) {
      if (const AutoTypeLoc Result = TypeNode->getAs<AutoTypeLoc>()) {
        if (!isStructuredBindingType(Node) &&
            !isDeducedAsLambda(Node, Result.getBeginLoc()) &&
            !isTemplateParam(Node))
          Range = Result.getSourceRange();
      }
      if (auto TTPAuto = TypeNode->getAs<TemplateTypeParmTypeLoc>()) {
        // We exclude concept constraints for now, as the SourceRange is wrong.
        // void foo(C auto x) {};
        //            ^^^^
        // TTPAuto->getSourceRange only covers "auto", not "C auto".
        if (TTPAuto.getDecl()->isImplicit() &&
            !TTPAuto.getDecl()->hasTypeConstraint())
          Range = TTPAuto.getSourceRange();
      }

      if (auto DTTL = TypeNode->getAs<DecltypeTypeLoc>()) {
        if (!isLambda(cast<DecltypeType>(DTTL.getType())->getUnderlyingType()))
          Range = DTTL.getSourceRange();
      }
    }
  }

  return Range.isValid();
}

Expected<Tweak::Effect> ExpandDeducedType::apply(const Selection &Inputs) {
  auto &SrcMgr = Inputs.AST->getSourceManager();

  std::optional<clang::QualType> DeducedType =
      getDeducedType(Inputs.AST->getASTContext(),
                     Inputs.AST->getHeuristicResolver(), Range.getBegin());

  // if we can't resolve the type, return an error message
  if (DeducedType == std::nullopt || (*DeducedType)->isUndeducedAutoType())
    return error("Could not deduce type for 'auto' type");

  // we shouldn't replace a dependent type which is likely not to print
  // usefully, e.g.
  //   template <class T>
  //   struct Foobar {
  //     decltype(T{}) foobar;
  //     ^^^^^^^^^^^^^ would turn out to be `<dependent-type>`
  //   };
  if ((*DeducedType)->isDependentType())
    return error("Could not expand a dependent type");

  const DeclContext &CurContext =
      Inputs.ASTSelection.commonAncestor()->getDeclContext();

  llvm::Expected<std::string> PrettyTypeName = computeDeducedTypeName(
      Inputs.AST->getASTContext(), Inputs.AST->getHeuristicResolver(),
      CurContext, Inputs.ASTSelection.commonAncestor(), Range.getBegin(),
      *DeducedType);

  if (!PrettyTypeName)
    return PrettyTypeName.takeError();

  tooling::Replacement Expansion(SrcMgr, CharSourceRange(Range, true),
                                 *PrettyTypeName);

  return Effect::mainFileEdit(SrcMgr, tooling::Replacements(Expansion));
}

} // namespace
} // namespace clangd
} // namespace clang
