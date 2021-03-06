//===--- AddUsing.cpp --------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AST.h"
#include "FindTarget.h"
#include "Logger.h"
#include "refactor/Tweak.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecursiveASTVisitor.h"

namespace clang {
namespace clangd {
namespace {

// Tweak for removing full namespace qualifier under cursor on DeclRefExpr and
// types and adding "using" statement instead.
//
// Only qualifiers that refer exclusively to namespaces (no record types) are
// supported. There is some guessing of appropriate place to insert the using
// declaration. If we find any existing usings, we insert it there. If not, we
// insert right after the inner-most relevant namespace declaration. If there is
// none, or there is, but it was declared via macro, we insert above the first
// top level decl.
//
// Currently this only removes qualifier from under the cursor. In the future,
// we should improve this to remove qualifier from all occurences of this
// symbol.
class AddUsing : public Tweak {
public:
  const char *id() const override;

  bool prepare(const Selection &Inputs) override;
  Expected<Effect> apply(const Selection &Inputs) override;
  std::string title() const override;
  Intent intent() const override { return Refactor; }

private:
  // The qualifier to remove. Set by prepare().
  NestedNameSpecifierLoc QualifierToRemove;
  // The name following QualifierToRemove. Set by prepare().
  llvm::StringRef Name;
};
REGISTER_TWEAK(AddUsing)

std::string AddUsing::title() const {
  return std::string(llvm::formatv(
      "Add using-declaration for {0} and remove qualifier.", Name));
}

// Locates all "using" statements relevant to SelectionDeclContext.
class UsingFinder : public RecursiveASTVisitor<UsingFinder> {
public:
  UsingFinder(std::vector<const UsingDecl *> &Results,
              const DeclContext *SelectionDeclContext, const SourceManager &SM)
      : Results(Results), SelectionDeclContext(SelectionDeclContext), SM(SM) {}

  bool VisitUsingDecl(UsingDecl *D) {
    auto Loc = D->getUsingLoc();
    if (SM.getFileID(Loc) != SM.getMainFileID()) {
      return true;
    }
    if (D->getDeclContext()->Encloses(SelectionDeclContext)) {
      Results.push_back(D);
    }
    return true;
  }

  bool TraverseDecl(Decl *Node) {
    // There is no need to go deeper into nodes that do not enclose selection,
    // since "using" there will not affect selection, nor would it make a good
    // insertion point.
    if (Node->getDeclContext()->Encloses(SelectionDeclContext)) {
      return RecursiveASTVisitor<UsingFinder>::TraverseDecl(Node);
    }
    return true;
  }

private:
  std::vector<const UsingDecl *> &Results;
  const DeclContext *SelectionDeclContext;
  const SourceManager &SM;
};

struct InsertionPointData {
  // Location to insert the "using" statement. If invalid then the statement
  // should not be inserted at all (it already exists).
  SourceLocation Loc;
  // Extra suffix to place after the "using" statement. Depending on what the
  // insertion point is anchored to, we may need one or more \n to ensure
  // proper formatting.
  std::string Suffix;
};

// Finds the best place to insert the "using" statement. Returns invalid
// SourceLocation if the "using" statement already exists.
//
// The insertion point might be a little awkward if the decl we're anchoring to
// has a comment in an unfortunate place (e.g. directly above function or using
// decl, or immediately following "namespace {". We should add some helpers for
// dealing with that and use them in other code modifications as well.
llvm::Expected<InsertionPointData>
findInsertionPoint(const Tweak::Selection &Inputs,
                   const NestedNameSpecifierLoc &QualifierToRemove,
                   const llvm::StringRef Name) {
  auto &SM = Inputs.AST->getSourceManager();

  // Search for all using decls that affect this point in file. We need this for
  // two reasons: to skip adding "using" if one already exists and to find best
  // place to add it, if it doesn't exist.
  SourceLocation LastUsingLoc;
  std::vector<const UsingDecl *> Usings;
  UsingFinder(Usings, &Inputs.ASTSelection.commonAncestor()->getDeclContext(),
              SM)
      .TraverseAST(Inputs.AST->getASTContext());

  for (auto &U : Usings) {
    if (SM.isBeforeInTranslationUnit(Inputs.Cursor, U->getUsingLoc()))
      // "Usings" is sorted, so we're done.
      break;
    if (U->getQualifier()->getAsNamespace()->getCanonicalDecl() ==
            QualifierToRemove.getNestedNameSpecifier()
                ->getAsNamespace()
                ->getCanonicalDecl() &&
        U->getName() == Name) {
      return InsertionPointData();
    }
    // Insertion point will be before last UsingDecl that affects cursor
    // position. For most cases this should stick with the local convention of
    // add using inside or outside namespace.
    LastUsingLoc = U->getUsingLoc();
  }
  if (LastUsingLoc.isValid()) {
    InsertionPointData Out;
    Out.Loc = LastUsingLoc;
    return Out;
  }

  // No relevant "using" statements. Try the nearest namespace level.
  const auto *NS = Inputs.ASTSelection.commonAncestor()
                       ->getDeclContext()
                       .getEnclosingNamespaceContext();
  if (auto *ND = dyn_cast<NamespaceDecl>(NS)) {
    auto Toks = Inputs.AST->getTokens().expandedTokens(ND->getSourceRange());
    const auto *Tok = llvm::find_if(Toks, [](const syntax::Token &Tok) {
      return Tok.kind() == tok::l_brace;
    });
    if (Tok == Toks.end() || Tok->endLocation().isInvalid()) {
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "Namespace with no {");
    }
    if (!Tok->endLocation().isMacroID()) {
      InsertionPointData Out;
      Out.Loc = Tok->endLocation();
      Out.Suffix = "\n";
      return Out;
    }
  }
  // No using, no namespace, no idea where to insert. Try above the first
  // top level decl.
  auto TLDs = Inputs.AST->getLocalTopLevelDecls();
  if (TLDs.empty()) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "Cannot find place to insert \"using\"");
  }
  InsertionPointData Out;
  Out.Loc = SM.getExpansionLoc(TLDs[0]->getBeginLoc());
  Out.Suffix = "\n\n";
  return Out;
}

bool AddUsing::prepare(const Selection &Inputs) {
  auto &SM = Inputs.AST->getSourceManager();
  auto *Node = Inputs.ASTSelection.commonAncestor();
  if (Node == nullptr)
    return false;

  // If we're looking at a type or NestedNameSpecifier, walk up the tree until
  // we find the "main" node we care about, which would be ElaboratedTypeLoc or
  // DeclRefExpr.
  for (; Node->Parent; Node = Node->Parent) {
    if (Node->ASTNode.get<NestedNameSpecifierLoc>()) {
      continue;
    } else if (auto *T = Node->ASTNode.get<TypeLoc>()) {
      if (T->getAs<ElaboratedTypeLoc>()) {
        break;
      } else if (Node->Parent->ASTNode.get<TypeLoc>() ||
                 Node->Parent->ASTNode.get<NestedNameSpecifierLoc>()) {
        // Node is TypeLoc, but it's parent is either TypeLoc or
        // NestedNameSpecifier. In both cases, we want to go up, to find
        // the outermost TypeLoc.
        continue;
      }
    }
    break;
  }
  if (Node == nullptr)
    return false;

  if (auto *D = Node->ASTNode.get<DeclRefExpr>()) {
    QualifierToRemove = D->getQualifierLoc();
    Name = D->getDecl()->getName();
  } else if (auto *T = Node->ASTNode.get<TypeLoc>()) {
    if (auto E = T->getAs<ElaboratedTypeLoc>()) {
      QualifierToRemove = E.getQualifierLoc();
      Name =
          E.getType().getUnqualifiedType().getBaseTypeIdentifier()->getName();
    }
  }

  // FIXME: This only supports removing qualifiers that are made up of just
  // namespace names. If qualifier contains a type, we could take the longest
  // namespace prefix and remove that.
  if (!QualifierToRemove.hasQualifier() ||
      !QualifierToRemove.getNestedNameSpecifier()->getAsNamespace() ||
      Name.empty()) {
    return false;
  }

  // Macros are difficult. We only want to offer code action when what's spelled
  // under the cursor is a namespace qualifier. If it's a macro that expands to
  // a qualifier, user would not know what code action will actually change.
  // On the other hand, if the qualifier is part of the macro argument, we
  // should still support that.
  if (SM.isMacroBodyExpansion(QualifierToRemove.getBeginLoc()) ||
      !SM.isWrittenInSameFile(QualifierToRemove.getBeginLoc(),
                              QualifierToRemove.getEndLoc())) {
    return false;
  }

  return true;
}

Expected<Tweak::Effect> AddUsing::apply(const Selection &Inputs) {
  auto &SM = Inputs.AST->getSourceManager();
  auto &TB = Inputs.AST->getTokens();

  // Determine the length of the qualifier under the cursor, then remove it.
  auto SpelledTokens = TB.spelledForExpanded(
      TB.expandedTokens(QualifierToRemove.getSourceRange()));
  if (!SpelledTokens) {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "Could not determine length of the qualifier");
  }
  unsigned Length =
      syntax::Token::range(SM, SpelledTokens->front(), SpelledTokens->back())
          .length();
  tooling::Replacements R;
  if (auto Err = R.add(tooling::Replacement(
          SM, SpelledTokens->front().location(), Length, ""))) {
    return std::move(Err);
  }

  auto InsertionPoint = findInsertionPoint(Inputs, QualifierToRemove, Name);
  if (!InsertionPoint) {
    return InsertionPoint.takeError();
  }

  if (InsertionPoint->Loc.isValid()) {
    // Add the using statement at appropriate location.
    std::string UsingText;
    llvm::raw_string_ostream UsingTextStream(UsingText);
    UsingTextStream << "using ";
    QualifierToRemove.getNestedNameSpecifier()->print(
        UsingTextStream, Inputs.AST->getASTContext().getPrintingPolicy());
    UsingTextStream << Name << ";" << InsertionPoint->Suffix;

    assert(SM.getFileID(InsertionPoint->Loc) == SM.getMainFileID());
    if (auto Err = R.add(tooling::Replacement(SM, InsertionPoint->Loc, 0,
                                              UsingTextStream.str()))) {
      return std::move(Err);
    }
  }

  return Effect::mainFileEdit(Inputs.AST->getASTContext().getSourceManager(),
                              std::move(R));
}

} // namespace
} // namespace clangd
} // namespace clang
