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
#include "llvm/ADT/DenseSet.h"
#include <optional>
#include <string>

namespace clang {
namespace clangd {
namespace {

// Copied from AST.cpp since it's not public.
llvm::DenseSet<const NamespaceDecl *>
getUsingNamespaceDirectives(const DeclContext *DestContext,
                            SourceLocation Until) {
  const auto &SM = DestContext->getParentASTContext().getSourceManager();
  llvm::DenseSet<const NamespaceDecl *> VisibleNamespaceDecls;
  for (const auto *DC = DestContext; DC; DC = DC->getLookupParent()) {
    for (const auto *D : DC->decls()) {
      if (!SM.isWrittenInSameFile(D->getLocation(), Until) ||
          !SM.isBeforeInTranslationUnit(D->getLocation(), Until))
        continue;
      if (auto *UDD = llvm::dyn_cast<UsingDirectiveDecl>(D))
        VisibleNamespaceDecls.insert(
            UDD->getNominatedNamespace()->getCanonicalDecl());
    }
  }
  return VisibleNamespaceDecls;
}

const NamedDecl *resolveTagOrTemplateDecl(QualType Type) {
  if (const auto *TT = Type->getAs<TagType>())
    return TT->getDecl();
  if (const auto *TST = Type->getAs<TemplateSpecializationType>())
    return TST->getTemplateName().getAsTemplateDecl();
  return nullptr;
}

struct DeducedTypeVisitor {
  enum class QualificationStrategy { Prefix, Full };

  // Checks if the 'Name' is defined in 'Context' and refers to something other
  // than 'Target'.
  static bool hasNameCollision(const DeclContext *Context,
                               const NamedDecl *Target) {
    for (const auto *D : Context->lookup(Target->getDeclName())) {
      if (D != Target && D->getCanonicalDecl() != Target->getCanonicalDecl())
        return true;
    }
    return false;
  }

  // Check if any declaration in the DeclStmt collides with Target.
  static bool shadowsInDeclStmt(const DeclStmt *DS, const NamedDecl *Target) {
    for (const Decl *D : DS->decls()) {
      if (const auto *NdLocal = dyn_cast<NamedDecl>(D)) {
        if (NdLocal->getDeclName() == Target->getDeclName())
          return true;
      }
    }
    return false;
  }

  // Check for shadowing within a CompoundStmt up to the current node.
  static bool shadowsInCompoundStmt(const CompoundStmt *CS,
                                    const Stmt *CurrentNode,
                                    const NamedDecl *Target) {
    for (const Stmt *S : CS->body()) {
      if (S == CurrentNode)
        break;
      if (const auto *DS = dyn_cast<DeclStmt>(S)) {
        if (shadowsInDeclStmt(DS, Target))
          return true;
      }
    }
    return false;
  }

  // Check if function parameters shadow the target.
  static bool shadowsInFunctionDecl(const FunctionDecl *FD,
                                    const NamedDecl *Target) {
    for (const auto *P : FD->parameters()) {
      if (P->getDeclName() == Target->getDeclName())
        return true;
    }
    return false;
  }

  // Checks if 'Target' is shadowed by a local declaration in the scope chain
  // starting from 'Node' (in 'SelectionTree') up to the 'CurContext'.
  static bool isShadowed(const NamedDecl *Target, const DeclContext *CurContext,
                         const SelectionTree::Node *Node) {
    // Check for shadowing in the current DeclContext chain (up to TU).
    for (const DeclContext *DC = CurContext; DC && !DC->isTranslationUnit();
         DC = DC->getLookupParent()) {
      if (hasNameCollision(DC, Target))
        return true;
    }

    // Check for shadowing in local scopes (function bodies, blocks)
    // which are not captured by DeclContext lookup.
    for (const SelectionTree::Node *Parent = Node->Parent; Parent;
         Node = Parent, Parent = Parent->Parent) {
      if (const auto *CS = Parent->ASTNode.get<CompoundStmt>()) {
        if (shadowsInCompoundStmt(CS, Node->ASTNode.get<Stmt>(), Target))
          return true;
      } else if (const auto *FD = Parent->ASTNode.get<FunctionDecl>()) {
        if (shadowsInFunctionDecl(FD, Target))
          return true;
      }
    }
    return false;
  }

  // Determines the qualification strategy for 'ND' at 'Loc'.
  static QualificationStrategy
  computeStrategy(ASTContext &Context, const DeclContext *CurContext,
                  const SelectionTree::Node *Node, SourceLocation Loc,
                  const NamedDecl *ND, std::string &Prefix) {
    Prefix = getQualification(Context, CurContext, Loc, ND);

    // If Qualification is empty, we must ensure that the simple name is NOT
    // ambiguous in the current context (e.g. via 'using namespace').
    if (Prefix.empty()) {
      bool IsAmbiguous = false;
      // Check for ambiguity with global scope
      const auto *TU = Context.getTranslationUnitDecl();
      if (hasNameCollision(TU, ND))
        IsAmbiguous = true;

      // Check for shadowing in the current scope chain
      if (!IsAmbiguous && isShadowed(ND, CurContext, Node))
        IsAmbiguous = true;

      // Check visible namespaces
      if (!IsAmbiguous) {
        auto VisibleNS = getUsingNamespaceDirectives(CurContext, Loc);
        for (const auto *NS : VisibleNS) {
          if (hasNameCollision(NS, ND)) {
            IsAmbiguous = true;
            break;
          }
        }
      }

      if (IsAmbiguous)
        return QualificationStrategy::Full;
    }
    return QualificationStrategy::Prefix;
  }

  static std::string getQualifiedName(ASTContext &Context,
                                      const DeclContext *CurContext,
                                      const SelectionTree::Node *Node,
                                      SourceLocation Loc, const NamedDecl *ND) {
    std::string Prefix;
    QualificationStrategy Strategy =
        computeStrategy(Context, CurContext, Node, Loc, ND, Prefix);

    if (Strategy == QualificationStrategy::Full) {
      std::string FQName;
      llvm::raw_string_ostream OS(FQName);
      ND->printQualifiedName(OS);
      if (ND->getDeclContext()->isTranslationUnit())
        return "::" + OS.str();
      return OS.str();
    }
    return Prefix + ND->getNameAsString();
  }
};

std::string injectQualifier(llvm::StringRef TypeString,
                            llvm::StringRef UnqualifiedName,
                            llvm::StringRef QualifiedName) {
  llvm::StringRef ShortName = TypeString;
  ShortName = ShortName.rtrim();

  // Find the name in the string.
  // We need to be careful not to match substrings (e.g. "Names" vs "Name").
  // We also need to handle qualifiers (const/volatile) and pointers/refs.
  size_t Pos = ShortName.find(UnqualifiedName);
  if (Pos != llvm::StringRef::npos) {
    // Check if it's a whole word match.
    bool StartOk =
        (Pos == 0) || !isAsciiIdentifierContinue(
                          static_cast<unsigned char>(ShortName[Pos - 1]));
    bool EndOk = (Pos + UnqualifiedName.size() == ShortName.size()) ||
                 !isAsciiIdentifierContinue(static_cast<unsigned char>(
                     ShortName[Pos + UnqualifiedName.size()]));

    if (StartOk && EndOk) {
      if (!QualifiedName.empty() && QualifiedName != UnqualifiedName) {
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

        if (!ShortName.substr(0, Pos).ends_with(ExpectedPrefix)) {
          return (ShortName.take_front(Pos) + ExpectedPrefix +
                  ShortName.drop_front(Pos))
              .str();
        }
      }
    }
  }
  return TypeString.str();
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

  // Some types aren't written as single chunks of text, e.g:
  //   auto fptr = &func; // auto is void(*)()
  // ==>
  //   void (*fptr)() = &func;
  // Replacing these requires examining the declarator, we don't support it yet.
  const DeclContext &CurContext =
      Inputs.ASTSelection.commonAncestor()->getDeclContext();

  std::string PrettyDeclarator =
      printType(*DeducedType, CurContext, "DECLARATOR_ID");

  // If the deduced type is a simple record/enum or a template specialization,
  // we can try to improve the qualification if it's currently ambiguous.
  if (const NamedDecl *ND = resolveTagOrTemplateDecl(*DeducedType)) {
    if (ND->getIdentifier()) {
      std::string QualifiedName = DeducedTypeVisitor::getQualifiedName(
          Inputs.AST->getASTContext(), &CurContext,
          Inputs.ASTSelection.commonAncestor(), Range.getBegin(), ND);

      if (QualifiedName != ND->getNameAsString()) {
        llvm::StringRef ShortName = PrettyDeclarator;
        if (ShortName.consume_back("DECLARATOR_ID")) {
          std::string NewDeclarator =
              injectQualifier(ShortName, ND->getName(), QualifiedName);
          if (NewDeclarator != ShortName)
            PrettyDeclarator = NewDeclarator + " DECLARATOR_ID";
        }
      }
    }
  }

  llvm::StringRef PrettyTypeName = PrettyDeclarator;
  if (!PrettyTypeName.consume_back("DECLARATOR_ID"))
    return error("Could not expand type that isn't a simple string");
  PrettyTypeName = PrettyTypeName.rtrim();

  tooling::Replacement Expansion(SrcMgr, CharSourceRange(Range, true),
                                 PrettyTypeName);

  return Effect::mainFileEdit(SrcMgr, tooling::Replacements(Expansion));
}

} // namespace
} // namespace clangd
} // namespace clang
